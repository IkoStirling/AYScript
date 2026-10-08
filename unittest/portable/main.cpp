#include <AYScript/DeterministicLogia.h>
#include <AYEntity/DeterministicRollback.h>
#include <iostream>
#include <cfenv>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif
using namespace ayt::entity;
using namespace ayt::script;
using D=ayt::math::DetFloat32;
void require(bool ok,const std::string& text){if(!ok)throw std::runtime_error(text);}
std::shared_ptr<const DeterministicLogiaProgram> compile(const std::string& text,DetLogiaOptions options={}){
    std::string error;auto p=compileDeterministicLogia(text,options,error);require(bool(p),error);return p;
}
const std::string source=R"(script DeterministicMovement {
    function speed():float { return 1.5 + 1.5 }
    on_update(dt:float) {
        sim.translate(2,det.vec3(speed()*dt,0,0))
        sim.write_i32(0,2,1,sim.read_i32(0,2,1)+1)
        sim.write_u32(0,2,2,sim.random_u32(7))
        sim.emit_byte(1,sim.tick()%256)
    }
})";
bool configure(DeterministicSession& s,std::shared_ptr<const DeterministicLogiaProgram> p,bool reverse=false){
    if(!s.registerTypedSchema({2,1,{{1,std::int32_t{0}},{2,std::uint32_t{0}},{3,D{}}}})
        || !s.registerGlobalState(2) || !s.registerRandomStream(7,42)
        || !installDeterministicLogia(s,10,0,p))return false;
    return (reverse?s.addEntity(100)&&s.addEntity(2):s.addEntity(2)&&s.addEntity(100)) && s.seal();
}
std::string wrap(const std::string& body){return "script Test { on_update(dt:float) {"+body+"} }";}
void rollbackChecks() {
    const auto program=compile(wrap(R"(
        var speed:int=0
        for(var i:0,sim.input_count()) { speed+=sim.input_byte(i,0) }
        sim.translate(2,det.vec3(det.from_int(speed)*dt,0,0))
        sim.write_i32(0,2,1,sim.read_i32(0,2,1)+1)
        sim.write_u32(0,2,2,sim.random_u32(7))
        sim.emit_byte(1,sim.tick()%256)
    )"));
    DeterministicSession s({1,1,1,64,0}),baseline({1,1,1,64,0});
    require(configure(s,program)&&configure(baseline,program,true),"Logia rollback config");
    DetRollbackConfig cfg;cfg.members={1,2};cfg.historyTicks=16;cfg.maxPredictionTicks=4;
    cfg.prediction={{1,DetPredictionMode::Hold}};DeterministicRollback owner(s,cfg);
    std::uint64_t eventCount=0;auto oracle=ayt::math::make_pcg32_state(42);std::uint32_t distance=0;
    auto input=[](std::uint64_t tick){return DetTickInput{tick,1,{{1,0,1,{3}},{2,0,1,{static_cast<std::uint8_t>(tick%7)}}}};};
    for(std::uint64_t tick=0;tick<10000;++tick) {
        const auto in=input(tick);require(baseline.advance(in),baseline.error());
        require(owner.submit(1,{tick,1,{in.commands[0]}}),owner.error());
        if(tick>=2){const auto past=input(tick-2);require(owner.submit(2,{tick-2,1,{past.commands[1]}}),owner.error());}
        require(owner.advance(),owner.error());
        eventCount+=owner.takeConfirmedEvents().size();distance+=3+static_cast<std::uint32_t>(tick%7);
        const auto cp=baseline.checkpoint();require(cp->globals.at(2)[1]==ayt::math::pcg32(oracle),"rollback script independent RNG");
    }
    for(std::uint64_t tick=9998;tick<10000;++tick){const auto in=input(tick);require(owner.submit(2,{tick,1,{in.commands[1]}}),owner.error());}
    eventCount+=owner.takeConfirmedEvents().size();
    const auto cp=s.checkpoint();require(owner.confirmedNextTick()==10000&&eventCount==10000&&owner.rollbackCount()>100,"Logia corrections and effect count");
    require(cp->actors.at(2).pose.position[0]==(D::fromUInt(distance)/D::fromUInt(64)).bits(),"Logia rollback independent pose");
    require(encodeDetCheckpoint(*cp)==encodeDetCheckpoint(*baseline.checkpoint()),"Logia rollback full state/RNG/counter bytes");
    std::cout<<"PASS actual Logia rollback 10000 delayed-input ticks, independent pose/RNG and exactly 10000 confirmed effects\n";
}
void fault(const std::string& body,const std::string& diagnostic,DetLogiaOptions options={}){
    DeterministicSession s({1,1,1,64,0});require(configure(s,compile(wrap(body),options)),"fault configure");
    const auto before=*s.checkpoint();require(!s.advance({0,1,{}})&&s.faulted(),"must fault");
    require(s.error().find(diagnostic)!=std::string::npos,"wrong diagnostic: "+s.error());
    require(s.restore(before)&&!s.faulted(),"explicit fault restore");
    require(encodeDetCheckpoint(*s.checkpoint())==encodeDetCheckpoint(before),"restore bytes");
}
int main(){try{
    std::fesetround(FE_UPWARD);
#if defined(_M_X64) || defined(__x86_64__)
    _mm_setcsr(_mm_getcsr()|0x8040u);
#endif
    auto p=compile(source);DeterministicSession a({1,1,1,64,0}),b({1,1,1,64,0});
    require(configure(a,p)&&configure(b,p,true),"configure movement");std::optional<DetSessionCheckpoint> middle;
    auto oracleRng=ayt::math::make_pcg32_state(42);
    for(unsigned tick=0;tick<10000;++tick){
        require(a.advance({tick,1,{}})&&b.advance({tick,1,{}}),"paired tick: "+a.error()+b.error());
        auto cp=*a.checkpoint();require(encodeDetCheckpoint(cp)==encodeDetCheckpoint(*b.checkpoint()),"paired full bytes");
        require(cp.actors.at(2).pose.position[0]==(D::fromUInt(3*(tick+1))/D::fromUInt(64)).bits(),"independent position oracle");
        const auto count=static_cast<std::int32_t>(cp.globals.at(2)[0]);require(count==static_cast<std::int32_t>(tick+1),"registered counter");
        require(cp.globals.at(2)[1]==ayt::math::pcg32(oracleRng),"registered RNG oracle");
        require(cp.pendingEvents.size()==1 && cp.pendingEvents[0].payload[0]==tick%256,"next tick event oracle");
        if(tick==4999)middle=cp;
    }
    auto expected=*a.checkpoint();require(detCheckpointHash(expected)==0x9f427001d6f95e05ull,"portable Logia golden hash");require(a.restore(*middle),"restore 5000");
    for(unsigned tick=5000;tick<10000;++tick)require(a.advance({tick,1,{}}),"replay tick");
    require(encodeDetCheckpoint(*a.checkpoint())==encodeDetCheckpoint(expected),"replay full bytes");
    std::cout<<"PASS Logia actual Session 10000 ticks + restore/replay 5000, hash "<<std::hex<<detCheckpointHash(expected)<<std::dec<<'\n';
    DeterministicSession other({1,1,1,64,0});require(configure(other,compile(source+"\n")),"different code");
    require(!other.restore(expected),"code fingerprint must reject restore");
    require(compile(source,{10001,16})->hash()!=p->hash(),"options fingerprint");
    for(const auto& body:{"time.now()","random()","self.hp=1","var a:string=\"x\"","var a:float=1.00000001","var a:int=1.5","var a:int=9223372036854775808","det.vec3(1,2)","var a:int=1 a.x=2"}){
        std::string error;require(!compileDeterministicLogia(wrap(body),{},error)&&!error.empty(),"unsupported source accepted: "+std::string(body));
    }
    for(const auto& text:{"script T { state x:int=0 on_update(dt:float){} }","script T { on_start(){} on_update(dt:float){} }","script T { function f():int { if true { return 1 } } on_update(dt:float){} }"}){
        std::string error;require(!compileDeterministicLogia(text,{},error),"unsupported declaration accepted");
    }
    std::string error;require(!compileDeterministicLogia(source,{0,16},error),"zero budget accepted");
    fault("sim.write_i32(0,2,1,7) while true {}","budget",{128,16});
    fault("var a:int=9223372036854775807 a+=1","overflow");
    fault("var a:int=1/0","division");fault("var a:float=det.sqrt(-1)","nonfinite");
    fault("sim.write_u32(0,2,2,-1)","range");fault("sim.read_i32(0,2,99)","field");
    auto recursive=compile("script T { function f():int { return f() } on_update(dt:float){var x:int=f()} }",{1000,4});
    DeterministicSession r;require(configure(r,recursive),"recursive configure");require(!r.advance({0,1,{}})&&r.error().find("depth")!=std::string::npos,"recursion budget");
    const auto controls=compile(wrap(R"(
        var sum:int=0
        for(var i:0,6) { if i==2 {continue} if i==5 {break} sum+=i }
        var lazy:bool=false && det.to_int(det.sqrt(-1))==0
        if sum==8 && !lazy {sim.write_i32(0,2,1,sum)}
        var decimals:float=1.5+0.125
        sim.write_float(0,2,3,decimals)
    )"));
    DeterministicSession c;require(configure(c,controls)&&c.advance({0,1,{}}),"control flow: "+c.error());
    require(c.checkpoint()->globals.at(2)[0]==8 && c.checkpoint()->globals.at(2)[2]==0x3fd00000u,"loop/lazy/decimal golden");
    std::cout<<"PASS code/options manifest4, type rejection, loop/lazy control flow, checked arithmetic, budgets and fault restore\n";
    rollbackChecks();
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
