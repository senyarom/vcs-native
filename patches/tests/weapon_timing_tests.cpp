#include "weapon_timing.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using vcs::patches::NpcFireCadence;
void require(bool b,const char *s){if(!b)throw std::runtime_error(s);}
int main(){try{
    for(auto fps:{30,60,90,120,144,240,360}) {
        NpcFireCadence c;int fired=0;
        for(int i=0;i<fps*10;++i){if(c.allow(1))++fired;c.advance(1./fps);}
        require(fired==300,"ten simulated seconds must produce 300 shots independent of FPS");
    }
    NpcFireCadence c;std::vector<double> times;
    for(auto fps:{60,30,120,30,60})for(int i=0;i<fps*2;++i){if(c.allow(1))times.push_back(c.seconds());c.advance(1./fps);}
    require(times.size()==300,"live FPS changes preserve cadence without entity health changes");
    for(unsigned i=1;i<times.size();++i)require(times[i]-times[i-1]>.016,"switching never emits a double shot");
    for(auto fps:{30,60,120}){
        NpcFireCadence burst;int count=0;
        for(int i=0;i<fps*10;++i){if(i%fps<fps/10 && burst.allow(1))++count;burst.advance(1./fps);}
        require(count==30,"100 ms bursts have the same shot count, with no idle backlog");
    }
    c.reset();require(c.allow(1),"new actor fires immediately");require(!c.allow(1),"same frame cannot duplicate a shot");
    require(c.allow(2),"separate actors do not share a cooldown");
    c.advance(0);c.advance(-1);c.advance(std::numeric_limits<double>::quiet_NaN());
    require(!c.allow(1),"pause and invalid time never advance firing");
    c.advance(.5);require(c.allow(1) && !c.allow(1),"long gap allows one shot, no catch-up burst");
    c.reset();require(c.allow(1),"restart discards old cadence state");
    std::cout<<"NPC firing cadence: 30-360 FPS, live transitions, bursts, pause and restart passed\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
