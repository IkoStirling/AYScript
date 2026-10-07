#include "AYScript/logia/Modules.h"
#include "AYScript/logia/Parser.h"
#include <AYIO/PathSafety.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_set>

namespace ayt::script::logia
{
namespace
{
void stampExpr(Expr *expr, const std::string &file)
{
    if (!expr)
        return;
    expr->sourceLoc.file = file;
    if (auto *b = dynamic_cast<BinaryExpr *>(expr))
    {
        stampExpr(b->left.get(), file);
        stampExpr(b->right.get(), file);
    }
    if (auto *u = dynamic_cast<UnaryExpr *>(expr))
        stampExpr(u->operand.get(), file);
    if (auto *m = dynamic_cast<MemberExpr *>(expr))
        stampExpr(m->object.get(), file);
    if (auto *i = dynamic_cast<IndexExpr *>(expr))
    {
        stampExpr(i->object.get(), file);
        stampExpr(i->index.get(), file);
    }
    if (auto *c = dynamic_cast<CallExpr *>(expr))
    {
        stampExpr(c->callee.get(), file);
        for (auto &arg : c->args)
            stampExpr(arg.get(), file);
    }
    if (auto *t = dynamic_cast<TableExpr *>(expr))
        for (auto &entry : t->entries)
        {
            stampExpr(entry.key.get(), file);
            stampExpr(entry.value.get(), file);
        }
}
void stamp(std::vector<StmtPtr> &statements, const std::string &file)
{
    for (auto &statement : statements)
    {
        if (!statement)
            continue;
        statement->sourceLoc.file = file;
        if (auto *v = dynamic_cast<VarDeclStmt *>(statement.get()))
            stampExpr(v->initializer.get(), file);
        if (auto *r = dynamic_cast<ReturnStmt *>(statement.get()))
            stampExpr(r->value.get(), file);
        if (auto *e = dynamic_cast<ExprStmt *>(statement.get()))
            stampExpr(e->expr.get(), file);
        if (auto *a = dynamic_cast<AwaitStmt *>(statement.get()))
        {
            stampExpr(a->operation.get(), file);
            for (auto &binding : a->bindings)
                binding.sourceLoc.file = file;
        }
        if (auto *f = dynamic_cast<FunctionDeclStmt *>(statement.get()))
            stamp(f->body, file);
        if (auto *f = dynamic_cast<LifecycleFuncDecl *>(statement.get()))
            stamp(f->body, file);
        if (auto *b = dynamic_cast<BlockStmt *>(statement.get()))
            stamp(b->body, file);
        if (auto *i = dynamic_cast<IfStmt *>(statement.get()))
        {
            stampExpr(i->condition.get(), file);
            stamp(i->thenBranch, file);
            stamp(i->elseBranch, file);
        }
        if (auto *w = dynamic_cast<WhileStmt *>(statement.get()))
        {
            stampExpr(w->condition.get(), file);
            stamp(w->body, file);
        }
        if (auto *f = dynamic_cast<ForStmt *>(statement.get()))
        {
            stampExpr(f->start.get(), file);
            stampExpr(f->bound.get(), file);
            stampExpr(f->step.get(), file);
            stamp(f->body, file);
        }
    }
}
std::string key(const std::filesystem::path &path)
{
    auto value = path.generic_string();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return value;
}
} // namespace
std::vector<LogiaDiagnostic> expandLogiaModules(Program &program, const LogiaHostContext &ctx,
                                                std::vector<std::string> &dependencies,
                                                std::vector<std::string>* resources)
{
    namespace fs = std::filesystem;
    std::vector<LogiaDiagnostic> diagnostics;
    auto error = [&](SourceLocation location, const std::string &message) {
        diagnostics.push_back({DiagnosticSeverity::Error,
                               ErrorCode::InvalidOperation,
                               message,
                               std::move(location),
                               {}});
    };
    std::unordered_set<std::string> active, loaded;
    std::size_t totalBytes = 0;
    fs::path root;
    if (!ctx.sourcePath.empty())
    {
        std::error_code failure;
        root = fs::weakly_canonical(ctx.assetRoot.empty() ? fs::path(ctx.sourcePath).parent_path()
                                                       : fs::path(ctx.assetRoot),failure);
        if(failure){error({ctx.sourcePath,0,0},"invalid module source root: "+failure.message());return diagnostics;}
        // The entry text may be an unsaved editor document. Canonicalize its
        // directory, which also resolves symlinks for relative imports, rather
        // than asking Windows to open a filename that has not been saved yet.
        const fs::path sourcePath(ctx.sourcePath);
        auto main = fs::weakly_canonical(sourcePath.parent_path(), failure) / sourcePath.filename();
        if(failure){error({ctx.sourcePath,0,0},"invalid module source path: "+failure.message());return diagnostics;}
        if (fs::exists(sourcePath, failure)) {
            main = fs::canonical(sourcePath, failure);
            if (failure) { error({ctx.sourcePath,0,0}, "invalid module source path: " + failure.message()); return diagnostics; }
        }
        active.insert(key(main));
    }
    std::function<void(std::vector<StmtPtr> &, const fs::path &, unsigned, bool)> expand;
    expand = [&](auto &statements, const fs::path &source, unsigned depth, bool module) {
        stamp(statements, source.generic_string());
        std::vector<StmtPtr> output;
        std::unordered_set<int> lifecycleKinds;
        for (auto &statement : statements)
        {
            if (auto *import = dynamic_cast<ImportStmt *>(statement.get()))
            {
                if (!ctx.strictSemantics || root.empty())
                {
                    error(import->sourceLoc, "imports require checked, file-backed compilation");
                    continue;
                }
                if (depth >= 32 || loaded.size() >= 128)
                {
                    error(import->sourceLoc, "module dependency limit exceeded");
                    continue;
                }
                try
                {
                    fs::path relative(import->path);
                    if (relative.is_absolute() || relative.has_root_name() ||
                        relative.extension() != ".logia")
                    {
                        error(import->sourceLoc, "module path must be a relative .logia path");
                        continue;
                    }
                    auto path = fs::weakly_canonical(source.parent_path() / relative);
                    auto rootKey = key(root), pathKey = key(path);
                    if (pathKey != rootKey && !pathKey.starts_with(rootKey + "/"))
                    {
                        error(import->sourceLoc, "module escapes authoring root");
                        continue;
                    }
                    if (active.contains(pathKey))
                    {
                        error(import->sourceLoc, "cyclic module import");
                        continue;
                    }
                    if (loaded.contains(pathKey))
                        continue;
                    auto bytes = fs::file_size(path);
                    if (bytes > 1024 * 1024 || totalBytes + bytes > 4 * 1024 * 1024)
                    {
                        error(import->sourceLoc, "module source size limit exceeded");
                        continue;
                    }
                    totalBytes += static_cast<std::size_t>(bytes);
                    std::ifstream file(path, std::ios::binary);
                    if (!file)
                    {
                        error(import->sourceLoc, "module could not be read");
                        continue;
                    }
                    std::string text((std::istreambuf_iterator<char>(file)), {});
                    std::vector<Token> tokens;
                    tokenize(text, tokens);
                    Parser parser(tokens);
                    auto parsed = parser.parse();
                    if (parser.hasErrors() || !parsed || parsed->scripts.size() != 1)
                    {
                        for (const auto &item : parser.errors())
                            error({path.generic_string(), item.line, item.column}, item.message);
                        if (!parser.hasErrors())
                            error(import->sourceLoc, "module requires one script block");
                        continue;
                    }
                    if(parsed->scripts[0]->persistenceVersion!=1) {
                        error(import->sourceLoc,"behavior persistence version belongs to the entry script");continue;
                    }
                    active.insert(pathKey);
                    loaded.insert(pathKey);
                    dependencies.push_back(path.generic_string());
                    expand(parsed->scripts[0]->members, path, depth + 1, true);
                    active.erase(pathKey);
                    for (auto &item : parsed->scripts[0]->members)
                        output.push_back(std::move(item));
                }
                catch (const std::exception &exception)
                {
                    error(import->sourceLoc,
                          std::string("module read failed: ") + exception.what());
                }
            }
            else
            {
                const auto* variable=dynamic_cast<VarDeclStmt*>(statement.get());
                auto* hook=dynamic_cast<LifecycleFuncDecl*>(statement.get());
                if(module && hook) {
                    hook->behaviorHook=true;
                    if(!lifecycleKinds.insert(static_cast<int>(hook->kind)).second)
                        error(hook->sourceLoc,"duplicate lifecycle in behavior module");
                }
                if (module && !dynamic_cast<FunctionDeclStmt *>(statement.get()) && !hook &&
                    !dynamic_cast<SignalDeclStmt*>(statement.get()) &&
                    !(variable && (variable->resourceReference || variable->instanceState)))
                    error(statement->sourceLoc, "behaviors export helpers, asset constants, instance state, signals and lifecycle hooks; shared var is forbidden");
                output.push_back(std::move(statement));
            }
        }
        statements = std::move(output);
    };
    for (auto &script : program.scripts) {
        expand(script->members, ctx.sourcePath, 0, false);
        // Turn every participant into an independent helper, preserving return
        // and local scope semantics. The composed lifecycle calls these hooks.
        std::vector<StmtPtr> composed;
        unsigned serial=0;
        for(int kind=0;kind<=static_cast<int>(LifecycleKind::Run);++kind) {
            std::vector<LifecycleFuncDecl*> hooks;
            bool imported=false; unsigned authored=0;
            for(auto& member:script->members) if(auto* hook=dynamic_cast<LifecycleFuncDecl*>(member.get());hook && static_cast<int>(hook->kind)==kind) {
                hooks.push_back(hook);imported|=hook->behaviorHook;if(!hook->behaviorHook)++authored;
            }
            if(!imported)continue;
            if(authored>1)error(script->sourceLoc,"duplicate lifecycle in entry script");
            std::vector<Param> params;
            for(auto* hook:hooks)if(!hook->params.empty()) {
                if(params.empty())params=hook->params;
                else {
                    bool same=params.size()==hook->params.size();
                    for(size_t i=0;same && i<params.size();++i)same=params[i].typeName==hook->params[i].typeName;
                    if(!same)error(hook->sourceLoc,"incompatible behavior lifecycle parameter types");
                }
            }
            if(kind==static_cast<int>(LifecycleKind::OnDestroy))std::reverse(hooks.begin(),hooks.end());
            std::vector<StmtPtr> calls;
            for(auto* hook:hooks) {
                auto name="__ay_behavior_"+std::to_string(serial++)+"_"+lifecycleKindName(hook->kind);
                auto function=std::make_unique<FunctionDeclStmt>(name,hook->params,std::move(hook->body));
                function->returnType="void";function->lifecycleHook=true;function->sourceLoc=hook->sourceLoc;function->closingLine=hook->closingLine;
                std::vector<ExprPtr> args;
                for(size_t i=0;i<hook->params.size() && i<params.size();++i) {
                    auto arg=std::make_unique<IdentifierExpr>(params[i].name);arg->sourceLoc=hook->sourceLoc;args.push_back(std::move(arg));
                }
                auto callee=std::make_unique<IdentifierExpr>(name);callee->sourceLoc=hook->sourceLoc;
                auto call=std::make_unique<CallExpr>(std::move(callee),std::move(args));call->sourceLoc=hook->sourceLoc;
                auto statement=std::make_unique<ExprStmt>(std::move(call));statement->sourceLoc=hook->sourceLoc;
                calls.push_back(std::move(statement));composed.push_back(std::move(function));
            }
            auto lifecycle=std::make_unique<LifecycleFuncDecl>(static_cast<LifecycleKind>(kind),std::move(params),std::move(calls));
            lifecycle->sourceLoc=script->sourceLoc;composed.push_back(std::move(lifecycle));
            for(auto& member:script->members)if(auto* hook=dynamic_cast<LifecycleFuncDecl*>(member.get());hook && static_cast<int>(hook->kind)==kind)member.reset();
        }
        std::erase_if(script->members,[](const auto& item){return !item;});
        for(auto& member:composed)script->members.push_back(std::move(member));
    }
    std::unordered_set<std::string> referenced;
    for(const auto& script:program.scripts) for(const auto& member:script->members) {
        auto* variable=dynamic_cast<VarDeclStmt*>(member.get());
        if(!variable || !variable->resourceReference) continue;
        auto* literal=dynamic_cast<LiteralExpr*>(variable->initializer.get());
        if(!ctx.strictSemantics || variable->typeName!="string" || !literal ||
           !std::holds_alternative<std::string>(literal->value)) {
            error(variable->sourceLoc,"asset requires checked literal string declaration"); continue;
        }
        const auto& path=std::get<std::string>(literal->value);
        fs::path relative(path);
        bool portable=ayt::io::path::isPortableRelative(path) && !relative.filename().empty();
        for(const auto& part:relative) if(part==".") portable=false;
        if(!portable) { error(variable->sourceLoc,"asset path must be portable and asset-root-relative"); continue; }
        if(!root.empty()) {
            fs::path resolved; std::string failure;
            if(!ayt::io::path::resolveWithinRoot(root,relative,resolved,failure)) {
                error(variable->sourceLoc,"asset reference escapes authoring root: "+failure); continue;
            }
        }
        if(referenced.size()>=128 && !referenced.contains(path)) {
            error(variable->sourceLoc,"asset reference limit exceeded"); continue;
        }
        if(referenced.insert(path).second && resources) resources->push_back(path);
    }
    if(resources) std::sort(resources->begin(),resources->end());
    return diagnostics;
}
} // namespace ayt::script::logia
