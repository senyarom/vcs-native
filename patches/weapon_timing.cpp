#include "weapon_timing.hpp"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vcs::patches {
namespace {
constexpr double period=1.0/30.0;
constexpr double epsilon=0.00005; // timer tick / floating point rounding only
NpcFireCadence cadence;
std::uint32_t previous_game_ms{};
bool have_game_ms{};
bool diagnostic{}, disabled{};

void vehicle_damage_probe(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    auto &m=rt.memory(); const auto gp=ctx.gpr[28];
    const auto script=m.load32(gp-29148);
    if (m.contains(script, 5724*4) && m.contains(ctx.gpr[4],640)) {
        std::fprintf(stderr,"[damage-probe] ms=%u dt=%.6f stage=%u caller=%08x target=%08x source=%08x weapon=%u damage=%.6f hp=%.6f\n",
            m.load32(gp+7660), std::bit_cast<float>(m.load32(gp+7676)), m.load32(script+5723*4),
            ctx.gpr[31],ctx.gpr[4],ctx.gpr[5],ctx.gpr[6],ctx.fpr[12],std::bit_cast<float>(m.load32(ctx.gpr[4]+636)));
    }
    // Preserve the original prologue; resume at its first basic-block branch.
    ctx.gpr[29]-=48;
    ctx.gpr[8]=static_cast<std::int32_t>(static_cast<std::int8_t>(m.load8(ctx.gpr[4]+615)));
    m.store32(ctx.gpr[29]+12,ctx.gpr[16]);ctx.gpr[16]=ctx.gpr[4];
    m.store32(ctx.gpr[29]+4,std::bit_cast<std::uint32_t>(ctx.fpr[20]));
    m.store32(ctx.gpr[29]+16,ctx.gpr[17]);m.store32(ctx.gpr[29]+20,ctx.gpr[18]);
    ctx.fpr[20]=ctx.fpr[12];ctx.gpr[4]=ctx.gpr[8]&64;ctx.gpr[17]=ctx.gpr[5];ctx.gpr[18]=ctx.gpr[6];
    m.store32(ctx.gpr[29]+8,std::bit_cast<std::uint32_t>(ctx.fpr[22]));
    for(unsigned i=19;i<=22;++i)m.store32(ctx.gpr[29]+24+(i-19)*4,ctx.gpr[i]);
    m.store32(ctx.gpr[29]+40,ctx.gpr[31]);ctx.gpr[19]=ctx.gpr[7];
    ctx.pc=ctx.gpr[4]?0x08B04578:0x08B04590;
}

void fire_hook(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    auto &m=rt.memory();
    const auto gp=ctx.gpr[28], weapon=ctx.gpr[4], owner=ctx.gpr[5];
    const bool simplified=ctx.gpr[31]==0x089126DC || ctx.gpr[31]==0x08912804;
    bool allow=true;
    if(simplified && m.contains(weapon,28) && m.contains(owner,1930)) {
        const auto kind=m.load32(weapon+4);
        const auto tables=m.load32(gp+10576);
        if(kind<64 && m.contains(tables,8)) {
            const auto table=m.load32(tables+(m.load8(gp-7080)?0:4));
            const auto info=table+kind*112;
            // Instant-hit firearms only. Area effects and melee have their own
            // continuous damage semantics; do not silently change those here.
            if(m.contains(info,112) && m.load32(info+4)==1 && !disabled)
                allow=cadence.allow((std::uint64_t(owner)<<32)|weapon);
        }
    }
    if(diagnostic && m.contains(weapon,28))
        std::fprintf(stderr,"[fire-probe] ms=%u time=%.6f dt=%.6f caller=%08x source=%08x weapon=%u state=%u ammo=%u timer=%u allow=%u\n",
            m.load32(gp+7660),cadence.seconds(),std::bit_cast<float>(m.load32(gp+7676)),ctx.gpr[31],owner,m.load32(weapon+4),m.load32(weapon+8),m.load32(weapon+12),m.load32(weapon+20),allow);
    if(!allow) {ctx.set_gpr(2,0);ctx.pc=ctx.gpr[31];return;}
    // Original 08A45338..08A45390 prologue, then unchanged AOT. No nested
    // execution, no temporary changes to the global timestep or guest code.
    ctx.gpr[29]-=208;
    m.store32(ctx.gpr[29]+172,std::bit_cast<std::uint32_t>(ctx.fpr[20]));
    m.store32(ctx.gpr[29]+176,std::bit_cast<std::uint32_t>(ctx.fpr[22]));
    for(unsigned i=16;i<=21;++i)m.store32(ctx.gpr[29]+180+(i-16)*4,ctx.gpr[i]);
    m.store32(ctx.gpr[29]+204,ctx.gpr[31]);
    ctx.gpr[19]=0;ctx.gpr[20]=1;ctx.fpr[20]=0;
    m.store32(ctx.gpr[29],0);m.store32(ctx.gpr[29]+4,0);
    ctx.gpr[7]=0x3f19999a;ctx.fpr[12]=std::bit_cast<float>(ctx.gpr[7]);
    m.store32(ctx.gpr[29]+8,ctx.gpr[7]);
    ctx.gpr[16]=ctx.gpr[4];ctx.gpr[17]=ctx.gpr[5];ctx.gpr[18]=ctx.gpr[6];
    ctx.pc=ctx.gpr[5]?0x08A45394:0x08A45408;
}
}
void NpcFireCadence::advance(double seconds) {
    if(std::isfinite(seconds) && seconds>0 && seconds<=1) seconds_+=seconds;
}
bool NpcFireCadence::allow(std::uint64_t key) {
    if(shots_.size()>1024) shots_.clear();
    auto [it,inserted]=shots_.try_emplace(key);
    auto &shot=it->second;
    // A new burst can fire immediately; unused idle time never builds up a
    // backlog of extra bullets. Preserve the fractional phase inside a burst.
    if(inserted || seconds_-shot.last_attempt>period+epsilon) shot.next=seconds_;
    if(shot.last_attempt==seconds_) return false;
    shot.last_attempt=seconds_;
    if(seconds_+epsilon<shot.next) return false;
    shot.next+=period;
    return true;
}
void NpcFireCadence::reset() {seconds_=0;shots_.clear();}
void install_weapon_timing(psprecomp::Runtime &rt) {
    cadence.reset();have_game_ms=false;
    diagnostic=std::getenv("PSPRECOMP_DAMAGE_PROBE")!=nullptr;
    disabled=std::getenv("PSPRECOMP_DISABLE_NPC_FIRE_FIX")!=nullptr;
    rt.register_function(0x08A45338,fire_hook,"vcs_npc_weapon_timing");
    if(diagnostic)rt.register_function(0x08B0452C,vehicle_damage_probe,"vcs_damage_probe");
}
void advance_weapon_timing(psprecomp::Runtime &rt,std::uint32_t gp) {
    auto &m=rt.memory();if(gp<29148 || !m.contains(gp-29148,40000))return;
    const auto ms=m.load32(gp+7660);
    if(have_game_ms && ms<previous_game_ms && previous_game_ms-ms<0x80000000u) cadence.reset();
    const auto step=std::bit_cast<float>(m.load32(gp+7676));
    if(!have_game_ms || ms!=previous_game_ms) cadence.advance(double(step)/50.0);
    previous_game_ms=ms;have_game_ms=true;
    // Read-only per-frame telemetry for 30/60 FPS swimming-combat comparisons.
    // Deliberately does not pin position, movement state, health or stamina.
    if(diagnostic) {
        const auto player=m.load32(0x08BDE4B0u+352u*m.load8(gp+7800));
        if(player>=0x08800000u && m.contains(player,0xCC8)) {
            auto f=[&](unsigned offset){return std::bit_cast<float>(m.load32(player+offset));};
            const auto script=m.load32(gp-29148);
            const auto stage=m.contains(script,5353*4)?m.load32(script+5352*4):0;
            std::fprintf(stderr,"[swim-probe] ms=%u dt=%.6f player=%08x stage=%u hp=%.6f armor=%.6f stamina=%.9f sprint=%.6f water=%u state=%u ped_type=%u x=%.6f y=%.6f z=%.6f vx=%.9f vy=%.9f vz=%.9f\n",
                ms,step,player,stage,f(0x4E4),f(0x4E8),f(0x82C),f(0xCC0),
                (m.load32(player+0xEC)>>8)&1,m.load32(player+0x8B4),m.load32(player+0x550),
                f(0x30),f(0x34),f(0x38),f(0x140),f(0x144),f(0x148));
        }
    }
}
} // namespace vcs::patches
