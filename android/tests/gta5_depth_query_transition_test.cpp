#include "gta5_depth_query_transition.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
// Authored state and packets, not captured game data or a GPU result.
static void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=std::uint8_t(value>>(24-i*8));
}
int main() {
    unsigned negatives=0;
    for(bool end:{false,true}) {
        std::vector<std::uint8_t> d(13860),q(156),samples(64,0xA5),cmd(36);
        put(d,16,0x10);put(d,20,2);put(d,48,0x20000);put(d,56,0x30000);put(d,11036,10);
        put(q,0,0x10000);put(q,4,9);put(q,28,0xA0100000);put(q,148,1);put(q,152,1);q[16]=end?2:0;
        auto da=d,qa=q,sa=samples;
        put(da,16,0x03000010);put(da,20,10);put(da,48,0x20024);qa[16]=end?1:2;
        if(end){qa[20]=0x40;put(qa,24,10);put(sa,16,0xFFFFFEED);put(sa,20,0xFFFFFEED);}
        const std::uint32_t words[]={0x2208,6,0x12000,0x20000,0,0x2325,end?0x100000u:0x100020u,0xC0005B00,21};
        for(unsigned i=0;i<9;++i)put(cmd,i*4,words[i]);
        const auto check=[&](std::uint32_t virt=0xA0100000,std::uint32_t phys=0x100000) {
            return describeGta5DepthQueryTransition(end,0x10000,virt,phys,d,da,q,qa,samples,sa,cmd);
        };
        const auto good=check();assert(good && good->end==end && good->snapshotPhysical==words[6]);
        assert(good->commandBegin==0x20004 && good->commandLastWord==0x20024 && good->submittedFence==(end?10u:0u));
        if(!end) {
            // Original constructor-style first issue, independently replayed
            // with captured before/after state in the private original probe.
            q[16]=1;assert(check());
            for(unsigned flag:{1u,0x40u,0x80u}) {
                q[20]=qa[20]=flag;assert(!check());++negatives;
            }
            q[20]=qa[20]=0;
            put(q,24,4);put(qa,24,4);assert(!check());++negatives;
            put(q,24,0);put(qa,24,0);
            for(unsigned issue:{2u,3u}){q[16]=issue;assert(!check());++negatives;}
            q[16]=0;
        }
        for(unsigned at:{0u,16u,20u,48u,56u,11036u,13859u}){da[at]^=1;assert(!check());da[at]^=1;++negatives;}
        for(unsigned at:{0u,4u,16u,20u,24u,28u,148u,152u,155u}){qa[at]^=1;assert(!check());qa[at]^=1;++negatives;}
        for(unsigned i=0;i<cmd.size();++i){cmd[i]^=1;assert(!check());cmd[i]^=1;++negatives;}
        for(unsigned i=0;i<sa.size();++i){sa[i]^=1;assert(!check());sa[i]^=1;++negatives;}
        for(auto phys:{1u,0x100020u,0x1FFFFFE0u,0x20000000u,0xFFFFFFE0u}){assert(!check(0xA0100000,phys));++negatives;}
        for(auto virt:{0u,1u,0xA0100020u,0xFFFFFFE0u}){assert(!check(virt));++negatives;}
        d[11068]=0x20;assert(!check());d[11068]=0;++negatives;
        put(d,56,0x1FFFC);assert(!check());put(d,56,0x30000);++negatives;
        q[20]=1;assert(!check());q[20]=0;++negatives;
        put(q,148,2);assert(!check());put(q,148,1);++negatives;
        put(q,152,2);assert(!check());put(q,152,1);++negatives;
        put(q,24,4);assert(!check());put(q,24,0);++negatives; // unread prior END needs different ordered packets
        cmd.pop_back();assert(!check());++negatives;
    }
    assert(negatives==270);
    std::printf("GTA5_DEPTH_QUERY_TRANSITION_PASS positives=3 negatives=%u bounds=1 full_state=1 unread_prior_refuses=1 execution=0\n",negatives);
}
