#include <AYScript/DeterministicLogia.h>
#include <AYScript/logia/Lexer.h>
#include <AYScript/logia/Parser.h>
#include <AYReplay/ReplayHash.h>
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
namespace ayt::script {
namespace {
using namespace logia;
using D=math::DetFloat32;using V=math::DetVec3;using I=std::int64_t;
enum class Type {Void,Bool,Int,Float,Vec3};
using Value=std::variant<std::monostate,bool,I,D,V>;
Type type(const Value& v){return static_cast<Type>(v.index());}
Type named(const std::string& name){
    if(name=="void" || name.empty())return Type::Void;if(name=="bool")return Type::Bool;
    if(name=="int")return Type::Int;if(name=="float")return Type::Float;if(name=="vec3")return Type::Vec3;
    throw std::runtime_error("unsupported deterministic type: "+name);
}
[[noreturn]] void invalid(const std::string& s){throw std::runtime_error("deterministic Logia: "+s);}
I integer(const std::string& s){I result=0;auto parsed=std::from_chars(s.data(),s.data()+s.size(),result);
    if(parsed.ec!=std::errc{} || parsed.ptr!=s.data()+s.size())invalid("int literal out of range");return result;}
D decimal(std::string text){
    // Exact bounded rational operands, no strtof/native float or locale conversion.
    auto exponent=text.find_first_of("eE");int power=0;
    if(exponent!=std::string::npos){auto e=text.substr(exponent+1);if(!e.empty() && e[0]=='+')e.erase(0,1);
        const auto n=integer(e);if(n<-16 || n>16)invalid("decimal exponent outside bounded profile; use det.bits");power=static_cast<int>(n);text.resize(exponent);}
    auto dot=text.find('.');if(dot!=std::string::npos){power-=static_cast<int>(text.size()-dot-1);text.erase(dot,1);}
    if(text.empty() || std::any_of(text.begin(),text.end(),[](char c){return c<'0'||c>'9';}))invalid("invalid decimal");
    while(text.size()>1 && text.back()=='0'){text.pop_back();++power;}
    const auto first=text.find_first_not_of('0');if(first==std::string::npos)return D{};text.erase(0,first);
    if(text.size()>8)invalid("decimal precision outside bounded profile; use det.bits");
    std::uint64_t n=static_cast<std::uint64_t>(integer(text)),denominator=1;
    constexpr std::uint64_t bound=1u<<24;
    if(n>bound)invalid("decimal numerator outside bounded profile; use det.bits");
    for(int i=0;i<power;++i){if(n>bound/10)invalid("decimal numerator outside bounded profile; use det.bits");n*=10;}
    for(int i=0;i<-power;++i){if(denominator>bound/10)invalid("decimal denominator outside bounded profile; use det.bits");denominator*=10;}
    return D::fromUInt(static_cast<std::uint32_t>(n))/D::fromUInt(static_cast<std::uint32_t>(denominator));
}
Value literal(const LiteralExpr& e){
    if(!e.numericLexeme.empty())return std::holds_alternative<float>(e.value)?Value{decimal(e.numericLexeme)}:Value{integer(e.numericLexeme)};
    if(auto* b=std::get_if<bool>(&e.value))return *b;
    invalid("only bool/int/software-float literals are allowed");
}
bool accepts(Type destination,Type actual){return destination==actual || (destination==Type::Float && actual==Type::Int);}
I asInt(const Value& v){return std::get<I>(v);}
bool asBool(const Value& v){return std::get<bool>(v);}
I ranged(I value,I low,I high){if(value<low || value>high)invalid("integer argument out of range");return value;}
D asFloat(const Value& v){if(auto* d=std::get_if<D>(&v))return *d;
    return D::fromInt(static_cast<std::int32_t>(ranged(asInt(v),INT32_MIN,INT32_MAX)));}
Value convert(Type destination,Value v){if(destination==Type::Float && type(v)==Type::Int)return asFloat(v);
    if(type(v)!=destination)invalid("runtime type mismatch");return v;}
Value zero(Type t){switch(t){case Type::Bool:return false;case Type::Int:return I{0};case Type::Float:return D{};case Type::Vec3:return V{};default:return {};}}
std::string callee(const Expr& e){if(auto* id=dynamic_cast<const IdentifierExpr*>(&e))return id->name;
    if(auto* m=dynamic_cast<const MemberExpr*>(&e))if(auto* id=dynamic_cast<const IdentifierExpr*>(m->object.get()))return id->name+"."+m->member;
    invalid("calls require a declared helper or allowed Sim facade");}
struct Signature {std::vector<Type> args;Type result;};
const std::map<std::string,Signature>& builtins(){
    static const auto rows=[](){std::map<std::string,Signature> r;
        const auto F=Type::Float,N=Type::Int,B=Type::Bool,W=Type::Void,P=Type::Vec3;
        r["sim.dt"]={{},F};r["sim.tick"]={{},N};r["sim.position"]={{N},P};
        r["sim.set_position"]={{N,P},B};r["sim.translate"]={{N,P},B};r["sim.random_u32"]={{N},N};
        for(const auto& [suffix,t]:std::vector<std::pair<std::string,Type>>{{"float",F},{"vec3",P},{"i32",N},{"u32",N},{"i64",N},{"bool",B}}){
            r["sim.read_"+suffix]={{N,N,N},t};r["sim.write_"+suffix]={{N,N,N,t},W};}
        r["sim.input_count"]={{},N};r["sim.event_count"]={{},N};
        for(auto prefix:{"sim.input_","sim.event_"}){r[std::string(prefix)+"source"]={{N},N};r[std::string(prefix)+"type"]={{N},N};r[std::string(prefix)+"byte"]={{N,N},N};}
        r["sim.emit_byte"]={{N,N},W};r["det.bits"]={{N},F};r["det.from_int"]={{N},F};r["det.to_int"]={{F},N};
        r["det.ratio"]={{N,N},F};r["det.vec3"]={{F,F,F},P};r["det.normalize"]={{P},P};r["det.dot"]={{P,P},F};
        for(auto n:{"det.sqrt","det.sin","det.cos","det.acos"})r[n]={{F},F};r["det.atan2"]={{F,F},F};return r;
    }();return rows;
}
bool numeric(Type t){return t==Type::Int || t==Type::Float;}
bool assignment(TokenType op){return op==TokenType::Equal || op==TokenType::PlusEqual || op==TokenType::MinusEqual || op==TokenType::StarEqual || op==TokenType::SlashEqual;}
TokenType arithmetic(TokenType op){switch(op){case TokenType::PlusEqual:return TokenType::Plus;case TokenType::MinusEqual:return TokenType::Minus;case TokenType::StarEqual:return TokenType::Star;case TokenType::SlashEqual:return TokenType::Slash;default:return op;}}
Type binaryType(TokenType op,Type a,Type b){
    if(op==TokenType::And || op==TokenType::Or){if(a==Type::Bool && b==a)return a;invalid("boolean operands required");}
    if(op==TokenType::EqualEqual || op==TokenType::BangEqual){if((a==b && a!=Type::Void) || (numeric(a)&&numeric(b)))return Type::Bool;invalid("incompatible comparison");}
    if(op==TokenType::Less || op==TokenType::LessEqual || op==TokenType::Greater || op==TokenType::GreaterEqual){if(numeric(a)&&numeric(b))return Type::Bool;invalid("numeric comparison required");}
    if(op==TokenType::Percent){if(a==Type::Int && b==a)return a;invalid("integer remainder required");}
    if(numeric(a)&&numeric(b))return a==Type::Float || b==Type::Float?Type::Float:Type::Int;
    if(a==Type::Vec3 && b==a && (op==TokenType::Plus || op==TokenType::Minus))return a;
    if(a==Type::Vec3 && numeric(b) && (op==TokenType::Star || op==TokenType::Slash))return a;
    if(numeric(a) && b==Type::Vec3 && op==TokenType::Star)return b;
    invalid("unsupported software arithmetic");
}
struct Definitions {
    const LifecycleFuncDecl* entry=nullptr;
    std::map<std::string,const FunctionDeclStmt*> helpers;
    std::map<std::string,Signature> signatures;
};
Definitions definitions(const Program& p){
    if(p.scripts.size()!=1)invalid("exactly one script is required");Definitions d;
    d.signatures=builtins();
    for(const auto& member:p.scripts[0]->members){
        if(auto* e=dynamic_cast<const LifecycleFuncDecl*>(member.get())){
            if(e->kind!=LifecycleKind::OnUpdate || d.entry || e->params.size()!=1 || e->params[0].typeName!="float")invalid("only on_update(dt:float) is supported");d.entry=e;
        } else if(auto* h=dynamic_cast<const FunctionDeclStmt*>(member.get())){
            if(h->asynchronous || h->lifecycleHook || d.helpers.contains(h->name) || d.signatures.contains(h->name))invalid("duplicate/async helper");
            Signature signature{{},named(h->returnType)};for(const auto& a:h->params){auto t=named(a.typeName);if(t==Type::Void)invalid("helper parameter needs a value type");signature.args.push_back(t);}
            d.helpers[h->name]=h;d.signatures[h->name]=std::move(signature);
        } else invalid("System profile forbids script shared/state/import/asset/signal/event declarations");
    }
    if(!d.entry)invalid("missing on_update(dt:float)");return d;
}
template<class T> struct Environment {
    std::vector<std::map<std::string,T>> scopes{{}};
    T& get(const std::string& name){for(auto i=scopes.rbegin();i!=scopes.rend();++i)if(i->contains(name))return i->at(name);invalid("unknown local: "+name);}
    void add(const std::string& name,T value){if(scopes.back().size()>=128 || scopes.back().contains(name))invalid("duplicate local or local count limit");scopes.back().emplace(name,std::move(value));}
};
struct Checker {
    const Definitions& defs;Environment<Type> env;Type result=Type::Void;unsigned depth=0,loops=0,nodes=0;
    explicit Checker(const Definitions& d):defs(d){}
    void charge(){if(++nodes>4096)invalid("AST node limit");}
    Type expr(const Expr& e){charge();if(++depth>64)invalid("expression depth limit");
        struct Guard{unsigned& v;~Guard(){--v;}} guard{depth};
        if(auto* p=dynamic_cast<const LiteralExpr*>(&e))return type(literal(*p));
        if(auto* p=dynamic_cast<const IdentifierExpr*>(&e))return env.get(p->name);
        if(auto* p=dynamic_cast<const MemberExpr*>(&e)){if(expr(*p->object)==Type::Vec3 && (p->member=="x"||p->member=="y"||p->member=="z"))return Type::Float;invalid("only software vec3 lanes are accessible");}
        if(auto* p=dynamic_cast<const CallExpr*>(&e)){auto name=callee(*p->callee);auto it=defs.signatures.find(name);
            if(it==defs.signatures.end() || p->args.size()!=it->second.args.size())invalid("unknown call/argument count: "+name);
            for(std::size_t i=0;i<p->args.size();++i)if(!accepts(it->second.args[i],expr(*p->args[i])))invalid("call argument type: "+name);return it->second.result;}
        if(auto* p=dynamic_cast<const UnaryExpr*>(&e)){auto t=expr(*p->operand);
            if(p->op.type==TokenType::Bang && t==Type::Bool)return t;
            if((p->op.type==TokenType::Minus || p->op.type==TokenType::Plus) && (numeric(t)||t==Type::Vec3))return t;invalid("unary type");}
        if(auto* p=dynamic_cast<const BinaryExpr*>(&e)){auto a=expr(*p->left),b=expr(*p->right);
            if(assignment(p->op.type)){if(!dynamic_cast<const IdentifierExpr*>(p->left.get()))invalid("assignment only targets a local");
                auto actual=p->op.type==TokenType::Equal?b:binaryType(arithmetic(p->op.type),a,b);
                if(!accepts(a,actual))invalid("assignment type");return a;}
            return binaryType(p->op.type,a,b);}
        invalid("unsupported expression");
    }
    bool block(const std::vector<StmtPtr>& statements){env.scopes.emplace_back();bool returns=false;
        for(const auto& s:statements)returns=stmt(*s)||returns;env.scopes.pop_back();return returns;}
    bool stmt(const Stmt& s){charge();if(env.scopes.size()>64)invalid("statement depth limit");
        if(auto* p=dynamic_cast<const VarDeclStmt*>(&s)){if(p->instanceState || p->persistent || p->resourceReference)invalid("locals cannot persist");
            auto t=p->typeName.empty()?(p->initializer?expr(*p->initializer):Type::Void):named(p->typeName);
            if(t==Type::Void || (p->initializer && !accepts(t,expr(*p->initializer))))invalid("local type");env.add(p->name,t);return false;}
        if(auto* p=dynamic_cast<const ExprStmt*>(&s)){expr(*p->expr);return false;}
        if(auto* p=dynamic_cast<const ReturnStmt*>(&s)){auto t=p->value?expr(*p->value):Type::Void;if(!accepts(result,t))invalid("return type");return true;}
        if(auto* p=dynamic_cast<const IfStmt*>(&s)){if(expr(*p->condition)!=Type::Bool)invalid("if needs bool");auto a=block(p->thenBranch),b=block(p->elseBranch);return a&&b;}
        if(auto* p=dynamic_cast<const WhileStmt*>(&s)){if(expr(*p->condition)!=Type::Bool)invalid("while needs bool");++loops;block(p->body);--loops;return false;}
        if(auto* p=dynamic_cast<const ForStmt*>(&s)){if(expr(*p->bound)!=Type::Int || (p->start && expr(*p->start)!=Type::Int) || (p->step && expr(*p->step)!=Type::Int))invalid("for bounds need int");
            env.scopes.emplace_back();env.add(p->counterName,Type::Int);++loops;block(p->body);--loops;env.scopes.pop_back();return false;}
        if(auto* p=dynamic_cast<const BlockStmt*>(&s))return block(p->body);
        if(auto* p=dynamic_cast<const BreakStmt*>(&s)){if(!loops || !p->label.empty())invalid("unlabelled loop break required");return false;}
        if(auto* p=dynamic_cast<const ContinueStmt*>(&s)){if(!loops || !p->label.empty())invalid("unlabelled loop continue required");return false;}
        invalid("unsupported statement");
    }
    void check(){env.add(defs.entry->params[0].name,Type::Float);block(defs.entry->body);
        for(const auto& [name,f]:defs.helpers){env.scopes={{}};result=named(f->returnType);loops=0;
            for(const auto& p:f->params)env.add(p.name,named(p.typeName));const bool returns=block(f->body);
            if(result!=Type::Void && !returns)invalid("helper must return on every path: "+name);}}
};
I add(I a,I b){if((b>0 && a>INT64_MAX-b)||(b<0 && a<INT64_MIN-b))invalid("integer overflow");return a+b;}
I subtract(I a,I b){if((b>0 && a<INT64_MIN+b)||(b<0 && a>INT64_MAX+b))invalid("integer overflow");return a-b;}
I multiply(I a,I b){
    if(a>0 ? (b>0 ? a>INT64_MAX/b : b<INT64_MIN/a) : (a<0 && (b>0 ? a<INT64_MIN/b : b<0 && a<INT64_MAX/b)))invalid("integer overflow");return a*b;
}
Value finite(Value v){if(auto* p=std::get_if<D>(&v);p && !p->isFinite())invalid("nonfinite software result");
    if(auto* p=std::get_if<V>(&v);p && !p->isFinite())invalid("nonfinite software vector");return v;}
Value binary(TokenType op,const Value& a,const Value& b){
    const auto result=binaryType(op,type(a),type(b));
    if(op==TokenType::And)return asBool(a)&&asBool(b);if(op==TokenType::Or)return asBool(a)||asBool(b);
    if(op==TokenType::EqualEqual || op==TokenType::BangEqual){const bool same=numeric(type(a))&&numeric(type(b))
        ? (type(a)==Type::Int && type(b)==Type::Int ? asInt(a)==asInt(b):asFloat(a)==asFloat(b)):a==b;
        return op==TokenType::EqualEqual?same:!same;}
    if(result==Type::Bool){auto compare=[&](auto x,auto y){switch(op){case TokenType::Less:return x<y;case TokenType::LessEqual:return x<=y;case TokenType::Greater:return x>y;default:return x>=y;}};
        return type(a)==Type::Int && type(b)==Type::Int ? compare(asInt(a),asInt(b)):compare(asFloat(a),asFloat(b));}
    if(result==Type::Int){auto x=asInt(a),y=asInt(b);switch(op){case TokenType::Plus:return add(x,y);case TokenType::Minus:return subtract(x,y);case TokenType::Star:return multiply(x,y);
        default:if(!y || (x==INT64_MIN && y==-1))invalid("integer division/domain overflow");return op==TokenType::Percent?x%y:x/y;}}
    if(result==Type::Float){auto x=asFloat(a),y=asFloat(b);switch(op){case TokenType::Plus:return finite(x+y);case TokenType::Minus:return finite(x-y);case TokenType::Star:return finite(x*y);default:return finite(x/y);}}
    if(type(a)==Type::Vec3){const auto& x=std::get<V>(a);if(type(b)==Type::Vec3)return finite(op==TokenType::Plus?x+std::get<V>(b):x-std::get<V>(b));
        return finite(op==TokenType::Star?x*asFloat(b):x/asFloat(b));}return finite(std::get<V>(b)*asFloat(a));
}
struct Flow {enum Kind{Return,Break,Continue} kind;Value value;};
struct Evaluator {
    const Definitions& defs;entity::DetTickContext& ctx;DetLogiaOptions options;std::uint32_t remaining,callDepth=0;
    Evaluator(const Definitions& d,entity::DetTickContext& c,DetLogiaOptions o):defs(d),ctx(c),options(o),remaining(o.maxOperations){}
    void charge(){if(!remaining)invalid("tick operation budget exhausted");--remaining;}
    Value builtin(const std::string& name,const std::vector<Value>& args){
        auto n=[&](unsigned i){return asInt(args.at(i));};
        auto id=[&](unsigned i){return static_cast<std::uint64_t>(ranged(n(i),0,INT64_MAX));};
        auto u32=[&](unsigned i){return static_cast<std::uint32_t>(ranged(n(i),1,UINT32_MAX));};
        auto f=[&](unsigned i){return asFloat(args.at(i));};
        if(name=="sim.dt")return ctx.dt();if(name=="sim.tick"){if(ctx.tick()>INT64_MAX)invalid("tick exceeds script int64");return static_cast<I>(ctx.tick());}
        if(name=="sim.position")return ctx.pose(id(0)).position;
        if(name=="sim.set_position" || name=="sim.translate"){const auto v=std::get<V>(args[1]);const bool ok=name=="sim.set_position"?ctx.pose(id(0)).setPosition(v):ctx.pose(id(0)).translate(v);
            if(!ok)invalid("pose rejected nonfinite result");return true;}
        if(name=="sim.random_u32")return static_cast<I>(math::pcg32(ctx.random(u32(0))));
        if(name.starts_with("sim.read_") || name.starts_with("sim.write_")){
            const bool write=name.starts_with("sim.write_");const auto suffix=name.substr(write?10:9);const auto actor=id(0);const auto schema=u32(1),field=u32(2);
            if(write){const auto& v=args[3];if(suffix=="float")ctx.write(actor,schema,field,asFloat(v));else if(suffix=="vec3")ctx.write(actor,schema,field,std::get<V>(v));
                else if(suffix=="bool")ctx.write(actor,schema,field,asBool(v));else if(suffix=="i64")ctx.write(actor,schema,field,asInt(v));
                else if(suffix=="i32")ctx.write(actor,schema,field,static_cast<std::int32_t>(ranged(asInt(v),INT32_MIN,INT32_MAX)));
                else ctx.write(actor,schema,field,static_cast<std::uint32_t>(ranged(asInt(v),0,UINT32_MAX)));return {};}
            if(suffix=="float")return ctx.read<D>(actor,schema,field);if(suffix=="vec3")return ctx.read<V>(actor,schema,field);if(suffix=="bool")return ctx.read<bool>(actor,schema,field);
            if(suffix=="i64")return ctx.read<I>(actor,schema,field);if(suffix=="i32")return static_cast<I>(ctx.read<std::int32_t>(actor,schema,field));return static_cast<I>(ctx.read<std::uint32_t>(actor,schema,field));
        }
        if(name=="sim.input_count")return static_cast<I>(ctx.input().commands.size());if(name=="sim.event_count")return static_cast<I>(ctx.events().size());
        if(name.starts_with("sim.input_") || name.starts_with("sim.event_")){
            const bool input=name.starts_with("sim.input_");const auto commands=input?std::span<const entity::DetTickCommand>{ctx.input().commands}:ctx.events();
            if(commands.empty())invalid("empty command list");const auto& command=commands[static_cast<std::size_t>(ranged(n(0),0,static_cast<I>(commands.size()-1)))];
            const auto suffix=name.substr(10);if(suffix=="source")return static_cast<I>(command.source);if(suffix=="type")return static_cast<I>(command.type);
            if(command.payload.empty())invalid("empty command payload");return static_cast<I>(command.payload[static_cast<std::size_t>(ranged(n(1),0,static_cast<I>(command.payload.size()-1)))]);
        }
        if(name=="sim.emit_byte"){const std::uint8_t b=static_cast<std::uint8_t>(ranged(n(1),0,255));ctx.emit(u32(0),{&b,1});return {};}
        if(name=="det.bits")return finite(D::fromBits(static_cast<std::uint32_t>(ranged(n(0),INT32_MIN,UINT32_MAX))));
        if(name=="det.from_int")return asFloat(args[0]);if(name=="det.to_int"){auto v=f(0).toInt();if(!v)invalid("float conversion out of int32 range");return static_cast<I>(*v);}
        if(name=="det.ratio")return finite(asFloat(args[0])/asFloat(args[1]));
        if(name=="det.vec3")return V{f(0),f(1),f(2)};
        if(name=="det.normalize"){auto result=std::get<V>(args[0]).normalized();if(!result)invalid("invalid normalization");return *result;}
        if(name=="det.dot")return finite(std::get<V>(args[0]).dot(std::get<V>(args[1])));
        if(name=="det.sqrt")return finite(f(0).sqrt());if(name=="det.sin")return math::detSin(f(0));if(name=="det.cos")return math::detCos(f(0));
        if(name=="det.acos")return finite(math::detAcos(f(0)));if(name=="det.atan2")return math::detAtan2(f(0),f(1));invalid("unknown builtin");
    }
    Value call(const std::string& name,std::vector<Value> args){charge();const auto& signature=defs.signatures.at(name);
        for(std::size_t i=0;i<args.size();++i)args[i]=convert(signature.args[i],std::move(args[i]));
        if(builtins().contains(name))return builtin(name,args);
        if(++callDepth>options.maxCallDepth)invalid("helper call depth exhausted");struct Guard{std::uint32_t& n;~Guard(){--n;}} guard{callDepth};
        const auto* f=defs.helpers.at(name);Environment<Value> env;for(std::size_t i=0;i<args.size();++i)env.add(f->params[i].name,std::move(args[i]));
        try{block(f->body,env);}catch(const Flow& flow){if(flow.kind!=Flow::Return)invalid("loop flow escaped helper");return convert(signature.result,flow.value);}
        if(signature.result!=Type::Void)invalid("helper did not return");return {};
    }
    Value expr(const Expr& e,Environment<Value>& env){charge();
        if(auto* p=dynamic_cast<const LiteralExpr*>(&e))return literal(*p);
        if(auto* p=dynamic_cast<const IdentifierExpr*>(&e))return env.get(p->name);
        if(auto* p=dynamic_cast<const MemberExpr*>(&e)){const auto v=std::get<V>(expr(*p->object,env));return p->member=="x"?v.x:p->member=="y"?v.y:v.z;}
        if(auto* p=dynamic_cast<const CallExpr*>(&e)){std::vector<Value> values;for(const auto& argument:p->args)values.push_back(expr(*argument,env));return call(callee(*p->callee),std::move(values));}
        if(auto* p=dynamic_cast<const UnaryExpr*>(&e)){auto value=expr(*p->operand,env);if(p->op.type==TokenType::Bang)return !asBool(value);if(p->op.type==TokenType::Plus)return value;
            if(auto* v=std::get_if<I>(&value)){if(*v==INT64_MIN)invalid("integer negation overflow");return -*v;}if(auto* v=std::get_if<D>(&value))return -*v;return -std::get<V>(value);}
        if(auto* p=dynamic_cast<const BinaryExpr*>(&e)){
            if(assignment(p->op.type)){const auto& name=dynamic_cast<const IdentifierExpr&>(*p->left).name;auto value=expr(*p->right,env);auto& destination=env.get(name);
                if(p->op.type!=TokenType::Equal)value=binary(arithmetic(p->op.type),destination,value);destination=convert(type(destination),std::move(value));return destination;}
            auto a=expr(*p->left,env);if(p->op.type==TokenType::And && !asBool(a))return false;if(p->op.type==TokenType::Or && asBool(a))return true;
            return binary(p->op.type,a,expr(*p->right,env));}
        invalid("unreachable unsupported expression");
    }
    void block(const std::vector<StmtPtr>& statements,Environment<Value>& env){charge();env.scopes.emplace_back();
        struct Guard{Environment<Value>& env;~Guard(){env.scopes.pop_back();}} guard{env};for(const auto& s:statements)stmt(*s,env);}
    void stmt(const Stmt& s,Environment<Value>& env){charge();
        if(auto* p=dynamic_cast<const VarDeclStmt*>(&s)){auto value=p->initializer?expr(*p->initializer,env):zero(named(p->typeName));
            env.add(p->name,p->typeName.empty()?value:convert(named(p->typeName),std::move(value)));return;}
        if(auto* p=dynamic_cast<const ExprStmt*>(&s)){expr(*p->expr,env);return;}
        if(auto* p=dynamic_cast<const ReturnStmt*>(&s))throw Flow{Flow::Return,p->value?expr(*p->value,env):Value{}};
        if(auto* p=dynamic_cast<const IfStmt*>(&s)){block(asBool(expr(*p->condition,env))?p->thenBranch:p->elseBranch,env);return;}
        if(auto* p=dynamic_cast<const BlockStmt*>(&s)){block(p->body,env);return;}
        if(dynamic_cast<const BreakStmt*>(&s))throw Flow{Flow::Break,{}};
        if(dynamic_cast<const ContinueStmt*>(&s))throw Flow{Flow::Continue,{}};
        auto loopBody=[&](const auto& body){try{block(body,env);}catch(const Flow& flow){if(flow.kind==Flow::Break)return false;if(flow.kind!=Flow::Continue)throw;}return true;};
        if(auto* p=dynamic_cast<const WhileStmt*>(&s)){while(asBool(expr(*p->condition,env)))if(!loopBody(p->body))break;return;}
        if(auto* p=dynamic_cast<const ForStmt*>(&s)){const auto start=p->start?asInt(expr(*p->start,env)):I{1};const auto end=asInt(expr(*p->bound,env));const auto step=p->step?asInt(expr(*p->step,env)):I{1};
            if(!step)invalid("zero for step");env.scopes.emplace_back();struct Guard{Environment<Value>& e;~Guard(){e.scopes.pop_back();}} guard{env};env.add(p->counterName,start);
            for(I counter=start;p->start?(step>0?counter<end:counter>end):counter<=end;counter=add(counter,step)){
                charge();env.get(p->counterName)=counter;if(!loopBody(p->body))break;}return;}
        invalid("unreachable unsupported statement");
    }
    bool execute(){Environment<Value> env;env.add(defs.entry->params[0].name,ctx.dt());
        try{block(defs.entry->body,env);}catch(const Flow& flow){if(flow.kind!=Flow::Return || type(flow.value)!=Type::Void)invalid("entry flow mismatch");}return true;}
};
}
struct DeterministicLogiaProgram::Impl {std::unique_ptr<logia::Program> code;Definitions defs;DetLogiaOptions options;std::uint64_t hash=0;};
DeterministicLogiaProgram::DeterministicLogiaProgram(std::shared_ptr<const Impl> value):_impl(std::move(value)){}
std::uint64_t DeterministicLogiaProgram::hash() const{return _impl->hash;}
bool DeterministicLogiaProgram::execute(entity::DetTickContext& context) const{return Evaluator(_impl->defs,context,_impl->options).execute();}
std::shared_ptr<const DeterministicLogiaProgram> compileDeterministicLogia(const std::string& source,DetLogiaOptions options,std::string& error){
    try {
        if(source.empty() || source.size()>16384 || !options.maxOperations || options.maxOperations>1000000 || !options.maxCallDepth || options.maxCallDepth>64)invalid("source/options bounds");
        std::vector<Token> tokens;Lexer(source).tokenize(tokens);if(tokens.size()>2048)invalid("token count limit");
        unsigned nesting=0;for(const auto& t:tokens){if(t.type==TokenType::LeftParen || t.type==TokenType::LeftBrace || t.type==TokenType::LeftBracket){if(++nesting>64)invalid("syntax nesting limit");}
            if(t.type==TokenType::RightParen || t.type==TokenType::RightBrace || t.type==TokenType::RightBracket){if(nesting)--nesting;}}
        Parser parser(tokens,true);auto code=parser.parse();if(parser.hasErrors() || !code)invalid(parser.errors().empty()?"parse failure":parser.errors()[0].toString());
        auto data=std::make_shared<DeterministicLogiaProgram::Impl>();data->code=std::move(code);data->defs=definitions(*data->code);data->options=options;
        Checker(data->defs).check();data->hash=replay::fnv1a64(reinterpret_cast<const std::uint8_t*>(source.data()),source.size());
        for(auto value:{DeterministicLogiaProgram::kProfileVersion,options.maxOperations,options.maxCallDepth}){
            std::array<std::uint8_t,4> bytes{};for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(value>>(8*i));data->hash=replay::fnv1a64Combine(data->hash,bytes.data(),bytes.size());}
        error.clear();return std::shared_ptr<const DeterministicLogiaProgram>(new DeterministicLogiaProgram(std::move(data)));
    }catch(const std::exception& e){error=e.what();return {};}
}
bool installDeterministicLogia(entity::DeterministicSession& session,std::uint32_t id,std::int32_t priority,std::shared_ptr<const DeterministicLogiaProgram> program){
    return program && session.registerLogicProfile(id,DeterministicLogiaProgram::kProfileVersion,program->hash())
        && session.registerSystem(id,priority,[program=std::move(program)](auto& context){return program->execute(context);});
}
}
