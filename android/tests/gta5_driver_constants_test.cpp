#include "gta5_driver_constants.h"
#include <cassert>
#include <map>
#include <cstdio>

int main() {
    using namespace recomp::gpu;
    std::vector<std::uint8_t> memory(8192);
    const auto put=[&](unsigned at,std::uint32_t value) {
        for(unsigned b=0;b<4;++b)memory.at(at+b)=static_cast<std::uint8_t>(value>>(24-8*b));
    };
    const Gta5ResourceReader heap=[&](std::uint32_t address,std::uint32_t size)
            ->std::optional<std::vector<std::uint8_t>> {
        if(address<0x1000 || std::uint64_t{address}-0x1000+size>memory.size())return std::nullopt;
        return std::vector<std::uint8_t>(memory.begin()+address-0x1000,memory.begin()+address-0x1000+size);
    };
    put(20,0x100);put(0x110,12); // One entry plus a four-byte zero terminator.
    put(0x114,(511u<<16)|4);put(0x118,0x300);
    put(0x304,0x3F800000);
    Gta5DriverConstants state;
    Gta5InlineDraw draw;
    assert(state.loadLiterals(0x1000,0x1000,heap,heap));
    state.apply(draw);
    assert(draw.pixelConstants[255*4+1]==0x3F800000 && state.size()==4);
    // A later application upload really wins over an earlier shader literal.
    put(0x1000+255*16+4,0x40000000);
    assert(state.upload(1,0x4400,0x2000,heap));
    state.apply(draw);assert(draw.pixelConstants[255*4+1]==0x40000000);
    // Rebinding the shader loads its constants again, in order.
    assert(state.loadLiterals(0x1000,0x1000,heap,heap));
    state.apply(draw);assert(draw.pixelConstants[255*4+1]==0x3F800000);
    put(0x114,(511u<<16)|8); // Cannot spill beyond the two float banks.
    assert(!state.loadLiterals(0x1000,0x1000,heap,heap));
    assert(!state.upload(1,0x47FF,0x2000,heap));
    assert(!state.loadLiterals(0xFFFFFFF0,0x1000,heap,heap));
    state.apply(draw);assert(draw.pixelConstants[255*4+1]==0x3F800000);
    // Sparse/full masks, updates and missing reads must preserve exact
    // assignment semantics, including the untouched values of unsubmitted slots.
    Gta5DriverConstants compact;std::map<unsigned,std::uint32_t> reference;
    std::uint32_t randomState=0x17AD9261u;unsigned seed=0;
    const auto random=[&]{randomState=randomState*1664525u+1013904223u;return randomState;};
    const auto value=[&](unsigned address){return (address*0x9E3779B9u)^seed;};
    unsigned reads=0,failAt=UINT32_MAX;
    const Gta5ResourceReader synthetic=[&](unsigned address,unsigned size)->std::optional<std::vector<std::uint8_t>> {
        assert(size==64);if(reads++==failAt)return std::nullopt;
        std::vector<std::uint8_t> bytes(size);
        for(unsigned i=0;i<16;++i)for(unsigned b=0;b<4;++b)bytes[i*4+b]=value(address+i*4)>>(24-8*b);
        return bytes;
    };
    const auto verify=[&] {
        Gta5InlineDraw result;result.vertexConstants.fill(0xBAD00BAD);result.pixelConstants.fill(0xBAD00BAD);
        compact.apply(result);assert(compact.size()==reference.size());
        for(unsigned i=0;i<2048;++i) {
            const auto found=reference.find(i);const auto expected=found==reference.end()?0xBAD00BAD:found->second;
            assert((i<1024?result.vertexConstants[i]:result.pixelConstants[i-1024])==expected);
        }
    };
    verify();
    for(unsigned iteration=0;iteration<256;++iteration) {
        const std::uint64_t mask=iteration<2?std::uint64_t{1}<<(63-iteration):
            iteration%17?((std::uint64_t{random()}<<32)|random()):UINT64_MAX;
        const unsigned first=iteration%2?0x4400:0x4000;seed=random();reads=0;
        assert(compact.upload(mask,first,0x10000,synthetic));
        for(unsigned bit=0;bit<64;++bit)if(mask&(std::uint64_t{1}<<(63-bit)))for(unsigned i=0;i<16;++i)
            reference[first-0x4000+bit*16+i]=value(0x10000+bit*64+i*4);
        verify();
        seed=random();reads=0;failAt=7;assert(!compact.upload(UINT64_MAX,first,0x10000,synthetic));verify();failAt=UINT32_MAX;
    }
    reads=0;assert(!compact.upload(UINT64_MAX,0x47FF,0x10000,synthetic));verify();
    assert(!compact.upload(UINT64_MAX,0x4000,0xFFFFFFC0,synthetic));verify();
    // Literal tables also stage multiple writes atomically. Exercise overlap,
    // both banks, and a failure after the first entry has already been read.
    put(0x110,28);put(0x12C,0);
    for(unsigned iteration=0;iteration<32;++iteration) {
        const unsigned firsts[3]{iteration*4,1024+iteration*4,iteration*4};
        for(unsigned entry=0;entry<3;++entry) {
            put(0x114+entry*8,((firsts[entry]/4)<<16)|4);
            put(0x118+entry*8,0x500+entry*16);
            for(unsigned i=0;i<4;++i)put(0x500+entry*16+i*4,random());
        }
        unsigned literalReads=0;
        const Gta5ResourceReader partial=[&](unsigned address,unsigned size)->std::optional<std::vector<std::uint8_t>> {
            if(++literalReads==2)return std::nullopt;return heap(address,size);
        };
        assert(!compact.loadLiterals(0x1000,0x1000,heap,partial));verify();
        assert(compact.loadLiterals(0x1000,0x1000,heap,heap));
        for(unsigned entry=0;entry<3;++entry)for(unsigned i=0;i<4;++i) {
            const unsigned at=0x500+entry*16+i*4;
            reference[firsts[entry]+i]=(std::uint32_t{memory[at]}<<24)|(std::uint32_t{memory[at+1]}<<16)|
                (std::uint32_t{memory[at+2]}<<8)|memory[at+3];
        }
        verify();put(0x124,(511u<<16)|8);
        assert(!compact.loadLiterals(0x1000,0x1000,heap,heap));verify();
    }
    std::puts("GTA5_DRIVER_CONSTANTS_PASS mixed_updates=256 literal_tables=32 failed_transactions_preserved=1 sparse_presence_preserved=1");
}
