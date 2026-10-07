#include "AYScript/logia/LogiaPipeline.h"
#include "AYScript/logia/Package.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_set>
namespace ayt::script::logia
{
namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
std::string pathKey(fs::path path)
{
    auto s = path.generic_string();
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return s;
}
bool inside(const fs::path &root, const fs::path &path)
{
    return pathKey(path).starts_with(pathKey(root) + "/");
}
std::string read(const fs::path &path)
{
    auto size = fs::file_size(path);
    if (size > 1024 * 1024)
        throw std::runtime_error("package file size limit");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("could not read package source");
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
std::string readResource(const fs::path& path)
{
    if(!fs::is_regular_file(path) || fs::file_size(path)>64*1024*1024)
        throw std::runtime_error("package resource size/type limit");
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("could not read declared resource");
    return std::string((std::istreambuf_iterator<char>(in)),{});
}
std::string fingerprint(const std::string &source)
{
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : source)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << h;
    return out.str();
}
void fail(LogiaPackageResult &result, const std::string &path, const std::exception &error)
{
    result.diagnostics.push_back(
        {DiagnosticSeverity::Error, ErrorCode::InvalidOperation, error.what(), {path, 0, 0}, {}});
}
} // namespace
LogiaPackageResult buildLogiaPackage(const std::string &sourcePath, const std::string &destination,
                                     LogiaHostContext ctx)
{
    LogiaPackageResult out;
    fs::path staging;
    try
    {
        auto entry = fs::canonical(sourcePath);
        auto root = fs::weakly_canonical(ctx.assetRoot.empty() ? entry.parent_path()
                                                               : fs::path(ctx.assetRoot));
        auto target = fs::absolute(destination).lexically_normal();
        if (fs::exists(target))
            throw std::runtime_error("package destination already exists");
        if (!inside(root, entry))
            throw std::runtime_error("entry is outside the authoring root");
        ctx.strictSemantics = true;
        ctx.sourcePath = entry.generic_string();
        ctx.assetRoot = root.generic_string();
        auto source = read(entry);
        auto compiled = compileLogiaToLua(source, ctx);
        out.diagnostics = compiled.diagnostics;
        if (!compiled.success)
            return out;
        Compiler compiler;
        auto ast = compiler.compile(source, ctx);
        out.scriptName = ast.program->scripts.at(0)->name;
        std::vector<fs::path> files{entry};
        for (const auto &file : compiled.dependencies)
            files.emplace_back(file);
        auto relativeEntry = fs::relative(entry, root).generic_string();
        Json manifest{{"format", "logia-source-package"},
                      {"version", 2},
                      {"entry", relativeEntry},
                      {"script", out.scriptName},
                      {"host", static_cast<int>(ctx.kind)},
                      {"expectSelf", ctx.expectSelf},
                      {"actorProperties", ctx.actorProperties},
                      {"files", Json::array()},
                      {"resources", Json::array()},
                      {"hostFunctions", Json::object()}};
        for (const auto &[name, signature] : ctx.hostFunctions)
            manifest["hostFunctions"][name] = {{"parameters", signature.parameters},
                                               {"result", signature.result}};
        manifest["projectEventSchemas"]=ctx.projectEventSchemas;
        // Unique sibling staging directory, then a single rename publishes the closure.
        staging = target;
        staging += ".staging-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        if (!fs::create_directories(staging))
            throw std::runtime_error("could not create package staging directory");
        std::size_t bytes = 0;
        for (const auto &file : files)
        {
            auto canonical = fs::canonical(file);
            if (!inside(root, canonical))
                throw std::runtime_error("dependency escapes root");
            auto text = read(canonical);
            bytes += text.size();
            if (bytes > 5 * 1024 * 1024)
                throw std::runtime_error("package total size limit");
            auto relative = fs::relative(canonical, root);
            auto dest = staging / relative;
            fs::create_directories(dest.parent_path());
            std::ofstream output(dest, std::ios::binary);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!output)
                throw std::runtime_error("could not write package source");
            manifest["files"].push_back(
                {{"path", relative.generic_string()}, {"fingerprint", fingerprint(text)}});
        }
        std::size_t resourceBytes=0;
        for(const auto& logical:compiled.resourceDependencies) {
            if(pathKey(fs::path(logical))=="logia-package.json")
                throw std::runtime_error("declared resource conflicts with package manifest");
            auto path=fs::canonical(root/fs::path(logical));
            if(!inside(root,path)) throw std::runtime_error("declared resource escapes root");
            auto content=readResource(path);
            resourceBytes+=content.size();
            if(resourceBytes>256*1024*1024) throw std::runtime_error("package resource total size limit");
            auto dest=staging/fs::path(logical);
            fs::create_directories(dest.parent_path());
            std::ofstream output(dest,std::ios::binary);
            output.write(content.data(),static_cast<std::streamsize>(content.size()));
            if(!output) throw std::runtime_error("could not write package resource");
            manifest["resources"].push_back({{"path",logical},{"fingerprint",fingerprint(content)}});
        }
        {
            std::ofstream output(staging / "logia-package.json", std::ios::binary);
            output << manifest.dump(2) << '\n';
            if (!output)
                throw std::runtime_error("could not write package manifest");
        }
        auto checked = readLogiaPackage((staging / "logia-package.json").generic_string());
        if (!checked.success)
        {
            out.diagnostics = std::move(checked.diagnostics);
            fs::remove_all(staging);
            staging.clear();
            return out;
        }
        fs::rename(staging, target);
        staging.clear();
        out = readLogiaPackage((target / "logia-package.json").generic_string());
    }
    catch (const std::exception &error)
    {
        fail(out, sourcePath, error);
        if (!staging.empty())
        {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
        }
    }
    return out;
}
LogiaPackageResult readLogiaPackage(const std::string &manifestPath)
{
    LogiaPackageResult out;
    try
    {
        auto manifest = fs::canonical(manifestPath);
        auto root = manifest.parent_path();
        auto json = Json::parse(read(manifest));
        if (!json.at("version").is_number_integer() ||
            json.at("format").get<std::string>() != "logia-source-package" ||
            (json.at("version").get<int>() != 1 && json.at("version").get<int>() != 2))
            throw std::runtime_error("unsupported Logia package format");
        auto safe = [&](const std::string &relative) {
            fs::path p(relative);
            if (p.is_absolute() || p.has_root_name() || p.extension() != ".logia")
                throw std::runtime_error("invalid package source path");
            auto path = fs::canonical(root / p);
            if (!inside(root, path))
                throw std::runtime_error("package source escapes root");
            return path;
        };
        std::unordered_set<std::string> listed;
        std::size_t bytes = 0;
        const auto &files = json.at("files");
        if (!files.is_array() || files.empty() || files.size() > 129)
            throw std::runtime_error("invalid package file count");
        for (const auto &file : files)
        {
            auto path = safe(file.at("path"));
            if (!listed.insert(pathKey(path)).second)
                throw std::runtime_error("duplicate package path");
            auto source = read(path);
            bytes += source.size();
            if (bytes > 5 * 1024 * 1024 ||
                fingerprint(source) != file.at("fingerprint").get<std::string>())
                throw std::runtime_error("package content mismatch or size limit");
        }
        std::unordered_set<std::string> resources;
        if(json.at("version").get<int>()==2) {
            const auto& declared=json.at("resources");
            if(!declared.is_array() || declared.size()>128) throw std::runtime_error("invalid package resource count");
            std::size_t total=0;
            for(const auto& resource:declared) {
                const auto logical=resource.at("path").get<std::string>();
                const fs::path relative(logical);
                bool valid=!logical.empty() && !relative.is_absolute() && !relative.has_root_name() &&
                    logical.find(':')==std::string::npos && logical.find('\\')==std::string::npos && logical.find(char(0))==std::string::npos;
                for(const auto& part:relative) if(part=="." || part=="..") valid=false;
                if(!valid || !resources.insert(logical).second) throw std::runtime_error("invalid/duplicate package resource path");
                auto path=fs::canonical(root/relative);
                if(!inside(root,path)) throw std::runtime_error("package resource escapes root");
                auto content=readResource(path);total+=content.size();
                if(total>256*1024*1024 || fingerprint(content)!=resource.at("fingerprint").get<std::string>())
                    throw std::runtime_error("package resource mismatch or total size limit");
            }
        }
        auto entry = safe(json.at("entry"));
        if (!listed.contains(pathKey(entry)))
            throw std::runtime_error("unlisted package entry");
        auto &ctx = out.context;
        ctx.strictSemantics = true;
        ctx.sourcePath = entry.generic_string();
        ctx.assetRoot = root.generic_string();
        auto kind = json.at("host").get<int>();
        if (kind < static_cast<int>(LogiaHostKind::Component) ||
            kind > static_cast<int>(LogiaHostKind::Actor))
            throw std::runtime_error("invalid package host");
        ctx.kind = static_cast<LogiaHostKind>(kind);
        ctx.expectSelf = json.at("expectSelf");
        const bool expected =
            ctx.kind != LogiaHostKind::Tool && ctx.kind != LogiaHostKind::EventHandler;
        if (ctx.expectSelf != expected)
            throw std::runtime_error("package receiver contract mismatch");
        ctx.actorProperties = json.at("actorProperties").get<decltype(ctx.actorProperties)>();
        for (auto it = json.at("hostFunctions").begin(); it != json.at("hostFunctions").end(); ++it)
            ctx.hostFunctions[it.key()] = {
                it.value().at("parameters").get<std::vector<std::string>>(),
                it.value().at("result").get<std::string>()};
        if(json.contains("projectEventSchemas"))ctx.projectEventSchemas=json.at("projectEventSchemas").get<decltype(ctx.projectEventSchemas)>();
        out.source = read(entry);
        Compiler compiler;
        auto compiled = compiler.compile(out.source, ctx);
        out.diagnostics = compiled.diagnostics;
        if (!compiled.success)
            return out;
        out.scriptName = compiled.program->scripts.at(0)->name;
        if (out.scriptName != json.at("script").get<std::string>())
            throw std::runtime_error("package script identity mismatch");
        for (const auto &dependency : compiled.dependencies)
            if (!listed.contains(pathKey(fs::canonical(dependency))))
                throw std::runtime_error("unlisted imported dependency");
        if (listed.size() != compiled.dependencies.size() + 1)
            throw std::runtime_error("package contains unused source files");
        for(const auto& resource:compiled.resourceDependencies)
            if(!resources.contains(resource)) throw std::runtime_error("unlisted declared resource dependency");
        if(resources.size()!=compiled.resourceDependencies.size()) throw std::runtime_error("package contains unused resources");
        out.resourceDependencies=std::move(compiled.resourceDependencies);
        out.dependencies = std::move(compiled.dependencies);
        out.manifestPath = manifest.generic_string();
        out.success = true;
    }
    catch (const std::exception &error)
    {
        fail(out, manifestPath, error);
    }
    return out;
}
} // namespace ayt::script::logia
