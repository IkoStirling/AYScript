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
                if (module && !dynamic_cast<FunctionDeclStmt *>(statement.get()) &&
                    !(variable && variable->resourceReference))
                    error(statement->sourceLoc, "modules may export only helper functions and asset constants");
                output.push_back(std::move(statement));
            }
        }
        statements = std::move(output);
    };
    for (auto &script : program.scripts)
        expand(script->members, ctx.sourcePath, 0, false);
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
