#pragma once
// artifacts-1 (2026-10-03; DIAGNOSTIC, census frames only): per-draw shader constants and raw texture bytes for the draws a
// census names, to tell apart the causes of the black tree canopies, dark facades, dark road and the horizon band
// (local-analysis/agents/artifacts-1). Nothing here runs outside a census frame, and nothing changes what is drawn.
//
//  files/private-trace/diag-census-constants  (read at census begin) every draw whose vertex or pixel shader key is listed
//      logs its non-zero float constants: "RENDER_CENSUS PCA i=N c81=a,b,c,d ..." (pixel c0-c255) and "VCA" (vertex c0-c255),
//      printed with %.9g (round-trips every finite float exactly; NaN/inf print as nan/inf).
//  files/private-trace/diag-census-textures   (read at census begin) every texture such a draw samples from guest memory
//      (not a resolve destination) is written once per census as files/private-trace/census/aftex-NN-SEQ-sSLOT-PART.bin
//      (parts of at most 60000 bytes: the contained process limits files to 64 KB): an "AFTX" header (version, the six fetch
//      words, slot, base address/bytes, mip address/bytes, plan sizes) then the base bytes then the mip bytes, as the guest
//      holds them (big-endian, tiled). Line "RENDER_CENSUS AFT i=N sS fF WxH base=ADDR/BYTES mip=ADDR/BYTES parts=P".
// Key list format: 16-digit hex keys (the census D line's vs=/ps= values) separated by spaces, commas, semicolons or newlines;
// an empty file selects every draw of the census frame. A missing file disables that part.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace recomp::gpu::generic::afdiag {

struct KeyFilter {
    bool active=false,all=false;std::vector<std::uint64_t> keys; // keys sorted, unique
    bool match(std::uint64_t vs,std::uint64_t ps)const {
        if(!active)return false;
        if(all)return true;
#if defined(RECOMP_AF_MUTANT_VS_ONLY) // test mutant: the pixel key is ignored (a listed pixel shader's draws are missed)
        (void)ps;return std::binary_search(keys.begin(),keys.end(),vs);
#else
        return std::binary_search(keys.begin(),keys.end(),vs) || std::binary_search(keys.begin(),keys.end(),ps);
#endif
    }
};
inline KeyFilter parseKeyFilter(const char* text,std::size_t n) {
    KeyFilter f;f.active=true;
    std::uint64_t v=0;unsigned digits=0;
    const auto flush=[&]{if(digits && digits<=16)f.keys.push_back(v);v=0;digits=0;};
    for(std::size_t i=0;i<n;++i) {
        const char c=text[i];unsigned d;
        if(c>='0' && c<='9')d=unsigned(c-'0');
        else if(c>='a' && c<='f')d=unsigned(c-'a')+10;
        else if(c>='A' && c<='F')d=unsigned(c-'A')+10;
        else if((c=='x' || c=='X') && digits==1 && v==0){digits=0;continue;} // "0x" prefix
        else {flush();continue;}
        v=(v<<4)|d;++digits;
    }
    flush();
    std::sort(f.keys.begin(),f.keys.end());f.keys.erase(std::unique(f.keys.begin(),f.keys.end()),f.keys.end());
    f.all=f.keys.empty();
    return f;
}
inline KeyFilter loadKeyFilter(const char* path) {
    std::FILE* file=std::fopen(path,"rb");
    if(!file)return {};
    std::vector<char> text(65536);const std::size_t n=std::fread(text.data(),1,text.size(),file);std::fclose(file);
    return parseKeyFilter(text.data(),n);
}
inline float wordFloat(std::uint32_t w){float f;std::memcpy(&f,&w,4);return f;}
// The non-zero registers of `regs` float4 constants (4 words each) as " cR=x,y,z,w", `perLine` per line.
inline std::vector<std::string> constantLines(const std::uint32_t* words,unsigned regs,unsigned perLine) {
    std::vector<std::string> out;std::string line;unsigned n=0;
    for(unsigned r=0;r<regs;++r) {
        const auto* w=words+4*r;
        if(!(w[0]|w[1]|w[2]|w[3]))continue;
        char x[112];std::snprintf(x,sizeof x," c%u=%.9g,%.9g,%.9g,%.9g",r,double(wordFloat(w[0])),double(wordFloat(w[1])),
                                  double(wordFloat(w[2])),double(wordFloat(w[3])));
        line+=x;
        if(++n%perLine==0){out.push_back(line);line.clear();}
    }
    if(!line.empty())out.push_back(line);
    return out;
}
struct TexDump {
    std::array<std::uint32_t,6> fetch{};std::uint32_t slot=0,baseAddress=0,mipAddress=0;
    std::uint32_t width=0,height=0,format=0;
};
constexpr std::uint32_t TexDumpMagic=0x58544641u; // "AFTX" little-endian
constexpr std::uint32_t TexDumpVersion=1;
constexpr std::size_t TexDumpHeaderBytes=4*18;
// Header (18 little-endian u32: magic, version, fetch[6], slot, base address, base bytes, mip address, mip bytes, width, height,
// format, payload bytes, 0) then the base bytes then the mip bytes (each as read; a missing span contributes 0 bytes).
inline std::vector<std::uint8_t> packTexture(const TexDump& t,const std::vector<std::uint8_t>* base,const std::vector<std::uint8_t>* mips) {
    const std::size_t b=base?base->size():0,m=mips?mips->size():0;
    const std::array<std::uint32_t,18> h{TexDumpMagic,TexDumpVersion,t.fetch[0],t.fetch[1],t.fetch[2],t.fetch[3],t.fetch[4],t.fetch[5],
        t.slot,t.baseAddress,std::uint32_t(b),t.mipAddress,std::uint32_t(m),t.width,t.height,t.format,std::uint32_t(b+m),0u};
    std::vector<std::uint8_t> out(TexDumpHeaderBytes+b+m);
    std::memcpy(out.data(),h.data(),TexDumpHeaderBytes);
    if(b)std::memcpy(out.data()+TexDumpHeaderBytes,base->data(),b);
    if(m)std::memcpy(out.data()+TexDumpHeaderBytes+b,mips->data(),m);
    return out;
}
// Splits `bytes` into (offset, size) parts of at most `partBytes` (the last may be shorter); an empty input gives no parts.
inline std::vector<std::pair<std::size_t,std::size_t>> parts(std::size_t bytes,std::size_t partBytes) {
    std::vector<std::pair<std::size_t,std::size_t>> out;
    if(!partBytes)return out;
    for(std::size_t at=0;at<bytes;at+=partBytes)out.emplace_back(at,std::min(partBytes,bytes-at));
    return out;
}

} // namespace recomp::gpu::generic::afdiag
