#pragma once
// Attempt v255 (direct_present.h): what the direct present must write to the window, as plain C++ (host-tested by
// tests/direct_present_test.cpp against the app's packNativeRgbaSurface and present_fast.h's gammaInPlace).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace recomp::gpu::direct {
constexpr unsigned SourceWidth=1280,SourceHeight=720;
constexpr std::size_t SourceBytes=std::size_t{SourceWidth}*SourceHeight*4;
struct Layout {std::uint32_t width,height,left,top,imageWidth,imageHeight;};
// native_rgba_surface.h's nativeRgbaSurfaceLayout, the same integer arithmetic.
inline std::optional<Layout> layout(std::uint32_t width,std::uint32_t height) noexcept {
    if(!width || !height || width>4096 || height>4096)return std::nullopt;
    auto w=width,h=height;
    if(width*720<=height*1280)h=std::max(1u,width*720/1280);
    else w=std::max(1u,height*1280/720);
    return Layout{width,height,(width-w)/2,(height-h)/2,w,h};
}
using Lut=std::array<std::uint8_t,768>; // [channel*256+value]
inline Lut identityLut() noexcept {Lut l{};for(unsigned c=0;c<3;++c)for(unsigned v=0;v<256;++v)l[c*256+v]=std::uint8_t(v);return l;}
// The window's bytes for a 1280x720 RGBA source: packNativeRgbaSurface(gammaInPlace(source)) with the source's alpha
// ignored (the renderer's readback set it to 255 before both; the surface's alpha is 255).
inline bool reference(const std::uint8_t* source,const Lut& lut,std::uint32_t width,std::uint32_t height,bool bgra,
                      std::vector<std::uint8_t>& out) {
    const auto l=layout(width,height);
    if(!l || !source)return false;
    out.assign(std::size_t{width}*height*4,0);
    for(std::size_t i=3;i<out.size();i+=4)out[i]=255;
    for(std::uint32_t y=0;y<l->imageHeight;++y)for(std::uint32_t x=0;x<l->imageWidth;++x) {
        const auto s=std::size_t((y*720/l->imageHeight)*1280+x*1280/l->imageWidth)*4;
        const auto d=(std::size_t(y+l->top)*width+x+l->left)*4;
        const std::uint8_t r=lut[source[s]],g=lut[256+source[s+1]],b=lut[512+source[s+2]];
        out[d]=bgra?b:r;out[d+1]=g;out[d+2]=bgra?r:b;
    }
    return true;
}
// v409: the filtered scale (files/private-trace/diag-v409-scale-cubic or -linear; neither: reference() above, byte for byte).
// Each window pixel inside the image filters the gamma-mapped source (the tables applied to every texel first: the picture
// the console scans out, then scaled) around its centre's position in source texels, u = (x + 0.5) * 1280 / imageWidth - 0.5
// (y alike; texels outside the source repeat the edge). Linear: the 2x2 around u. Cubic: Catmull-Rom over the 4x4, the
// result clamped per channel to the 2x2 around u (no halo past an edge's two sides). fp32 in the shader's order; the GPU is
// compared with this at <= 1 per colour channel (fp32 contraction and the float-to-UNORM rounding may differ by one), the
// border and alpha exactly.
enum class Filter : unsigned {Nearest=0,Linear=1,Cubic=2};
inline void cubicWeights(float t,float w[4]) noexcept { // the texels at -1, 0, +1, +2 from floor(u)
    const float t2=t*t,t3=t2*t;
    w[0]=-0.5f*t3+t2-0.5f*t;w[1]=1.5f*t3-2.5f*t2+1.0f;w[2]=-1.5f*t3+2.0f*t2+0.5f*t;w[3]=0.5f*t3-0.5f*t2;
#ifdef RECOMP_V409_MUTANT_WEIGHTS
    std::swap(w[0],w[3]);
#endif
}
inline float mappedTexel(const std::uint8_t* source,const Lut& lut,int x,int y,unsigned c) noexcept {
    x=std::clamp(x,0,int(SourceWidth)-1);y=std::clamp(y,0,int(SourceHeight)-1);
    return float(lut[c*256+source[(std::size_t(y)*SourceWidth+std::size_t(x))*4+c]]);
}
inline float filteredChannel(const std::uint8_t* source,const Lut& lut,Filter f,float ux,float uy,unsigned c) noexcept {
    const float fx=std::floor(ux),fy=std::floor(uy),tx=ux-fx,ty=uy-fy;const int ix=int(fx),iy=int(fy);
    if(f==Filter::Linear) {
        const float a=mappedTexel(source,lut,ix,iy,c),b=mappedTexel(source,lut,ix+1,iy,c);
        const float d=mappedTexel(source,lut,ix,iy+1,c),e=mappedTexel(source,lut,ix+1,iy+1,c);
        const float top=a+(b-a)*tx,bottom=d+(e-d)*tx;
        return top+(bottom-top)*ty;
    }
    float wx[4],wy[4];cubicWeights(tx,wx);cubicWeights(ty,wy);
    float v=0.0f,lo=255.0f,hi=0.0f;
    for(int y=0;y<4;++y) {
        float row=0.0f;
        for(int x=0;x<4;++x) {
            const float s=mappedTexel(source,lut,ix+x-1,iy+y-1,c);
            row+=wx[x]*s;
            if((x==1 || x==2) && (y==1 || y==2)){lo=std::min(lo,s);hi=std::max(hi,s);}
        }
        v+=wy[y]*row;
    }
#ifdef RECOMP_V409_MUTANT_NO_CLAMP
    return v;
#endif
    return std::clamp(v,lo,hi);
}
inline std::uint8_t unorm8(float v) noexcept {return std::uint8_t(std::clamp(int(std::floor(v+0.5f)),0,255));}
inline bool referenceFiltered(const std::uint8_t* source,const Lut& lut,std::uint32_t width,std::uint32_t height,bool bgra,Filter f,
                              std::vector<std::uint8_t>& out) {
    if(f==Filter::Nearest)return reference(source,lut,width,height,bgra,out);
    const auto l=layout(width,height);
    if(!l || !source)return false;
    out.assign(std::size_t{width}*height*4,0);
    for(std::size_t i=3;i<out.size();i+=4)out[i]=255;
    const float sx=float(SourceWidth)/float(l->imageWidth),sy=float(SourceHeight)/float(l->imageHeight);
#ifdef RECOMP_V409_MUTANT_NO_HALF
    constexpr float half=0.0f;
#else
    constexpr float half=0.5f;
#endif
    for(std::uint32_t y=0;y<l->imageHeight;++y) {
        const float uy=(float(y)+0.5f)*sy-half;
        for(std::uint32_t x=0;x<l->imageWidth;++x) {
            const float ux=(float(x)+0.5f)*sx-half;
            const auto d=(std::size_t(y+l->top)*width+x+l->left)*4;
            const std::uint8_t r=unorm8(filteredChannel(source,lut,f,ux,uy,0)),g=unorm8(filteredChannel(source,lut,f,ux,uy,1)),
                b=unorm8(filteredChannel(source,lut,f,ux,uy,2));
            out[d]=bgra?b:r;out[d+1]=g;out[d+2]=bgra?r:b;
        }
    }
    return true;
}
// The source whose scan-out mapping gives the tables: pixel p = (p%256, p%256, p%256, 255).
inline std::vector<std::uint8_t> rampSource() {
    std::vector<std::uint8_t> s(SourceBytes);
    for(std::size_t p=0;p<std::size_t{SourceWidth}*SourceHeight;++p){const auto v=std::uint8_t(p%256);s[p*4]=s[p*4+1]=s[p*4+2]=v;s[p*4+3]=255;}
    return s;
}
// The tables from the mapped rampSource(). False unless the mapping is per channel value (every pixel maps as the pixel
// with its value among the first 256) and keeps alpha: then applying the tables equals applying the mapping.
inline bool lutFromMapped(const std::uint8_t* mapped,std::size_t bytes,Lut& out) {
    if(!mapped || bytes!=SourceBytes)return false;
    for(unsigned c=0;c<3;++c)for(unsigned v=0;v<256;++v)out[c*256+v]=mapped[v*4+c];
    for(std::size_t p=0;p<std::size_t{SourceWidth}*SourceHeight;++p) {
        const auto v=unsigned(p%256);
        if(mapped[p*4]!=out[v] || mapped[p*4+1]!=out[256+v] || mapped[p*4+2]!=out[512+v] || mapped[p*4+3]!=255)return false;
    }
    return true;
}
}
