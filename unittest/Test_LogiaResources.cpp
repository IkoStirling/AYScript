#include <AYScript/ScriptRuntimeBridge.h>
#include <AYScript/logia/LogiaPipeline.h>
#include <AYScript/logia/Package.h>
#include <AYScript/logia/Tooling.h>
#include <AYEntity/components/ActorInstanceComponent.h>
#include <AYTest.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
using namespace ayt::script;
namespace {
namespace fs=std::filesystem;
struct Files {
    fs::path root=fs::temp_directory_path()/("logia-resources-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Files(){fs::create_directories(root/"Assets/modules");fs::create_directories(root/"Assets/data");}
    ~Files(){std::error_code e;fs::remove_all(root,e);}
    void put(const std::string& path,const std::string& text){std::ofstream(root/"Assets"/path,std::ios::binary)<<text;}
    auto context(){auto c=logia::checkedLogiaHostContext(logia::actorLogiaHostContext());
        c.actorProperties={{"hp","float"}};c.sourcePath=(root/"Assets/main.logia").string();
        c.assetRoot=(root/"Assets").string();return c;}
};
}
TEST_SUITE(LogiaResourceTests)
TEST_CASE(resources_are_explicit_immutable_and_collected_through_imports)
{
    Files files;
    files.put("modules/data.logia",R"(script Data {
        asset item:string="data/item.bin"
        function item_path():string {return item}
    })");
    const std::string source=R"(script Main {
        import "modules/data.logia"
        asset sound:string="audio/ping.wav"
        on_start(){ log.info(item_path()) log.info("unlisted/dynamic.txt") }
    })";
    auto c=files.context();auto compiled=logia::compileLogiaToLua(source,c);
    CHECK(compiled.success);
    CHECK(compiled.resourceDependencies==std::vector<std::string>({"audio/ping.wav","data/item.bin"}));
    auto analysis=logia::analyzeLogia(source,c);
    CHECK(analysis.resourceDependencies==compiled.resourceDependencies);
    const auto suggestions=analysis.complete("sound");
    CHECK(!suggestions.empty());if(!suggestions.empty())CHECK(suggestions[0].kind=="asset");
    CHECK(!logia::compileLogiaToLua(R"(script Bad {
        asset item:string="data/item.bin" on_start(){item="different.bin"}
    })",c).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad {asset item:string="../escape.bin"})",c).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad {asset item:string="D:/outside.bin"})",c).success);
    CHECK(!logia::compileLogiaToLua(R"(script Bad {asset item:int=7})",c).success);
}
TEST_CASE(package_v2_moves_binary_resources_and_rejects_missing_or_modified_content)
{
    Files files;
    files.put("main.logia",R"(script Main {
        asset item:string="data/item.bin" on_start(){ self.hp=probe.read(item) }
    })");
    const std::string bytes("\0payload\xff",9);
    files.put("data/item.bin",bytes);
    auto c=files.context();c.hostFunctions["probe.read"]={{"string"},"float"};
    auto package=logia::buildLogiaPackage(c.sourcePath,(files.root/"Published").string(),c);
    CHECK(package.success); if(!package.success)return;
    CHECK(package.resourceDependencies==std::vector<std::string>({"data/item.bin"}));
    fs::rename(files.root/"Assets",files.root/"AuthoringMoved");
    LogiaRuntimeBridge bridge;
    CHECK(bridge.registerHostFunction("probe.read",{{"string"},"float"},[&](const auto& args)->ScriptHostValue{
        std::ifstream in(files.root/"Published"/std::get<std::string>(args[0]),std::ios::binary);
        const std::string read((std::istreambuf_iterator<char>(in)),{});return read==bytes?42.0:0.0;
    }));
    std::vector<logia::CompilerError> errors;
    CHECK(bridge.loadScriptPackage(package.manifestPath,errors));
    ayt::entity::ActorScriptComponent actor;actor.properties["hp"]=0.0;
    CHECK(bridge.callActorLifecycle("Main","on_start",actor));
    CHECK_FLOAT_EQ(std::get<double>(actor.properties["hp"]),42.0,0.001);
    std::ofstream(files.root/"Published/data/item.bin",std::ios::binary)<<"corrupt";
    CHECK(!logia::readLogiaPackage(package.manifestPath).success);
    bridge.releaseInstance("Main",&actor);
    fs::remove(files.root/"AuthoringMoved/data/item.bin");
    c.sourcePath=(files.root/"AuthoringMoved/main.logia").string();c.assetRoot=(files.root/"AuthoringMoved").string();
    CHECK(!logia::buildLogiaPackage(c.sourcePath,(files.root/"Missing").string(),c).success);
    CHECK(!fs::exists(files.root/"Missing"));
}
TEST_SUITE_END
