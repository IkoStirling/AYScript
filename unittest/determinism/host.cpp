#include "DetHostFixture.h"
#include <AYScript/DeterministicLogia.h>
#include <iostream>
using namespace dethost_test;
using namespace ayt::script;
using D=ayt::math::DetFloat32;
void require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
int main(){try{
    require(bool(registerEntityCoreComponents(ComponentRegistry::instance())),"Core components");
    Fixture f;std::string error;
    auto program=compileDeterministicLogia("script Movement { on_update(dt:float) { sim.translate(2,det.vec3(3.0*dt,0,0)) } }",{},error);
    require(bool(program),error);
    auto configure=[program](auto& s){return installDeterministicLogia(s,10,0,program)&&s.addEntity(2)&&s.seal();};
    auto recipe=Fixture::recipe();recipe.configure=configure;recipe.input={};
    require(f.bind(recipe),f.controller.error());DeterministicSession kernel(recipe.config);require(configure(kernel),"kernel configure");
    std::optional<DetSessionCheckpoint> middle;
    for(unsigned tick=0;tick<10000;++tick){
        f.frame(1.0f/64.0f,tick+1);require(kernel.advance({tick,1,{}}),kernel.error());
        require(encodeDetCheckpoint(*f.controller.checkpoint())==encodeDetCheckpoint(*kernel.checkpoint()),"Host/owned kernel bytes");
        require(f.controller.checkpoint()->actors.at(2).pose.position[0]==(D::fromUInt(3*(tick+1))/D::fromUInt(64)).bits(),"Host software movement oracle");
        if(tick==4999)middle=f.controller.checkpoint();
    }
    const auto final=encodeDetCheckpoint(*kernel.checkpoint());require(f.controller.restore(*middle),f.controller.error());require(f.controller.resume(),"resume");
    for(unsigned tick=5000;tick<10000;++tick)f.frame(1.0f/64.0f,tick+10001);
    require(encodeDetCheckpoint(*f.controller.checkpoint())==final,"Host script restore/replay");
    std::cout<<"PASS deterministic Logia standard Host/Scene 10000 ticks vs owned Session, restore/replay 5000\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
