#include "xenos_vertex_arithmetic.h"
#include "xenos_copy_shader_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
namespace {
Words fetch(unsigned format=38,unsigned swizzle=0x688,unsigned stride=4,unsigned offset=0,
        bool mini=false,unsigned dest=0,unsigned src=0) {
    return {{(src<<5)|(dest<<12)|(1u<<19),swizzle|(format==6?0u:0x3000u)|(format<<16)|(unsigned(mini)<<30),(offset<<8)|stride}};
}
Words math(unsigned op=11,unsigned dest=62,unsigned mask=15,bool exported=true,
        std::array<unsigned,3> sources={0,0,1},std::array<bool,3> temps={true,false,false},
        std::array<unsigned,3> swizzles={0,0,0}) {
    return {{(50u<<26)|dest|(mask<<16)|(unsigned(exported)<<15),
        (swizzles[0]<<16)|(swizzles[1]<<8)|swizzles[2],
        (op<<24)|(sources[0]<<16)|(sources[1]<<8)|sources[2]|(unsigned(temps[0])<<31)|(unsigned(temps[1])<<30)|(unsigned(temps[2])<<29)}};
}
void write(Bytes& bytes,unsigned at,std::uint32_t bits){for(unsigned b=0;b<4;++b)bytes[at+b]=bits>>(24-b*8);}
Bytes basic(Words op=math()) {return program({alloc(1),exec(1,2)|(1u<<16)},{fetch(),op});}
}
int main() {
    Bytes vertices(64);for(unsigned v=0;v<4;++v)for(unsigned lane=0;lane<4;++lane)write(vertices,v*16+lane*4,vertexFloatBits(float(v+lane+2)));
    VertexArithmeticInputs input;input.vertices=vertices.data();input.vertexBytes=vertices.size();
    for(unsigned lane=0;lane<4;++lane){input.constantBits[0][lane]=vertexFloatBits(float(lane+3));input.constantBits[1][lane]=vertexFloatBits(float(lane+1));}
    input.constantKnown[0]=input.constantKnown[1]=15;
    VertexArithmeticResult result;VertexArithmeticError error;
    const auto run=[&](const Bytes& code,const VertexArithmeticInputs& in,unsigned index=0) {
        return evaluateVertexArithmetic(code.data(),code.size(),in,index,result,error);
    };
    assert(run(basic(),input));assert(result.executedCount==2 && result.fetchCount==1 && result.controlCount==2);
    assert(result.exports[62].known==15 && result.constantsRead.count()==2);
    assert((result.exports[62].values==std::array<float,4>{7,14,23,34}));
    assert(run(basic(),input,3));assert((result.exports[62].values==std::array<float,4>{16,26,38,52}));
    const auto sentinel=result;unsigned rejections=0;
    const auto reject=[&](const Bytes& code,const VertexArithmeticInputs& in,unsigned index=0) {
        result=sentinel;assert(!run(code,in,index) && error.failure!=VertexArithmeticFailure::None);
        assert(result.executedCount==sentinel.executedCount && result.fetchCount==sentinel.fetchCount &&
            result.exported==sentinel.exported && result.constantsRead==sentinel.constantsRead &&
            result.controlCount==sentinel.controlCount && result.executedAddresses==sentinel.executedAddresses);
        for(unsigned slot=0;slot<64;++slot)assert(result.exports[slot].known==sentinel.exports[slot].known && result.exports[slot].values==sentinel.exports[slot].values);
        ++rejections;
    };
    reject({},input);reject(Bytes(1),input);reject(Bytes(4104),input);
    for(unsigned mode=0;mode<6;++mode){auto bad=input;
        if(mode==0)bad.vertices=nullptr;if(mode==1)bad.vertexBytes=0;if(mode==2)bad.vertexBytes=3;
        if(mode==3)bad.vertexBytes=4100;if(mode==4)bad.fetchSlot=96;if(mode==5)bad.fetchSlot=1;
        reject(basic(),bad);
    }
    reject(basic(),input,4);reject(basic(),input,0x1000000);
    const auto rounding=std::fegetround();assert(std::fesetround(FE_DOWNWARD)==0);reject(basic(),input);
    assert(std::fesetround(rounding)==0);
#if defined(__aarch64__) || defined(__arm64__)
    std::uint64_t control=0;__asm__ volatile("mrs %0, fpcr" : "=r"(control));
    const auto flushed=control|(std::uint64_t{1}<<24);
    __asm__ volatile("msr fpcr, %0" :: "r"(flushed) : "memory");reject(basic(),input);
    __asm__ volatile("msr fpcr, %0" :: "r"(control) : "memory");assert(vertexArithmeticEnvironmentSupported());
#elif defined(__x86_64__) || defined(_M_X64)
    const auto control=_mm_getcsr();_mm_setcsr(control|(1u<<15)|(1u<<6));reject(basic(),input);
    _mm_setcsr(control);assert(vertexArithmeticEnvironmentSupported());
#endif
    for(unsigned constant=0;constant<2;++constant)for(unsigned lane=0;lane<4;++lane){
        auto bad=input;bad.constantKnown[constant]&=~(1u<<lane);reject(basic(),bad);
        for(auto bits:{1u,0x007FFFFFu,0x80000001u,0x7F800000u,0xFF800000u,0x7FC00001u}){
            bad=input;bad.constantBits[constant][lane]=bits;reject(basic(),bad);
        }
    }
    for(unsigned lane=0;lane<4;++lane)for(auto bits:{1u,0x007FFFFFu,0x80000001u,0x7F800000u,0xFF800000u,0x7FC00001u}){
        auto badBytes=vertices;write(badBytes,lane*4,bits);auto bad=input;bad.vertices=badBytes.data();reject(basic(),bad);
    }
    for(unsigned word=0;word<3;++word)for(unsigned bit=0;bit<32;++bit){
        auto changed=fetch();changed[word]^=1u<<bit;
        const auto data=program({alloc(1),exec(1,2)|(1u<<16)},{changed,math()});
        // Some changes are valid offsets/destinations/mini hints; malformed ones
        // must preserve the same sentinel, rather than presuming every bit rejects.
        result=sentinel;if(!run(data,input)){assert(result.exports[62].values==sentinel.exports[62].values);++rejections;}
    }
    for(unsigned op:{0u,3u,4u,7u,9u,14u,15u})reject(basic(math(op)),input);
    for(auto op:{math(11,32),math(11,62,15,false,{64,0,1}),math(2,62,15,true,{0,1,0},{true,true,true})})reject(basic(op),input);
    for(unsigned w=0;w<2;++w)for(unsigned bit:(w==0?std::initializer_list<unsigned>{6,7,24,25}:std::initializer_list<unsigned>{24,28,29,30,31})){
        auto op=math();op[w]^=1u<<bit;reject(basic(op),input);
    }
    for(const auto& cf:std::vector<std::vector<std::uint64_t>>{{alloc(3),exec()}, {alloc(1),alloc(1),exec(2)},
            {alloc(1),exec(0)}, {alloc(1),exec(2)}, {alloc(1),exec(1,0)}, {alloc(1),exec(1,7)},
            {alloc(1),exec(1,1,3)}, {alloc(1),exec(1,1,1)}, {alloc(1)|(1u<<20),exec()},
            {alloc(1),exec()|(std::uint64_t{1}<<35)}, {1,exec()}})reject(program(cf,{alu(62,8,8,true,true)}),input);
    // ALLOC encoded sizes remain unverified metadata, not export bounds.
    const auto parameters=program({alloc(2,1),exec()},{alu(15,15,15,true,true)});
    assert(run(parameters,input) && result.exports[15].known==15);
    // Every in-place swizzle uses simultaneous operands, not cascading writes.
    for(unsigned swizzle=0;swizzle<256;++swizzle){
        const auto data=program({alloc(1),exec(1,3)|(1u<<16)},
            {fetch(),math(1,0,15,false,{0,0,0},{true,true,true},{swizzle,0,0}),alu(62)});
        assert(run(data,input));
        for(unsigned lane=0;lane<4;++lane)assert(result.exports[62].values[lane]==float(lane+2)*float((((swizzle>>(lane*2))+lane)&3)+2));
    }
    // Packed UNORM conversion and miniature fetch inheritance over an overwritten
    // r0.x, followed by a full fetch which must reject that overwritten index.
    Bytes packed(4);write(packed,0,0xFF804000);auto packedInput=input;packedInput.vertices=packed.data();packedInput.vertexBytes=4;
    assert(run(program({alloc(1),exec(1,2)|(1u<<16)},{fetch(6,0x688,1),alu(62)}),packedInput));
    for(unsigned lane=0;lane<4;++lane)assert(result.exports[62].values[lane]==float((0xFF804000u>>(lane*8))&255)*(1.f/255.f));
    const auto mini=program({alloc(1),exec(1,3)|(5u<<16)},{fetch(),fetch(38,0x688,255,4,true,1,63),alu(62,15,0,true,false,1)});
    assert(run(mini,input));assert((result.exports[62].values==std::array<float,4>{3,4,5,6}));
    reject(program({alloc(1),exec(1,3)|(5u<<16)},{fetch(),fetch(38,0x688,4,0,false,1),alu(62,15,0,true,false,1)}),input);
    // Explicit unfused result and zero-product policy; arithmetic over/underflow.
    auto numeric=input;Bytes numbers=vertices;numeric.vertices=numbers.data();
    for(unsigned lane=0;lane<4;++lane){write(numbers,lane*4,0x3F800001);numeric.constantBits[0][lane]=0x3F7FFFFE;numeric.constantBits[1][lane]=0xBF800000;}
    assert(run(basic(),numeric));for(float v:result.exports[62].values)assert(vertexFloatBits(v)==0);
    write(numbers,0,0x80000000);numeric.constantBits[0][0]=vertexFloatBits(-5.f);numeric.constantBits[1][0]=0;
    assert(run(basic(),numeric) && vertexFloatBits(result.exports[62].values[0])==0);
    for(auto operands:std::array<std::array<unsigned,2>,2>{{{{0x7F000000,0x40800000}},{{0x00800000,0x3F000000}}}}){
        write(numbers,0,operands[0]);numeric.constantBits[0][0]=operands[1];reject(basic(),numeric);
    }
    // Exact execution/control boundaries with constant exports, plus hostile data.
    std::vector<std::uint64_t> controls(128);controls[0]=alloc(1);controls[127]=exec(64);
    assert(run(program(controls,{alu(62,8,8,true,true)}),input) && result.controlCount==128);
    controls.resize(130);controls[127]=0;controls[129]=exec(65);reject(program(controls,{alu(62,8,8,true,true)}),input);
    controls={alloc(1)};for(unsigned i=0;i<42;++i)controls.push_back(exec(23,6,1));controls.push_back(exec(23,4));
    std::vector<Words> body(7,alu(62,8,8,true,true));assert(run(program(controls,body),input) && result.executedCount==256);
    controls.back()=exec(23,5);reject(program(controls,body),input);
    controls={alloc(1)};for(unsigned i=0;i<5;++i)controls.push_back(exec(4,6,1)|(0x555u<<16));
    controls.push_back(exec(8,3)|(5u<<16));body.assign(6,fetch(38,0x688,4,0,false,1));body.push_back(alu(62,15,0,true,false,1));
    assert(run(program(controls,body),input) && result.fetchCount==32);
    controls.back()=exec(7,4)|(0x15u<<16);reject(program(controls,body),input);
    // Unknown KEEP lanes and partial exports stay unknown, never guessed zero.
    assert(run(program({alloc(1),exec(1,2)|(1u<<16)},{fetch(),alu(62,3)}),input));
    assert(result.exports[62].known==3);
    reject(program({exec()},{alu(62,15,15,true,true)}),input);
    auto invalidKnown=input;invalidKnown.constantKnown[0]=31;reject(basic(),invalidKnown);
    std::uint32_t seed=0x4A819357;
    for(unsigned n=0;n<10000;++n){Bytes data((n%340+1)*12);for(auto& b:data){seed=seed*1664525u+1013904223u;b=seed>>24;}
        (void)run(data,input);}
    std::printf("VERTEX_ARITHMETIC: %u rejection controls; 256 swizzles, saved mini index, unfused arithmetic, 10000 hostile streams PASS\n",rejections);
}
