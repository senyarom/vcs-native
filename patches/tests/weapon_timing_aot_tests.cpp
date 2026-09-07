#include "weapon_timing.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
namespace psprecomp {void register_generated_unit_67(Runtime&);void register_generated_unit_144(Runtime&);}
namespace {
constexpr unsigned gp=0x08BB1D60,owner=0x09000000,weapon=owner+1396,stack=0x09FE0000,table_ptr=0x09100000,table=table_ptr+16;
void require(bool ok,const char *s){if(!ok)throw std::runtime_error(s);}
void setup(psprecomp::Runtime &rt){
 psprecomp::register_generated_unit_67(rt);psprecomp::register_generated_unit_144(rt);
 rt.register_function(0x08B60E84,[](auto&,auto&){throw std::runtime_error("helper should yield without execution");},"test-prologue-boundary");
 auto&m=rt.memory();m.store32(gp+10576,table_ptr);m.store32(table_ptr+4,table);
 m.store32(table+25*112+4,1);m.store32(owner+72,6);m.store8(owner+1929,0);
 m.store32(weapon+4,25);m.store32(weapon+8,0);m.store32(weapon+12,30);m.store32(owner+1252,100);
}
psprecomp::AllegrexContext context(){
 psprecomp::AllegrexContext ctx{};for(unsigned i=1;i<32;++i)ctx.gpr[i]=0x12340000+i;
 for(unsigned i=0;i<32;++i)ctx.fpr[i]=float(i)+.25f;
 ctx.gpr[4]=weapon;ctx.gpr[5]=owner;ctx.gpr[6]=stack+512;ctx.gpr[28]=gp;ctx.gpr[29]=stack;ctx.gpr[31]=0x089126DC;return ctx;
}
}
void test_swimming_timing();
void test_swimming_drag();
int main(){try{
 test_swimming_timing();
 test_swimming_drag();
 psprecomp::Runtime baseline,patched;setup(baseline);setup(patched);vcs::patches::install_weapon_timing(patched);
 auto a=context(),b=a;
 require(baseline.invoke_isolated_aot(0x08A45338,a),"original Weapon::Fire exists");
 require(a.pc==0x08B60E84,"original prologue reaches first cross-unit helper");
 require(patched.invoke_isolated_aot(0x08A45338,b),"patched Weapon::Fire exists");
 require(b.pc==0x08A45394,"allowed shot resumes unchanged original block");
 require(patched.invoke_isolated_aot(b.pc,b),"original continuation exists");
 require(a.gpr==b.gpr && a.pc==b.pc,"allowed path preserves all original registers");
 for(unsigned i=0;i<32;++i)require(std::bit_cast<unsigned>(a.fpr[i])==std::bit_cast<unsigned>(b.fpr[i]),"allowed path preserves original FPRs");
 require(baseline.memory().bytes()==patched.memory().bytes(),"allowed path preserves every original memory write");

 auto c=context();const auto before=patched.memory().bytes();
 require(patched.invoke_isolated_aot(0x08A45338,c),"repeat shot reaches gate");
 require(c.pc==0x089126DC && c.gpr[2]==0 && c.gpr[29]==stack,"extra shot returns false before touching stack");
 require(patched.memory().bytes()==before,"extra shot cannot consume ammo, trace or damage anything");

 // Enter the actual simplified NPC call site with direct chaining enabled.
 // It must dispatch the override instead of jumping around it.
 c=context();c.gpr[17]=owner;c.gpr[31]=0x1234;
 require(patched.invoke_isolated_aot(0x089127E0,c),"real NPC call site exists");
 require(c.pc==0x08A45338 && c.gpr[31]==0x08912804,"actual NPC cross-unit call reaches replacement");
 require(patched.invoke_isolated_aot(c.pc,c) && c.pc==0x08912804,"second NPC call site shares per-weapon gate");

 // Player/animation-driven callers are never gated, even in the same frame.
 for(unsigned ra:{0x088E6438u,0x0894F7D8u}){
  c=context();c.gpr[31]=ra;
  require(patched.invoke_isolated_aot(0x08A45338,c) && c.pc==0x08A45394,"other firing paths bypass the NPC gate");
 }
 std::cout<<"Actual NPC AOT call, Weapon::Fire prologue, skipped-shot side effects and caller scope passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
