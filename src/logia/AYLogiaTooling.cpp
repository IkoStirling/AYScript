#include "AYScript/logia/CheckedSemantics.h"
#include "AYScript/logia/Tooling.h"
#include <AYReflect/IReflect.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <unordered_map>
#include <filesystem>
#include <cctype>
namespace ayt::script::logia
{
namespace {
std::string fileKey(const std::string& file) {
    auto key=std::filesystem::path(file).lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
#endif
    return key;
}
}
std::vector<LogiaSymbol> LogiaAnalysis::complete(const std::string &prefix, const std::string &file,
                                                 int line) const
{
    std::unordered_map<std::string, LogiaSymbol> visible;
    for (const auto &symbol : symbols)
    {
        if (!symbol.name.starts_with(prefix))
            continue;
        if (symbol.scopeBegin &&
            (fileKey(file) != fileKey(symbol.definition.file) || line < symbol.scopeBegin || line > symbol.scopeEnd))
            continue;
        auto old = visible.find(symbol.name);
        if (old == visible.end() || old->second.scopeBegin < symbol.scopeBegin ||
            (old->second.scopeBegin == symbol.scopeBegin &&
             old->second.definition.line < symbol.definition.line))
            visible[symbol.name] = symbol;
    }
    std::vector<LogiaSymbol> out;
    for (auto &[name, symbol] : visible)
        out.push_back(std::move(symbol));
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    return out;
}
std::optional<SourceLocation> LogiaAnalysis::definitionAt(const SourceLocation &loc) const
{
    for (const auto &ref : references)
        if (fileKey(ref.location.file) == fileKey(loc.file) && ref.location.line == loc.line &&
            loc.column >= ref.location.column && ref.symbol < symbols.size() &&
            loc.column < ref.location.column + static_cast<int>(symbols[ref.symbol].name.size()))
            return symbols[ref.symbol].definition;
    return std::nullopt;
}
LogiaAnalysis analyzeLogia(const std::string &source, LogiaHostContext ctx)
{
    ctx.strictSemantics = true;
    ctx.sourcePath=std::filesystem::path(ctx.sourcePath).generic_string();
    Compiler compiler({ctx.sourcePath, false});
    auto compiled = compiler.compile(source, ctx);
    LogiaAnalysis result;
    result.success = compiled.success;
    result.diagnostics = std::move(compiled.diagnostics);
    result.dependencies = std::move(compiled.dependencies);
    result.resourceDependencies = std::move(compiled.resourceDependencies);
    auto add = [&](std::string name, std::string type, std::string kind, SourceLocation loc = {},
                   int begin = 0, int end = 0) {
        result.symbols.push_back(
            {std::move(name), std::move(type), std::move(kind), std::move(loc), begin, end});
        return result.symbols.size() - 1;
    };
    auto signature = [](const auto &params, const std::string &returnType) {
        std::string type = "(";
        for (const auto &p : params)
        {
            if (type != "(")
                type += ", ";
            type += p;
        }
        return type + "): " + returnType;
    };
    for (const auto &[name, fn] : checkedBuiltinFunctions())
        add(name, signature(fn.first, fn.second), "facade");
    for (const auto &[name, fn] : ctx.hostFunctions)
        add(name, signature(fn.parameters, fn.result), "provider");
    for (const char *name : {"array.length", "array.push", "array.pop", "tasks.start",
                             "event.subscribe", "event.emit", "emit", "connect", "disconnect"})
        add(name, "", "builtin");
    add("time.delta", "float", "ambient");
    add("time.total", "float", "ambient");
    if (ctx.expectSelf)
    {
        for (const auto &[name, type] : ctx.actorProperties)
            add("self." + name, type, "property");
        if (ctx.kind == LogiaHostKind::Actor)
            for (const char *group : {"position", "scale", "rotation"})
            {
                auto base = std::string("self.transform.") + group;
                add(base, std::string(group) == "rotation" ? "quat" : "vec3", "property");
                for (const char *axis : {"x", "y", "z", "w"})
                    if (std::string(axis) != "w" || std::string(group) == "rotation")
                        add(base + "." + axis, "float", "property");
            }
        if (ctx.hostType)
            for (size_t i = 0; i < ctx.hostType->getFieldCount(); ++i)
            {
                auto *f = ctx.hostType->getField(i);
                if (!ayt::reflect::isScriptReadable(f, ctx.hostType))
                    continue;
                std::string name = f->getName();
                if (name.starts_with("m_") || name.starts_with("b_"))
                    name.erase(0, 2);
                add("self." + name, f->getType() ? f->getType()->getName() : "",
                    ayt::reflect::isScriptWritable(f) ? "property" : "readonly");
            }
    }
    if (!compiled.program)
        return result;
    // Token braces supply scope end positions even for empty blocks. AST declarations and
    // resolved references supply identity, so shadowed names never jump to a global by guesswork.
    struct Range
    {
        int begin, end;
    };
    std::unordered_map<std::string, std::vector<Range>> ranges;
    auto scan = [&](const std::string &file, const std::string &text) {
        std::vector<Token> tokens;
        tokenize(text, tokens);
        std::vector<int> stack;
        for (const auto &token : tokens)
        {
            if (token.type == TokenType::LeftBrace)
                stack.push_back(token.line);
            if (token.type == TokenType::RightBrace && !stack.empty())
            {
                ranges[file].push_back({stack.back(), token.line});
                stack.pop_back();
            }
        }
    };
    scan(ctx.sourcePath, source);
    for (const auto &file : result.dependencies)
    {
        std::ifstream in(file, std::ios::binary);
        scan(file, std::string((std::istreambuf_iterator<char>(in)), {}));
    }
    auto scope = [&](const SourceLocation &loc) {
        Range out{loc.line, std::numeric_limits<int>::max()};
        for (const auto &r : ranges[loc.file])
            if (r.begin <= loc.line && r.end >= loc.line &&
                (out.end == std::numeric_limits<int>::max() ||
                 r.end - r.begin < out.end - out.begin))
            {
                out = r;
            }
        return out;
    };
    std::unordered_map<const void *, size_t> bindings;
    auto variable = [&](VarDeclStmt &var, bool global) {
        auto r = scope(var.sourceLoc);
        auto id = add(var.name, var.typeName, var.resourceReference ? "asset" : var.instanceState ? "state" : "variable",
                      var.sourceLoc, global ? 0 : var.sourceLoc.line, global ? 0 : r.end);
        bindings[&var] = id;
        for (const char *axis : {"x", "y", "z", "w"})
            if ((var.typeName == "vec2" &&
                 (std::string(axis) == "x" || std::string(axis) == "y")) ||
                (var.typeName == "vec3" && std::string(axis) != "w") || var.typeName == "quat")
                add(var.name + "." + axis, "float", "field", var.sourceLoc,
                    global ? 0 : var.sourceLoc.line, global ? 0 : r.end);
    };
    std::function<void(std::vector<StmtPtr> &)> declarations;
    declarations = [&](auto &body) {
        for (auto &s : body)
        {
            if (auto *v = dynamic_cast<VarDeclStmt *>(s.get()))
                variable(*v, false);
            if (auto *wait = dynamic_cast<AwaitStmt *>(s.get()))
                for (auto &binding : wait->bindings) {
                    const auto r = scope(binding.sourceLoc);
                    bindings[&binding] = add(binding.name, binding.typeName, "event-payload",
                                             binding.sourceLoc, binding.sourceLoc.line, r.end);
                }
            if (auto *b = dynamic_cast<IfStmt *>(s.get()))
            {
                declarations(b->thenBranch);
                declarations(b->elseBranch);
            }
            if (auto *b = dynamic_cast<WhileStmt *>(s.get()))
                declarations(b->body);
            if (auto *b = dynamic_cast<ForStmt *>(s.get()))
            {
                auto r = scope(b->sourceLoc);
                add(b->counterName, "int", "counter", b->sourceLoc, b->sourceLoc.line, r.end);
                declarations(b->body);
            }
            if (auto *b = dynamic_cast<BlockStmt *>(s.get()))
                declarations(b->body);
        }
    };
    for (auto &script : compiled.program->scripts)
        for (auto &s : script->members)
        {
            if (auto *v = dynamic_cast<VarDeclStmt *>(s.get()))
                variable(*v, true);
            if (auto *event = dynamic_cast<ProjectEventDeclStmt *>(s.get())) {
                std::vector<std::string> types;for (const auto &param : event->params) types.push_back(param.typeName);
                add(event->name, signature(types, "void"), "project-event", event->sourceLoc);
            }
            std::vector<Param> *params = nullptr;
            std::vector<StmtPtr> *body = nullptr;
            if (auto *fn = dynamic_cast<FunctionDeclStmt *>(s.get()))
            {
                std::vector<std::string> types;
                for (auto &p : fn->params)
                    types.push_back(p.typeName);
                if(!fn->lifecycleHook)bindings[fn] = add(
                    fn->name, signature(types, fn->returnType.empty() ? "void" : fn->returnType),
                    fn->asynchronous ? "task" : "function", fn->sourceLoc);
                params = &fn->params;
                body = &fn->body;
            }
            if (auto *fn = dynamic_cast<LifecycleFuncDecl *>(s.get()))
            {
                add(lifecycleKindName(fn->kind), "", "lifecycle", fn->sourceLoc);
                params = &fn->params;
                body = &fn->body;
            }
            if (body)
            {
                auto r = scope(s->sourceLoc);
                if (s->closingLine > 0)
                    r.end = s->closingLine;
                for (auto &p : *params)
                    bindings[&p] = add(p.name, p.typeName, "parameter", s->sourceLoc,
                                       s->sourceLoc.line, r.end);
                declarations(*body);
            }
        }
    std::function<void(Expr *)> reference;
    std::function<void(std::vector<StmtPtr> &)> walk;
    reference = [&](Expr *e) {
        if (!e)
            return;
        if (auto *id = dynamic_cast<IdentifierExpr *>(e))
        {
            auto found = bindings.find(id->resolvedDecl);
            if (found != bindings.end())
                result.references.push_back({id->sourceLoc, found->second});
        }
        if (auto *b = dynamic_cast<BinaryExpr *>(e))
        {
            reference(b->left.get());
            reference(b->right.get());
        }
        if (auto *u = dynamic_cast<UnaryExpr *>(e))
            reference(u->operand.get());
        if (auto *m = dynamic_cast<MemberExpr *>(e))
            reference(m->object.get());
        if (auto *i = dynamic_cast<IndexExpr *>(e))
        {
            reference(i->object.get());
            reference(i->index.get());
        }
        if (auto *c = dynamic_cast<CallExpr *>(e))
        {
            reference(c->callee.get());
            for (auto &a : c->args)
                reference(a.get());
        }
        if (auto *t = dynamic_cast<TableExpr *>(e))
            for (auto &entry : t->entries)
                reference(entry.value.get());
    };
    walk = [&](auto &body) {
        for (auto &s : body)
        {
            if (auto *v = dynamic_cast<VarDeclStmt *>(s.get()))
                reference(v->initializer.get());
            if (auto *e = dynamic_cast<ExprStmt *>(s.get()))
                reference(e->expr.get());
            if (auto *r = dynamic_cast<ReturnStmt *>(s.get()))
                reference(r->value.get());
            if (auto *a = dynamic_cast<AwaitStmt *>(s.get()))
                reference(a->operation.get());
            if (auto *f = dynamic_cast<FunctionDeclStmt *>(s.get()))
                walk(f->body);
            if (auto *f = dynamic_cast<LifecycleFuncDecl *>(s.get()))
                walk(f->body);
            if (auto *b = dynamic_cast<BlockStmt *>(s.get()))
                walk(b->body);
            if (auto *b = dynamic_cast<IfStmt *>(s.get()))
            {
                reference(b->condition.get());
                walk(b->thenBranch);
                walk(b->elseBranch);
            }
            if (auto *b = dynamic_cast<WhileStmt *>(s.get()))
            {
                reference(b->condition.get());
                walk(b->body);
            }
            if (auto *b = dynamic_cast<ForStmt *>(s.get()))
            {
                reference(b->start.get());
                reference(b->bound.get());
                reference(b->step.get());
                walk(b->body);
            }
        }
    };
    for (auto &script : compiled.program->scripts)
        walk(script->members);
    return result;
}
} // namespace ayt::script::logia
