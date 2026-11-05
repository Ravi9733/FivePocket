#include "gta5_tile_extent_publication.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
namespace {
using Bytes=std::vector<std::uint8_t>;
unsigned cases=0;
void put(Bytes& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(24-i*8);}
struct Fixture {
    static constexpr unsigned Command=0xA1000000,Table=0xA2000000,Physical=0x2000000;
    Bytes commands,table;
    std::vector<std::array<unsigned,4>> tiles{{0,0,1280,384},{0,384,1280,720}};
    Fixture() {
        const unsigned words[]{0x80000000,0xC0006000,UINT32_MAX,0xC0032201,0,0x30004,0x5000,0x40000003,
            0xC0006000,0x80000000,0xC0015A01,0x1A,Physical+9,
            0x80000000,0xC0006000,UINT32_MAX,0xC0012201,0,0x84,
            0xC0006000,0x80000000,0xC0015A01,0x1A,Physical+25};
        commands.resize(sizeof(words));for(unsigned i=0;i<std::size(words);++i)put(commands,i*4,words[i]);
        table.resize(40);put(table,0,Gta5TileRecordingDiagnostic::originalLink(Table+36));
        put(table,4,Command);put(table,20,Command+13*4);put(table,36,0xC0000000);
        put(table,16,0x12345678);put(table,32,0xABCD0123); // Z must not be invented.
    }
    auto prepare(){return Gta5TileExtentPublication::prepare({Command,0x1000000,commands},{Table,Physical,table},tiles);}
    auto reader(){return [this](auto at,auto n)->std::optional<Bytes>{
        if(at==Command && n==commands.size())return commands;
        if(at==Table && n==table.size())return table;
        return std::nullopt;
    };}
};
template<class Edit>void reject(Edit edit){Fixture f;edit(f);assert(!f.prepare());++cases;}
}
int main() {
    {Fixture f;auto p=f.prepare();assert(p && p->bindings().size()==2);
        assert(p->bindings()[0].drawOffsetWords==3 && p->bindings()[1].eventOffsetWords==21);
        assert(p->bindings()[1].maskAddress==Fixture::Command+60);
        assert(p->completed()[10]==0 && p->completed()[11]==159 && p->completed()[15]==89);
        assert(std::equal(f.table.begin()+16,f.table.begin()+20,p->completed().begin()+16));
        assert(std::equal(f.table.begin()+32,f.table.begin()+36,p->completed().begin()+32));
        unsigned fences=0,writes=0;
        assert(p->publish(f.reader(),[&]{++fences;return true;},[&](auto at,const auto& before,const auto& after){
            assert(at==Fixture::Table && before==f.table);f.table=after;++writes;return true;}));
        assert(fences==1 && writes==1 && p->phase()==Gta5TileExtentPublication::Phase::Complete);
        assert(!p->publish(f.reader(),[]{return true;},[](auto,const auto&,const auto&){return true;}));++cases;}
    reject([](auto& f){f.tiles.clear();});
    reject([](auto& f){f.tiles[0][2]=0;});
    reject([](auto& f){f.tiles[1][3]=524289;});
    reject([](auto& f){f.tiles.resize(16,{0,0,8,8});});
    reject([](auto& f){put(f.table,0,Fixture::Table+36);}); // Unsealed/C-link mismatch.
    reject([](auto& f){put(f.table,36,0xC3000000);}); // Linked block unsupported.
    reject([](auto& f){f.table.pop_back();});
    reject([](auto& f){put(f.table,4,Fixture::Command+1);});
    reject([](auto& f){put(f.table,4,Fixture::Command-4);});
    reject([](auto& f){put(f.table,20,Fixture::Command);}); // Duplicate.
    reject([](auto& f){put(f.table,4,Fixture::Command+52);put(f.table,20,Fixture::Command);});
    reject([](auto& f){put(f.commands,4,0xC0006001);});
    reject([](auto& f){put(f.commands,8,0x8000000F);}); // Already patched.
    reject([](auto& f){put(f.commands,12,0xC0032200);});
    reject([](auto& f){put(f.commands,16,1);}); // Conditional visibility token.
    reject([](auto& f){put(f.commands,20,0x60004);}); // DMA count mismatch.
    reject([](auto& f){put(f.commands,36,UINT32_MAX);}); // Not first-pass extent.
    reject([](auto& f){put(f.commands,40,0xC0015A00);});
    reject([](auto& f){put(f.commands,44,0x19);});
    reject([](auto& f){put(f.commands,48,Fixture::Physical+8);}); // Wrong endian.
    reject([](auto& f){put(f.commands,48,Fixture::Physical+25);}); // Wrong record.
    reject([](auto& f){put(f.commands,72,0x10084);}); // Zero draw became nonzero.
    reject([](auto& f){const auto n=f.commands.size();f.commands.resize(n+12);put(f.commands,n,0xC0015A01);});
    reject([](auto& f){put(f.commands,4,0xC0003F00);}); // Hidden traversal.
    {Fixture f;assert(!Gta5TileExtentPublication::prepare({Fixture::Command,Fixture::Physical,f.commands},
        {Fixture::Table,Fixture::Physical,f.table},f.tiles));++cases;}
    {Fixture f;assert(!Gta5TileExtentPublication::prepare({Fixture::Command,0x1FFFFFFC,f.commands},
        {Fixture::Table,Fixture::Physical,f.table},f.tiles));++cases;}
    // Failures before/at/after the fence never reach publication, and latch.
    for(unsigned mode=0;mode<8;++mode) {
        Fixture f;auto p=f.prepare();assert(p);const auto table=f.table;unsigned fences=0,writes=0;
        if(mode==0)f.commands[0]^=1;if(mode==1)f.table[12]^=1;
        auto read=f.reader();
        const bool ok=p->publish([&](auto a,auto n)->std::optional<Bytes>{
            if(mode==2)return std::nullopt;if(mode==3)throw 1;return read(a,n);
        },[&]{++fences;if(mode==4)return false;if(mode==5)f.commands[0]^=1;
            if(mode==6)f.table[12]^=1;return true;
        },[&](auto,const auto&,const auto&){++writes;return mode!=7;});
        assert(!ok && p->phase()==Gta5TileExtentPublication::Phase::Failed && writes==unsigned(mode==7));
        assert(fences==unsigned(mode>=4));
        if(mode!=1 && mode!=6)assert(f.table==table);
        assert(!p->publish(read,[]{return true;},[](auto,const auto&,const auto&){return true;}));++cases;
    }
    {Fixture f;auto p=f.prepare();assert(p);unsigned nested=0;
        assert(p->publish(f.reader(),[&]{assert(!p->publish(f.reader(),[]{return true;},{}));++nested;return true;},
            [](auto,const auto&,const auto&){return true;}));assert(nested==1);++cases;}
    {Fixture f;f.tiles={{1,9,8,16},{524280,524280,524288,524288}};auto p=f.prepare();
        assert(p && p->completed()[10]==255 && p->completed()[11]==255);++cases;}
    std::printf("TILE_EXTENT_PUBLICATION_CONTROLS_PASS cases=%u packet_binding=1 xy_conservative=1 z_untouched=1 stale_rejected=1 single_attempt=1 gpu_execution=0\n",cases);
}
