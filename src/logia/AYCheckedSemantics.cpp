#include "AYReflect.h"
#include "AYReflect/IReflect.h"
#include "AYReflect/ReflectRegistry.h"
#include "AYScript/logia/CheckedSemantics.h"
#include "AYScript/logia/SemanticAnalyzer.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_set>

namespace ayt::script::logia
{
namespace
{
std::string canonical(std::string t)
{
    if (t == "Int32" || t == "int32_t" || t == "Int64" || t == "int64_t")
        return "int";
    if (t == "Float32" || t == "Float64" || t == "double")
        return "float";
    if (t == "Bool" || t == "Boolean")
        return "bool";
    if (t == "std::string" || t == "String")
        return "string";
    return t;
}
bool durableType(const std::string& type, unsigned depth = 0)
{
    if (depth > 8) return false;
    if (type.starts_with("array<") && type.ends_with(">"))
        return durableType(type.substr(6, type.size() - 7), depth + 1);
    const auto value = canonical(type);
    return value == "int" || value == "float" || value == "bool" || value == "string" ||
           value == "vec2" || value == "vec3" || value == "quat";
}
bool numeric(const std::string &t)
{
    return t == "int" || t == "float";
}
bool arrayType(const std::string &t)
{
    return t.starts_with("array<") && t.ends_with('>');
}
std::string elementType(const std::string &t)
{
    return t.substr(6, t.size() - 7);
}
bool accepts(const std::string &target, const std::string &value)
{
    return target == value || (target == "float" && value == "int") ||
           (target == "table" && value == "table");
}
struct Symbol
{
    std::string type;
    const void *decl = nullptr;
    bool instanceState = false;
    bool immutable = false;
};
struct Checker
{
    const LogiaHostContext &ctx;
    std::string file;
    std::vector<LogiaDiagnostic> diagnostics;
    std::vector<std::unordered_map<std::string, Symbol>> scopes;
    std::unordered_map<std::string, FunctionDeclStmt *> functions;
    std::unordered_map<std::string, SignalDeclStmt *> signals;
    const ayt::reflect::ITypeInfo *host = nullptr;
    std::string returnType = "void";
    bool inHelper = false;
    std::unordered_map<std::string,std::vector<std::string>> events=checkedEventPayloads();
    std::unordered_set<std::string> projectAliases;
    bool sharedInitializer = false;
    std::unordered_set<int> lifecycleKinds;

    void error(SourceLocation loc, std::string message, ErrorCode code = ErrorCode::TypeMismatch)
    {
        if (loc.file.empty())
            loc.file = file;
        diagnostics.push_back(
            {DiagnosticSeverity::Error, code, std::move(message), std::move(loc), {}});
    }
    const ayt::reflect::ITypeInfo *lookupType(const std::string &type)
    {
        return ayt::reflect::TypeRegistryImpl::instance().findType(type.c_str());
    }
    bool known(const std::string &type)
    {
        if (arrayType(type))
            return known(elementType(type)) && elementType(type) != "void";
        return type == "void" || type == "int" || type == "float" || type == "bool" ||
               type == "string" || type == "Entity" || type == "Scene" || type == "table" || type == "vec2" ||
               type == "vec3" || type == "quat" || lookupType(type);
    }
    Symbol *symbol(const std::string &name)
    {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
        {
            auto entry = it->find(name);
            if (entry != it->end())
                return &entry->second;
        }
        return nullptr;
    }
    void declare(const std::string &name, std::string type, const void *decl, SourceLocation loc)
    {
        if (!known(type))
            error(loc, "unknown type '" + type + "'");
        if (name == "self" || functions.contains(name) || signals.contains(name))
            error(loc, "reserved or conflicting declaration '" + name + "'");
        if (!scopes.back().emplace(name, Symbol{std::move(type), decl}).second)
            error(loc, "duplicate declaration '" + name + "'");
    }
    void require(const std::string &expected, Expr *value, SourceLocation loc)
    {
        if (auto *type = lookupType(expected))
        {
            if (dynamic_cast<const ayt::reflect::IEnumTypeInfo *>(type))
            {
                auto actual = expr(value);
                if (actual != expected && actual != "int" && actual != "string")
                    error(loc, "enum values require an integer or registered item name");
                return;
            }
        }
        if (auto *literal = dynamic_cast<TableExpr *>(value))
        {
            if (arrayType(expected))
            {
                literal->checkedValueType = expected;
                for (auto &entry : literal->entries)
                {
                    if (entry.key)
                        error(loc, "array literals must use positional entries");
                    require(elementType(expected), entry.value.get(), entry.value->sourceLoc);
                }
                return;
            }
            if (auto *type = lookupType(expected); type && type->getFieldCount() > 0)
            {
                for (auto &entry : literal->entries)
                {
                    auto *key = dynamic_cast<IdentifierExpr *>(entry.key.get());
                    auto *field = key ? type->findField(key->name.c_str()) : nullptr;
                    if (!field || !ayt::reflect::isScriptWritable(field) ||
                        !ayt::reflect::isScriptReadable(field, type))
                        error(loc, "unknown or non-writable reflected literal field");
                    else
                        require(canonical(field->getType()->getName()), entry.value.get(),
                                entry.value->sourceLoc);
                }
                return;
            }
            if (auto *container =
                    dynamic_cast<const ayt::reflect::IContainerTypeInfo *>(lookupType(expected)))
            {
                for (auto &entry : literal->entries)
                {
                    if (entry.key)
                        error(loc, "container literals require positional entries");
                    if (container->getElementType())
                        require(canonical(container->getElementType()->getName()),
                                entry.value.get(), entry.value->sourceLoc);
                }
                return;
            }
            if (expected == "vec2" || expected == "vec3" || expected == "quat")
            {
                std::unordered_set<std::string> keys;
                for (auto &entry : literal->entries)
                {
                    auto *key = dynamic_cast<IdentifierExpr *>(entry.key.get());
                    if (!key || !keys.insert(key->name).second ||
                        (key->name != "x" && key->name != "y" &&
                         (key->name != "z" || expected == "vec2") &&
                         (key->name != "w" || expected != "quat")))
                        error(loc, "invalid vector or quaternion field");
                    require("float", entry.value.get(), entry.value->sourceLoc);
                }
                if (keys.size() != (expected == "vec2" ? 2u : expected == "vec3" ? 3u : 4u))
                    error(loc, "incomplete vector or quaternion literal");
                return;
            }
        }
        auto actual = expr(value);
        if (!actual.empty() && !accepts(expected, actual))
            error(loc, "expected " + expected + ", got " + actual);
    }
    std::string member(MemberExpr &m, bool writing = false)
    {
        if (auto *id = dynamic_cast<IdentifierExpr *>(m.object.get()); id && id->name == "self")
        {
            if (sharedInitializer)
                error(m.sourceLoc, "shared variable initializers cannot access self; use state or a lifecycle");
            if (!ctx.expectSelf)
            {
                error(m.sourceLoc, "this host has no self receiver");
                return {};
            }
            if (ctx.kind == LogiaHostKind::Actor)
            {
                if (m.member == "transform")
                    return "@transform";
                auto p = ctx.actorProperties.find(m.member);
                if (p == ctx.actorProperties.end())
                {
                    error(m.sourceLoc, "unknown Actor property '" + m.member + "'");
                    return {};
                }
                return canonical(p->second);
            }
        }
        auto parent = expr(m.object.get());
        if (parent == "@transform")
        {
            if (m.member == "position" || m.member == "scale")
                return "vec3";
            if (m.member == "rotation")
                return "quat";
        }
        if (parent == "@vec3" || parent == "vec2" || parent == "vec3" || parent == "quat")
        {
            if (m.member == "x" || m.member == "y" || (parent != "vec2" && m.member == "z") ||
                (parent == "quat" && m.member == "w"))
                return "float";
        }
        if (parent == "@time" && (m.member == "delta" || m.member == "total"))
            return "float";
        if (parent == "table")
            return "table"; // explicitly dynamic data, never a typed host field
        auto *type = m.object->resolvedType;
        if (!type && parent == "@self")
            type = host;
        if (!type && !parent.empty())
            type = lookupType(parent);
        auto *field = type ? type->findField(m.member.c_str()) : nullptr;
        if (!field)
        {
            // Author-visible normalized field names follow the same contract as legacy reflection.
            if (type)
                for (size_t i = 0; i < type->getFieldCount(); ++i)
                {
                    auto *candidate = type->getField(i);
                    std::string name = candidate->getName();
                    if (name.starts_with("m_") || name.starts_with("b_"))
                        name.erase(0, 2);
                    if (name == m.member)
                    {
                        field = candidate;
                        break;
                    }
                }
        }
        if (!field)
        {
            error(m.sourceLoc, "unknown field '" + m.member + "'");
            return {};
        }
        if (!ayt::reflect::isScriptReadable(field, type))
            error(m.sourceLoc, "field '" + m.member + "' is not ScriptVisible");
        if (writing && !ayt::reflect::isScriptWritable(field))
            error(m.sourceLoc, "field '" + m.member + "' is ScriptReadOnly");
        if (writing)
            for (Expr *parent = m.object.get(); parent;)
            {
                if (parent->resolvedField && !ayt::reflect::isScriptWritable(parent->resolvedField))
                    error(m.sourceLoc, "cannot write through a ScriptReadOnly parent");
                auto *member = dynamic_cast<MemberExpr *>(parent);
                parent = member ? member->object.get() : nullptr;
            }
        m.resolvedField = field;
        m.resolvedType = field->getType();
        return m.resolvedType ? canonical(m.resolvedType->getName()) : std::string{};
    }
    std::string call(CallExpr &c)
    {
        if (sharedInitializer)
            error(c.sourceLoc,
                  "shared variable initializers cannot call functions; use state or a lifecycle");
        auto checkArgs = [&](const std::vector<std::string> &params) {
            if (params.size() != c.args.size())
                error(c.sourceLoc, "wrong argument count");
            for (size_t i = 0; i < c.args.size(); ++i)
            {
                if (i < params.size())
                    require(params[i], c.args[i].get(), c.args[i]->sourceLoc);
                else
                    expr(c.args[i].get());
            }
        };
        if (auto *id = dynamic_cast<IdentifierExpr *>(c.callee.get()))
        {
            auto found = functions.find(id->name);
            if (found != functions.end())
            {
                if (found->second->asynchronous)
                    error(c.sourceLoc, "asynchronous functions must be started with tasks.start");
                id->resolvedDecl = found->second;
                id->resolvedFunction = found->second;
                std::vector<std::string> params;
                for (const auto &p : found->second->params)
                    params.push_back(canonical(p.typeName));
                checkArgs(params);
                return found->second->returnType.empty() ? "void"
                                                         : canonical(found->second->returnType);
            }
            if (id->name == "emit" || id->name == "connect")
            {
                if (!ctx.expectSelf)
                    error(c.sourceLoc, "signals require a self receiver");
                auto *literal =
                    c.args.empty() ? nullptr : dynamic_cast<LiteralExpr *>(c.args[0].get());
                auto *name = literal ? std::get_if<std::string>(&literal->value) : nullptr;
                auto signal = name ? signals.find(*name) : signals.end();
                if (signal == signals.end())
                {
                    error(c.sourceLoc, "unknown or non-literal signal");
                    return {};
                }
                if (id->name == "emit")
                {
                    c.ambientCall = CallExpr::AmbientCallKind::Emit;
                    if (c.args.size() != signal->second->params.size() + 1)
                        error(c.sourceLoc, "signal argument count mismatch");
                    for (size_t i = 1; i < c.args.size(); ++i)
                    {
                        if (i <= signal->second->params.size())
                            require(canonical(signal->second->params[i - 1].typeName),
                                    c.args[i].get(), c.args[i]->sourceLoc);
                        else
                            expr(c.args[i].get());
                    }
                    return "void";
                }
                c.ambientCall = CallExpr::AmbientCallKind::Connect;
                auto *handler =
                    c.args.size() == 2 ? dynamic_cast<IdentifierExpr *>(c.args[1].get()) : nullptr;
                auto fn = handler ? functions.find(handler->name) : functions.end();
                if (fn == functions.end())
                    error(c.sourceLoc, "connect requires a declared handler function");
                else
                {
                    handler->resolvedDecl = fn->second;
                    handler->resolvedFunction = fn->second;
                    if (fn->second->params.size() != signal->second->params.size())
                        error(c.sourceLoc, "handler parameter count mismatch");
                    else
                        for (size_t i = 0; i < fn->second->params.size(); ++i)
                            if (canonical(fn->second->params[i].typeName) !=
                                canonical(signal->second->params[i].typeName))
                                error(c.sourceLoc, "handler parameter type mismatch");
                    if (!fn->second->returnType.empty() && fn->second->returnType != "void")
                        error(c.sourceLoc, "signal handler must return void");
                    if (fn->second->asynchronous)
                        error(c.sourceLoc, "signal handlers cannot await; start a task from a synchronous handler");
                }
                return "int";
            }
            if (id->name == "disconnect")
            {
                if (!ctx.expectSelf)
                    error(c.sourceLoc, "disconnect requires a self receiver");
                checkArgs({"int"});
                c.ambientCall = CallExpr::AmbientCallKind::Disconnect;
                return "void";
            }
        }
        if (auto *m = dynamic_cast<MemberExpr *>(c.callee.get()))
        {
            auto *receiver = dynamic_cast<IdentifierExpr *>(m->object.get());
            if (receiver && receiver->name == "self" && host)
            {
                auto *method = host->findMethod(m->member.c_str());
                if (!method)
                {
                    error(c.sourceLoc, "unknown method '" + m->member + "'");
                    return {};
                }
                std::vector<std::string> params;
                for (size_t i = 0; i < method->getParamCount(); ++i)
                {
                    auto *t = method->getParamType(i);
                    params.push_back(t ? canonical(t->getName()) : "@unsupported");
                }
                checkArgs(params);
                c.resolvedMethod = method;
                c.resolvedMethodOwnerName = host->getName();
                c.resolvedType = method->getReturnType();
                return c.resolvedType ? canonical(c.resolvedType->getName()) : "void";
            }
            if (receiver)
            {
                const auto name = receiver->name + "." + m->member;
                if (auto provided = ctx.hostFunctions.find(name);
                    provided != ctx.hostFunctions.end())
                {
                    checkArgs(provided->second.parameters);
                    return provided->second.result;
                }
                if (name == "tasks.start")
                {
                    auto *handler = !c.args.empty()
                                        ? dynamic_cast<IdentifierExpr *>(c.args[0].get())
                                        : nullptr;
                    auto fn = handler ? functions.find(handler->name) : functions.end();
                    if (fn == functions.end() ||
                        (!fn->second->returnType.empty() && fn->second->returnType != "void"))
                        error(c.sourceLoc,
                              "tasks.start requires a declared void function followed by its arguments");
                    else {
                        handler->resolvedDecl = fn->second;
                        handler->resolvedFunction = fn->second;
                        if (c.args.size() != fn->second->params.size() + 1)
                            error(c.sourceLoc, "task argument count mismatch");
                        for (size_t i = 1; i < c.args.size(); ++i) {
                            if (i <= fn->second->params.size()) {
                                const auto type = canonical(fn->second->params[i - 1].typeName);
                                auto element = type;
                                while (arrayType(element)) element = elementType(element);
                                if (element == "Scene" || element == "Entity")
                                    error(c.args[i]->sourceLoc,
                                          "borrowed Scene/Entity pointers cannot be task arguments; consume them synchronously");
                                require(type,
                                        c.args[i].get(), c.args[i]->sourceLoc);
                            } else
                                expr(c.args[i].get());
                        }
                    }
                    return "int";
                }
                if (name == "array.length" || name == "array.push" || name == "array.pop")
                {
                    const auto type = c.args.empty() ? std::string{} : expr(c.args[0].get());
                    if (!arrayType(type))
                        error(c.sourceLoc, "array operation requires a typed array");
                    const auto count = name == "array.push" ? 2u : 1u;
                    if (c.args.size() != count)
                        error(c.sourceLoc, "wrong array argument count");
                    if (name == "array.push" && c.args.size() > 1 && arrayType(type))
                        require(elementType(type), c.args[1].get(), c.sourceLoc);
                    return name == "array.length" ? "int"
                           : name == "array.push" ? "void"
                           : arrayType(type)      ? elementType(type)
                                                  : "";
                }
                const auto &builtins = checkedBuiltinFunctions();
                auto builtin = builtins.find(name);
                if (builtin != builtins.end())
                {
                    if(name.starts_with("storage.") && !ctx.expectSelf)
                        error(c.sourceLoc,"storage requires a receiver host");
                    checkArgs(builtin->second.first);
                    return builtin->second.second;
                }
                if (name == "event.emit")
                {
                    if (c.args.empty())
                        error(c.sourceLoc, "event.emit requires alias");
                    else
                        require("string", c.args[0].get(), c.sourceLoc);
                    auto* literal=c.args.empty()?nullptr:dynamic_cast<LiteralExpr*>(c.args[0].get());
                    auto* alias=literal?std::get_if<std::string>(&literal->value):nullptr;
                    if(alias && (projectAliases.contains(*alias) || *alias=="logia_task_complete")) {
                        auto schema=events.find(*alias);
                        if(schema!=events.end()) {
                            if(c.args.size()!=schema->second.size()+1)error(c.sourceLoc,"project event payload count mismatch");
                            for(size_t i=0;i<schema->second.size() && i+1<c.args.size();++i)require(schema->second[i],c.args[i+1].get(),c.sourceLoc);
                        }
                    } else for (size_t i = 1; i < c.args.size(); ++i)expr(c.args[i].get());
                    return "bool";
                }
                if (name == "event.subscribe")
                {
                    if (c.args.size() != 2)
                        error(c.sourceLoc, "event.subscribe expects alias and handler");
                    if (!c.args.empty())
                        require("string", c.args[0].get(), c.sourceLoc);
                    auto *handler = c.args.size() > 1
                                        ? dynamic_cast<IdentifierExpr *>(c.args[1].get())
                                        : nullptr;
                    auto fn = handler ? functions.find(handler->name) : functions.end();
                    auto *literal =
                        c.args.empty() ? nullptr : dynamic_cast<LiteralExpr *>(c.args[0].get());
                    auto *alias = literal ? std::get_if<std::string>(&literal->value) : nullptr;
                    auto event = alias ? events.find(*alias) : events.end();
                    if (event == events.end())
                        error(c.sourceLoc, "event.subscribe requires a known literal alias");
                    if (fn == functions.end())
                        error(c.sourceLoc, "event handler must be a declared function");
                    else
                    {
                        handler->resolvedDecl = fn->second;
                        handler->resolvedFunction = fn->second;
                        if (fn->second->asynchronous ||
                            (!fn->second->returnType.empty() && fn->second->returnType != "void"))
                            error(c.sourceLoc, "event handler must be synchronous and void");
                        if (event != events.end())
                        {
                            if (fn->second->params.size() != event->second.size())
                                error(c.sourceLoc, "event handler parameter count mismatch");
                            else
                                for (size_t i = 0; i < event->second.size(); ++i)
                                    if (canonical(fn->second->params[i].typeName) !=
                                        event->second[i])
                                        error(c.sourceLoc, "event handler parameter type mismatch");
                        }
                    }
                    return "int";
                }
            }
        }
        error(c.sourceLoc, "unknown callable");
        for (auto &arg : c.args)
            expr(arg.get());
        return {};
    }
    std::string expr(Expr *e, bool writing = false, bool statement = false)
    {
        if (!e)
            return "void";
        if (auto *lit = dynamic_cast<LiteralExpr *>(e))
        {
            if (std::holds_alternative<int>(lit->value))
                return "int";
            if (std::holds_alternative<float>(lit->value))
                return "float";
            if (std::holds_alternative<bool>(lit->value))
                return "bool";
            if (std::holds_alternative<std::string>(lit->value))
                return "string";
            return "null";
        }
        if (auto *id = dynamic_cast<IdentifierExpr *>(e))
        {
            if (auto *s = symbol(id->name))
            {
                if(writing && s->immutable) error(id->sourceLoc,"asset references are immutable");
                if (sharedInitializer && s->instanceState)
                    error(id->sourceLoc, "shared variables cannot read instance state");
                id->resolvedDecl = s->decl;
                id->resolvedType = lookupType(s->type);
                id->resolvedInstanceState = s->instanceState;
                return s->type;
            }
            if (id->name == "self" && ctx.expectSelf)
            {
                if (sharedInitializer)
                    error(id->sourceLoc, "self is unavailable in shared initialization");
                id->resolvedType = host;
                return "@self";
            }
            if (functions.contains(id->name))
            {
                id->resolvedDecl = functions[id->name];
                id->resolvedFunction = functions[id->name];
                return "@function";
            }
            if (id->name == "time")
                return "@time";
            error(e->sourceLoc, "undeclared identifier '" + id->name + "'",
                  ErrorCode::UnknownIdentifier);
            return {};
        }
        if (auto *m = dynamic_cast<MemberExpr *>(e))
            return member(*m, writing);
        if (auto *c = dynamic_cast<CallExpr *>(e))
            return call(*c);
        if (auto *table = dynamic_cast<TableExpr *>(e))
        {
            for (auto &item : table->entries)
                expr(item.value.get());
            return "table";
        }
        if (auto *idx = dynamic_cast<IndexExpr *>(e))
        {
            auto object = expr(idx->object.get());
            if (arrayType(object))
            {
                require("int", idx->index.get(), idx->sourceLoc);
                return elementType(object);
            }
            if (auto *container = dynamic_cast<const ayt::reflect::IContainerTypeInfo *>(
                    idx->object->resolvedType))
            {
                require("int", idx->index.get(), idx->sourceLoc);
                if (writing)
                    error(idx->sourceLoc, "replace a reflected container as a whole; indexed "
                                          "writes affect only a snapshot");
                idx->resolvedType = container->getElementType();
                return idx->resolvedType ? canonical(idx->resolvedType->getName()) : "";
            }
            expr(idx->index.get());
            if (object != "table")
                error(e->sourceLoc, "indexing requires a table");
            return "table";
        }
        if (auto *u = dynamic_cast<UnaryExpr *>(e))
        {
            auto t = expr(u->operand.get());
            if (u->op.type == TokenType::Bang)
            {
                if (t != "bool")
                    error(e->sourceLoc, "! requires bool");
                return "bool";
            }
            if (!numeric(t))
                error(e->sourceLoc, "unary arithmetic requires numeric value");
            return t;
        }
        if (auto *b = dynamic_cast<BinaryExpr *>(e))
        {
            auto op = b->op.type;
            const bool assignment = op == TokenType::Equal || op == TokenType::PlusEqual ||
                                    op == TokenType::MinusEqual || op == TokenType::StarEqual ||
                                    op == TokenType::SlashEqual;
            if (assignment && !statement)
                error(e->sourceLoc, "assignment is only allowed as a statement");
            if (assignment && !dynamic_cast<IdentifierExpr *>(b->left.get()) &&
                !dynamic_cast<MemberExpr *>(b->left.get()) &&
                !dynamic_cast<IndexExpr *>(b->left.get()))
                error(e->sourceLoc, "invalid assignment target");
            auto left = expr(b->left.get(), assignment);
            auto right = expr(b->right.get());
            if (assignment)
            {
                if (op == TokenType::Equal)
                {
                    if(!left.empty())require(left, b->right.get(), b->sourceLoc);
                    return left;
                }
                if (!left.empty() && !right.empty() && !accepts(left, right))
                    error(e->sourceLoc, "cannot assign " + right + " to " + left);
                if (op != TokenType::Equal && (!numeric(left) || !numeric(right)))
                    error(e->sourceLoc, "compound assignment requires numeric values");
                return left;
            }
            if (op == TokenType::And || op == TokenType::Or)
            {
                if (left != "bool" || right != "bool")
                    error(e->sourceLoc, "logical operands must be bool");
                return "bool";
            }
            if (op == TokenType::EqualEqual || op == TokenType::BangEqual)
            {
                if (!accepts(left, right) && !accepts(right, left))
                    error(e->sourceLoc, "incompatible comparison operands");
                return "bool";
            }
            if (op == TokenType::Less || op == TokenType::LessEqual || op == TokenType::Greater ||
                op == TokenType::GreaterEqual)
            {
                if (!numeric(left) || !numeric(right))
                    error(e->sourceLoc, "ordered comparison requires numeric operands");
                return "bool";
            }
            if (!numeric(left) || !numeric(right))
                error(e->sourceLoc, "arithmetic requires numeric operands");
            return left == "float" || right == "float" || op == TokenType::Slash ? "float" : "int";
        }
        error(e->sourceLoc, "unsupported expression");
        return {};
    }
    bool body(std::vector<StmtPtr> &statements, bool newScope = true)
    {
        if (newScope)
            scopes.emplace_back();
        bool returns = false;
        for (auto &statement : statements)
        {
            if (!statement)
                continue;
            if (returns)
                error(statement->sourceLoc, "unreachable statement");
            returns = stmt(*statement) || returns;
        }
        if (newScope)
            scopes.pop_back();
        return returns;
    }
    bool stmt(Stmt &s)
    {
        if (auto *wait = dynamic_cast<AwaitStmt *>(&s))
        {
            if (!inHelper)
                error(wait->sourceLoc, "await is only allowed in task helper functions");
            auto *call = dynamic_cast<CallExpr *>(wait->operation.get());
            auto *name = call ? dynamic_cast<IdentifierExpr *>(call->callee.get()) : nullptr;
            const bool delay=name && name->name=="delay";
            const bool event=name && name->name=="wait_event";
            const bool join=name && name->name=="join";
            const bool timed=call && call->args.size()==2;
            bool valid=call && (delay || event || join) && (call->args.size()==1 || (timed && !delay));
            if(!valid)error(wait->sourceLoc,"await expects delay(seconds), wait_event(alias[,timeout]) or join(id[,timeout])");
            else {
                require(delay?"float":join?"int":"string",call->args[0].get(),wait->sourceLoc);
                if(timed)require("float",call->args[1].get(),wait->sourceLoc);
            }
            if (!wait->bindings.empty()) {
                std::vector<std::string> payload;
                if(join)payload={"bool","bool"};
                else if(event && valid) {
                    auto* literal=dynamic_cast<LiteralExpr*>(call->args[0].get());
                    auto* alias=literal?std::get_if<std::string>(&literal->value):nullptr;
                    auto schema=alias?events.find(*alias):events.end();
                    if(schema==events.end())error(wait->sourceLoc,"event payload await requires a known literal alias");
                    else if(std::find(schema->second.begin(),schema->second.end(),"Scene")!=schema->second.end())
                        error(wait->sourceLoc,"borrowed Scene payload requires synchronous event.subscribe");
                    else { payload=schema->second;if(timed)payload.insert(payload.begin(),"bool"); }
                } else error(wait->sourceLoc,"into requires event or join waiting");
                if(wait->bindings.size()!=payload.size())error(wait->sourceLoc,"await payload binding count mismatch");
                for(size_t i=0;i<wait->bindings.size() && i<payload.size();++i)
                    if(canonical(wait->bindings[i].typeName)!=payload[i])error(wait->bindings[i].sourceLoc,"await payload binding type mismatch");
                for(auto& binding:wait->bindings)declare(binding.name,canonical(binding.typeName),&binding,binding.sourceLoc);
            }
            return false;
        }
        if (auto *var = dynamic_cast<VarDeclStmt *>(&s))
        {
            auto type = canonical(var->typeName);
            if (type == "void")
                error(var->sourceLoc, "variables cannot have void type");
            if (var->initializer)
                require(type, var->initializer.get(), var->sourceLoc);
            declare(var->name, type, var, var->sourceLoc);
            if(auto* entry=symbol(var->name)) entry->immutable=var->resourceReference;
            var->resolvedType = lookupType(type);
            if (var->instanceState)
            {
                if (!ctx.expectSelf)
                    error(var->sourceLoc, "state requires a receiver");
                if (auto *entry = symbol(var->name))
                    entry->instanceState = true;
            }
            if (var->persistent && !durableType(type))
                error(var->sourceLoc, "persist requires scalar, vector or bounded nested typed array data");
        }
        else if (auto *expression = dynamic_cast<ExprStmt *>(&s))
        {
            auto *binary = dynamic_cast<BinaryExpr *>(expression->expr.get());
            const bool assignment = binary && (binary->op.type == TokenType::Equal ||
                                               binary->op.type == TokenType::PlusEqual ||
                                               binary->op.type == TokenType::MinusEqual ||
                                               binary->op.type == TokenType::StarEqual ||
                                               binary->op.type == TokenType::SlashEqual);
            if (!assignment && !dynamic_cast<CallExpr *>(expression->expr.get()))
                error(s.sourceLoc, "expression statements must be calls or assignments");
            expr(expression->expr.get(), false, true);
        }
        else if (auto *ret = dynamic_cast<ReturnStmt *>(&s))
        {
            require(returnType, ret->value.get(), ret->sourceLoc);
            return true;
        }
        else if (auto *branch = dynamic_cast<IfStmt *>(&s))
        {
            require("bool", branch->condition.get(), branch->sourceLoc);
            bool thenReturns = body(branch->thenBranch), elseReturns = body(branch->elseBranch);
            return thenReturns && elseReturns && !branch->elseBranch.empty();
        }
        else if (auto *loop = dynamic_cast<WhileStmt *>(&s))
        {
            require("bool", loop->condition.get(), loop->sourceLoc);
            body(loop->body);
        }
        else if (auto *loop = dynamic_cast<ForStmt *>(&s))
        {
            if (loop->start)
                require("int", loop->start.get(), loop->sourceLoc);
            require("int", loop->bound.get(), loop->sourceLoc);
            if (loop->step)
            {
                require("int", loop->step.get(), loop->sourceLoc);
                auto *literal = dynamic_cast<LiteralExpr *>(loop->step.get());
                if (!literal || !std::get_if<int>(&literal->value) ||
                    std::get<int>(literal->value) <= 0)
                    error(loop->sourceLoc, "loop step must be a positive integer literal");
            }
            scopes.emplace_back();
            declare(loop->counterName, "int", nullptr, loop->sourceLoc);
            body(loop->body);
            scopes.pop_back();
        }
        else if (auto *block = dynamic_cast<BlockStmt *>(&s))
            body(block->body);
        return false;
    }
    void function(const std::vector<Param> &params, std::vector<StmtPtr> &statements,
                  std::string result, SourceLocation loc)
    {
        scopes.emplace_back();
        for (const auto &p : params)
            declare(p.name, canonical(p.typeName), &p, loc);
        returnType = result;
        if (!known(result))
            error(loc, "unknown return type '" + result + "'");
        if (!body(statements, false) && result != "void")
            error(loc, "function can fall through without returning " + result);
        scopes.pop_back();
    }
    void script(ScriptDecl &s)
    {
        auto registerSchema=[&](const std::string& name,const std::vector<std::string>& types,SourceLocation loc,bool source) {
            bool valid=!name.empty() && name.size()<=128 && (std::isalpha(static_cast<unsigned char>(name[0]))||name[0]=='_');
            for(char c:name)valid&=std::isalnum(static_cast<unsigned char>(c))||c=='_'||c=='.';
            if(!valid || types.size()>16)error(loc,"invalid project event alias or payload limit");
            for(const auto& type:types)if(type!="int" && type!="float" && type!="bool" && type!="string")error(loc,"project events require scalar payload types");
            auto builtin=checkedEventPayloads().find(name);
            if(builtin!=checkedEventPayloads().end() && (source || builtin->second!=types))error(loc,"project event alias conflicts with a builtin");
            auto old=events.find(name);if(old!=events.end() && old->second!=types)error(loc,"project event schema conflicts with the live project contract");
            if(builtin==checkedEventPayloads().end() && projectAliases.size()>=128 && !projectAliases.contains(name))error(loc,"project event schema limit exceeded");
            events[name]=types;if(builtin==checkedEventPayloads().end())projectAliases.insert(name);
        };
        for(const auto& [name,types]:ctx.projectEventSchemas)registerSchema(name,types,s.sourceLoc,false);
        std::unordered_set<std::string> declared;
        for(const auto& item:s.members)if(auto* declaration=dynamic_cast<ProjectEventDeclStmt*>(item.get())) {
            if(!declared.insert(declaration->name).second)error(declaration->sourceLoc,"duplicate project event declaration");
            std::vector<std::string> types;for(const auto& param:declaration->params)types.push_back(canonical(param.typeName));
            registerSchema(declaration->name,types,declaration->sourceLoc,true);
        }
        scopes.clear();
        scopes.emplace_back();
        functions.clear();
        signals.clear();
        lifecycleKinds.clear();
        host = ctx.hostType ? ctx.hostType : lookupType(s.name);
        if (ctx.kind == LogiaHostKind::Component && ctx.strictInheritance && ctx.hostType)
        {
            auto *declared = lookupType(s.name);
            if (!declared || !ayt::reflect::isDerivedFrom(declared, ctx.hostType))
                error(s.sourceLoc, "script host violates strict inheritance");
        }
        s.hostTypeName = host ? host->getName() : s.name;
        for (auto &item : s.members)
        {
            if (auto *fn = dynamic_cast<FunctionDeclStmt *>(item.get()))
            {
                if (!functions.emplace(fn->name, fn).second)
                    error(fn->sourceLoc, "duplicate function '" + fn->name + "'");
                std::function<bool(const std::vector<StmtPtr> &)> hasAwait = [&](const auto &body) {
                    for (const auto &statement : body)
                    {
                        if (dynamic_cast<AwaitStmt *>(statement.get()))
                            return true;
                        if (auto *branch = dynamic_cast<IfStmt *>(statement.get());
                            branch &&
                            (hasAwait(branch->thenBranch) || hasAwait(branch->elseBranch)))
                            return true;
                        if (auto *loop = dynamic_cast<WhileStmt *>(statement.get());
                            loop && hasAwait(loop->body))
                            return true;
                        if (auto *loop = dynamic_cast<ForStmt *>(statement.get());
                            loop && hasAwait(loop->body))
                            return true;
                        if (auto *block = dynamic_cast<BlockStmt *>(statement.get());
                            block && hasAwait(block->body))
                            return true;
                    }
                    return false;
                };
                fn->asynchronous = hasAwait(fn->body);
            }
            if (auto *sig = dynamic_cast<SignalDeclStmt *>(item.get()))
            {
                if (!signals.emplace(sig->name, sig).second)
                    error(sig->sourceLoc, "duplicate signal '" + sig->name + "'");
                for (const auto &p : sig->params)
                    if (!known(p.typeName))
                        error(sig->sourceLoc, "unknown signal parameter type");
            }
        }
        for (auto &item : s.members)
            if (auto *var = dynamic_cast<VarDeclStmt *>(item.get()))
            {
                sharedInitializer = !var->instanceState;
                stmt(*var);
                sharedInitializer = false;
            }
        for (auto &item : s.members)
        {
            if (auto *fn = dynamic_cast<FunctionDeclStmt *>(item.get()))
            {
                inHelper = !fn->lifecycleHook;
                function(fn->params, fn->body,
                         fn->returnType.empty() ? "void" : canonical(fn->returnType),
                         fn->sourceLoc);
                inHelper = false;
                if(fn->name == "migrate") {
                    if(fn->params.size()!=2 || canonical(fn->params[0].typeName)!="int" ||
                       canonical(fn->params[1].typeName)!="table" || canonical(fn->returnType)!="table")
                        error(fn->sourceLoc,"migrate must have (from:int,data:table):table signature");
                    std::function<void(Expr*)> pure;
                    pure=[&](Expr* value) {
                        if(!value) return;
                        if(auto* id=dynamic_cast<IdentifierExpr*>(value); id &&
                           (id->name=="self" || id->name=="time" || id->resolvedInstanceState ||
                            (symbol(id->name) && symbol(id->name)->decl==id->resolvedDecl)))
                            error(id->sourceLoc,"migration cannot access self or module state");
                        if(auto* call=dynamic_cast<CallExpr*>(value)) {
                            auto* member=dynamic_cast<MemberExpr*>(call->callee.get());
                            auto* root=member ? dynamic_cast<IdentifierExpr*>(member->object.get()) : nullptr;
                            if(!root || (root->name!="vec2" && root->name!="vec3" && root->name!="quat" && root->name!="array"))
                                error(call->sourceLoc,"migration calls must be pure math/array operations");
                            for(auto& arg:call->args) pure(arg.get());
                        }
                        if(auto* binary=dynamic_cast<BinaryExpr*>(value)){pure(binary->left.get());pure(binary->right.get());}
                        if(auto* unary=dynamic_cast<UnaryExpr*>(value))pure(unary->operand.get());
                        if(auto* member=dynamic_cast<MemberExpr*>(value))pure(member->object.get());
                        if(auto* index=dynamic_cast<IndexExpr*>(value)){pure(index->object.get());pure(index->index.get());}
                        if(auto* table=dynamic_cast<TableExpr*>(value))for(auto& entry:table->entries){
                            if(!dynamic_cast<IdentifierExpr*>(entry.key.get()))pure(entry.key.get());
                            pure(entry.value.get());
                        }
                    };
                    std::function<void(const std::vector<StmtPtr>&)> visit;
                    visit=[&](const auto& body) { for(const auto& statement:body) {
                        if(auto* var=dynamic_cast<VarDeclStmt*>(statement.get()))pure(var->initializer.get());
                        if(auto* expression=dynamic_cast<ExprStmt*>(statement.get()))pure(expression->expr.get());
                        if(auto* result=dynamic_cast<ReturnStmt*>(statement.get()))pure(result->value.get());
                        if(auto* branch=dynamic_cast<IfStmt*>(statement.get())){pure(branch->condition.get());visit(branch->thenBranch);visit(branch->elseBranch);}
                        if(auto* loop=dynamic_cast<WhileStmt*>(statement.get())){pure(loop->condition.get());visit(loop->body);}
                        if(auto* loop=dynamic_cast<ForStmt*>(statement.get())){pure(loop->start.get());pure(loop->bound.get());pure(loop->step.get());visit(loop->body);}
                        if(auto* block=dynamic_cast<BlockStmt*>(statement.get()))visit(block->body);
                        if(dynamic_cast<AwaitStmt*>(statement.get()))error(statement->sourceLoc,"migration cannot await");
                    }};
                    visit(fn->body);
                }
            }
            if (auto *fn = dynamic_cast<LifecycleFuncDecl *>(item.get()))
            {
                if (!lifecycleKinds.insert(static_cast<int>(fn->kind)).second)
                    error(fn->sourceLoc, "duplicate lifecycle");
                const bool runHost =
                    ctx.kind == LogiaHostKind::Tool || ctx.kind == LogiaHostKind::EventHandler;
                if ((fn->kind == LifecycleKind::Run) != runHost)
                    error(fn->sourceLoc, "lifecycle unsupported by this host");
                if (fn->kind == LifecycleKind::OnUpdate &&
                    (fn->params.size() > 1 ||
                     (!fn->params.empty() && fn->params[0].typeName != "float")))
                    error(fn->sourceLoc, "on_update takes at most one float parameter");
                if ((fn->kind == LifecycleKind::Run || fn->kind == LifecycleKind::OnDestroy ||
                     fn->kind == LifecycleKind::OnReload || fn->kind == LifecycleKind::OnRestore) &&
                    !fn->params.empty())
                    error(fn->sourceLoc, "lifecycle takes no parameters");
                if (fn->kind == LifecycleKind::OnStart &&
                    (fn->params.size() > 1 ||
                     (!fn->params.empty() && fn->params[0].typeName != "Entity")))
                    error(fn->sourceLoc, "on_start takes at most one Entity parameter");
                function(fn->params, fn->body, "void", fn->sourceLoc);
            }
        }
    }
};
} // namespace
const BuiltinFunctionCatalog &checkedBuiltinFunctions()
{
    static const BuiltinFunctionCatalog builtins{
        {"vec3.new", {{"float", "float", "float"}, "vec3"}},
        {"vec3.length", {{"vec3"}, "float"}},
        {"vec3.normalized", {{"vec3"}, "vec3"}},
        {"vec3.add", {{"vec3", "vec3"}, "vec3"}},
        {"vec3.subtract", {{"vec3", "vec3"}, "vec3"}},
        {"vec3.scale", {{"vec3", "float"}, "vec3"}},
        {"vec3.dot", {{"vec3", "vec3"}, "float"}},
        {"vec3.cross", {{"vec3", "vec3"}, "vec3"}},
        {"quat.identity", {{}, "quat"}},
        {"quat.axis_angle", {{"vec3", "float"}, "quat"}},
        {"quat.normalized", {{"quat"}, "quat"}},
        {"quat.multiply", {{"quat", "quat"}, "quat"}},
        {"quat.rotate", {{"quat", "vec3"}, "vec3"}},
        {"quat.slerp", {{"quat", "quat", "float"}, "quat"}},
        {"log.info", {{"string"}, "void"}},
        {"log.warn", {{"string"}, "void"}},
        {"log.error", {{"string"}, "void"}},
        {"input.is_pressed", {{"string"}, "bool"}},
        {"input.is_just_pressed", {{"string"}, "bool"}},
        {"input.is_just_released", {{"string"}, "bool"}},
        {"input.axis", {{"string"}, "float"}},
        {"input.vec2", {{"string"}, "vec2"}},
        {"vec2.length", {{"vec2"}, "float"}},
        {"vec2.normalized", {{"vec2"}, "vec2"}},
        {"event.unsubscribe", {{"int"}, "void"}},
        {"entity.create", {{"string"}, "int"}},
        {"entity.self", {{}, "int"}},
        {"entity.spawn_actor", {{"string", "string"}, "int"}},
        {"entity.find", {{"string"}, "int"}},
        {"entity.exists", {{"int"}, "bool"}},
        {"entity.pending", {{"int"}, "bool"}},
        {"entity.destroy", {{"int"}, "bool"}},
        {"entity.has_component", {{"int", "string"}, "bool"}},
        {"entity.add_component", {{"int", "string"}, "bool"}},
        {"entity.remove_component", {{"int", "string"}, "bool"}},
        {"entity.set_position", {{"int", "float", "float", "float"}, "bool"}},
        {"entity.get_number", {{"int", "string", "string"}, "float"}},
        {"entity.set_number", {{"int", "string", "string", "float"}, "bool"}},
        {"tasks.cancel", {{"int"}, "bool"}},
        {"tasks.pending", {{"int"}, "bool"}},
        {"tasks.status", {{"int"}, "string"}},
        {"event.error", {{}, "string"}},
        {"storage.snapshot", {{}, "string"}},
        {"storage.restore", {{"string"}, "bool"}},
        {"storage.version", {{}, "int"}},
        {"storage.error", {{}, "string"}}};
    return builtins;
}
const std::unordered_map<std::string, std::vector<std::string>> &checkedEventPayloads()
{
    static const std::unordered_map<std::string, std::vector<std::string>> events{
        {"window_resize", {"int", "int"}},
        {"window_close", {}},
        {"resource_ready", {"int", "int", "bool"}},
        {"script_test_ping", {"int"}},
        {"device_action", {"int", "bool"}},
        {"task_complete", {"int", "bool"}},
        {"logia_task_complete", {"int", "bool"}},
        {"physics_collision", {"int", "int", "int"}},
        {"scene_current_changed", {"Scene"}},
        {"scene_begin_play", {"Scene"}},
        {"scene_end_play", {"Scene"}}};
    return events;
}
std::vector<LogiaDiagnostic> checkLogiaSemantics(Program &program, const LogiaHostContext &ctx,
                                                 const std::string &fileName)
{
    // The legacy analyzer constructor bootstraps the shared reflection types
    // (FVector3, Transform, primitives); do not run its legacy analysis here.
    SemanticAnalyzer reflectionBootstrap({}, ctx);
    Checker checker{ctx, fileName};
    if (program.scripts.size() != 1)
        checker.error({}, "one script declaration is required per source");
    for (auto &script : program.scripts)
        checker.script(*script);
    return std::move(checker.diagnostics);
}
} // namespace ayt::script::logia
