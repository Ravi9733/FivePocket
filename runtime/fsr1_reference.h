#pragma once
// agent fsr1 (v450): what the presenter's FSR 1 scaler (diag-v450-scale-fsr1) must write, as plain C++ for its audit
// (RENDER_DIRECT_AUDIT ... fsr=N). A port of AMD's FsrEasuF and FsrRcasF (ffx_fsr1.h, 32-bit paths; MIT, see fsr1_cpu.h) in the
// headers' fp32 operation order, with AMD's bit-pattern approximations (APrxLoRcpF1, APrxLoRsqF1, APrxMedRcpF1) as defined in
// ffx_a.h. EASU's twelve taps are fetched by integer position (b c / e f g h / i j k l / n o around floor(pp)) instead of
// through the shader's four gathers per channel: the host test (tools/fsr1/fsr1_test.cpp) runs AMD's own header text through a
// GLSL-as-C++ shim with Vulkan gather rules and requires bit-identical fp32 results, so it checks the gather plumbing too.
// Choices where GLSL leaves freedom, made as Adreno is expected to (the device audit decides): min / max return the non-NaN
// operand (IEEE maxNum; RCAS's limiters compute 0 * inf = NaN where a channel's ring is all 0 or all 1), division is IEEE, no
// fused multiply-add (#pragma below; the GPU may fuse: the audit tolerance covers it), UNORM8 texels convert as k / 255 and
// results as round(v * 255) with NaN -> 0.
// Device audit (two stages for EASU + RCAS): (A) the GPU's EASU picture (read back) against easuPicture() of the frame's mapped
// source, (B) the window against RCAS of the GPU's own EASU picture, each <= 1 per colour channel, alpha and border exact; an
// off-by-one in the 8-bit picture would otherwise reach the window amplified by RCAS (up to 1 / (1 + 4 lobe) = 4x at maximum
// sharpness). EASU only: the window against easuPicture() placed in the rect. EASU tie pixels (see easuPixel) may also lie
// anywhere within their 2x2 clamp bounds +/- 1 (counted separately: tieRange).
#include "direct_present_reference.h"
#include "fsr1_cpu.h"
#include <cstring>

namespace recomp::gpu::direct::fsr1 {
inline float asFloat(std::uint32_t u) noexcept {float f;std::memcpy(&f,&u,4);return f;}
inline std::uint32_t asUint(float f) noexcept {std::uint32_t u;std::memcpy(&u,&f,4);return u;}
#ifdef RECOMP_FSR1_MUTANT_NAN // (host test mutant: the GLSL formula x < y ? y : x, which keeps a NaN x)
inline float fmaxN(float a,float b) noexcept {return a<b?b:a;}
inline float fminN(float a,float b) noexcept {return b<a?b:a;}
#else
inline float fmaxN(float a,float b) noexcept {return std::fmax(a,b);} // GLSL max as maxNum
inline float fminN(float a,float b) noexcept {return std::fmin(a,b);}
#endif
inline float sat(float x) noexcept {return fminN(fmaxN(x,0.0f),1.0f);} // ASatF1 = clamp(x, 0, 1) = min(max(x, 0), 1)
inline float prxLoRcp(float a) noexcept {return asFloat(0x7ef07ebbu-asUint(a));}
inline float prxLoRsq(float a) noexcept {return asFloat(0x5f347d74u-(asUint(a)>>1));}
inline float prxMedRcp(float a) noexcept {
#pragma clang fp contract(off)
    const float b=asFloat(0x7ef19fffu-asUint(a));return b*(-b*a+2.0f);
}
// A mapped (tables applied) RGBA8 picture's texel as the shader samples it: clamped to the edge, k / 255.
inline float texel(const std::uint8_t* rgba,int width,int height,int x,int y,int c) noexcept {
    x=std::clamp(x,0,width-1);y=std::clamp(y,0,height-1);
    return float(rgba[(std::size_t(y)*unsigned(width)+unsigned(x))*4+unsigned(c)])/255.0f;
}
struct Rgb {float r,g,b;};
// FsrEasuSetF for one bilinear corner (w = its weight): a above, b left, c centre, d right, e below.
inline void easuSet(float& dirX,float& dirY,float& len,float w,float lA,float lB,float lC,float lD,float lE) noexcept {
#pragma clang fp contract(off)
    const float dc=lD-lC,cb=lC-lB;
    float lenX=fmaxN(std::fabs(dc),std::fabs(cb));
    lenX=prxLoRcp(lenX);
    const float dX=lD-lB;
    dirX+=dX*w;
    lenX=sat(std::fabs(dX)*lenX);
    lenX*=lenX;
    len+=lenX*w;
    const float ec=lE-lC,ca=lC-lA;
    float lenY=fmaxN(std::fabs(ec),std::fabs(ca));
    lenY=prxLoRcp(lenY);
    const float dY=lE-lA;
    dirY+=dY*w;
    lenY=sat(std::fabs(dY)*lenY);
    lenY*=lenY;
    len+=lenY*w;
}
// FsrEasuTapF: one tap at offset (offX, offY) from the resolve position.
inline void easuTap(float aC[3],float& aW,float offX,float offY,float dirX,float dirY,float lenX,float lenY,float lob,float clp,
                    const float c[3]) noexcept {
#pragma clang fp contract(off)
    float vX=(offX*dirX)+(offY*dirY);
    float vY=(offX*(-dirY))+(offY*dirX);
    vX*=lenX;vY*=lenY;
    float d2=vX*vX+vY*vY;
    d2=fminN(d2,clp);
    float wB=float(2.0/5.0)*d2+(-1.0f);
    float wA=lob*d2+(-1.0f);
    wB*=wB;
    wA*=wA;
    wB=float(25.0/16.0)*wB+float(-(25.0/16.0-1.0));
    const float w=wB*wA;
    for(int k=0;k<3;++k)aC[k]+=c[k]*w;
    aW+=w;
}
// FsrEasuF at output pixel (x, y) of the image rect, from the 1280x720 mapped picture, with FsrEasuCon's words con[16].
// tie (optional): set when the pixel's kernel shape rests on a luma tie between different colours (both luma steps of one
// axis of a bilinear corner below 1/4096 while the three texels are not identical): there FsrEasuSetF's length is a ratio of
// rounding residues (luma steps of different colours are multiples of 0.5 / 255 or rounding-level), so a GPU whose UNORM8
// conversion differs from k / 255 by an ulp may shape the kernel differently (host test: up to ~28 levels at a few dozen
// pixels per frame). EASU still clamps every result to the 2x2 around it: lo / hi receive those bounds.
// (lane fsr-stage-b: W x H = the mapped picture's size, 1280x720 by default)
inline Rgb easuPixel(const std::uint8_t* mapped,const std::uint32_t con[16],int x,int y,bool* tie=nullptr,float* lo=nullptr,
                     float* hi=nullptr,int W=int(SourceWidth),int H=int(SourceHeight)) noexcept {
#pragma clang fp contract(off)
    float ppX=float(unsigned(x))*asFloat(con[0])+asFloat(con[2]);
    float ppY=float(unsigned(y))*asFloat(con[1])+asFloat(con[3]);
    const float fpX=std::floor(ppX),fpY=std::floor(ppY);
    ppX-=fpX;ppY-=fpY;
    const int fx=int(fpX),fy=int(fpY);
    // The taps (column, row offsets from f) in AMD's diagram: b c / e f g h / i j k l / n o.
    enum {B,C,E,F,G,Hh,I,J,K,L,N,O,Taps};
    static constexpr int off[Taps][2]{{0,-1},{1,-1},{-1,0},{0,0},{1,0},{2,0},{-1,1},{0,1},{1,1},{2,1},{0,2},{1,2}};
    float t[Taps][3],l[Taps];
    for(int k=0;k<Taps;++k) {
        for(int c=0;c<3;++c)t[k][c]=texel(mapped,W,H,fx+off[k][0],fy+off[k][1],c);
#ifdef RECOMP_FSR1_MUTANT_LUMA
        l[k]=t[k][0]*0.5f+(t[k][2]*0.5f+t[k][1]);
#else
        l[k]=t[k][2]*0.5f+(t[k][0]*0.5f+t[k][1]); // luma times 2: B * 0.5 + (R * 0.5 + G)
#endif
    }
    if(tie) {
        const auto same=[&](int a,int b){return t[a][0]==t[b][0] && t[a][1]==t[b][1] && t[a][2]==t[b][2];};
        const auto axis=[&](int a,int c,int b){constexpr float T=1.0f/4096.0f; // centre c between a and b
            return std::fabs(l[a]-l[c])<T && std::fabs(l[c]-l[b])<T && !(same(a,c) && same(c,b));};
        *tie=axis(E,F,G) || axis(B,F,J) || axis(F,G,Hh) || axis(C,G,K) || axis(I,J,K) || axis(F,J,N) || axis(J,K,L) || axis(G,K,O);
    }
    float dirX=0.0f,dirY=0.0f,len=0.0f;
    easuSet(dirX,dirY,len,(1.0f-ppX)*(1.0f-ppY),l[B],l[E],l[F],l[G],l[J]);
    easuSet(dirX,dirY,len,ppX*(1.0f-ppY),l[C],l[F],l[G],l[Hh],l[K]);
    easuSet(dirX,dirY,len,(1.0f-ppX)*ppY,l[F],l[I],l[J],l[K],l[N]);
    easuSet(dirX,dirY,len,ppX*ppY,l[G],l[J],l[K],l[L],l[O]);
    const float dir2X=dirX*dirX,dir2Y=dirY*dirY;
    float dirR=dir2X+dir2Y;
    const bool zro=dirR<float(1.0/32768.0);
    dirR=prxLoRsq(dirR);
    dirR=zro?1.0f:dirR;
    dirX=zro?1.0f:dirX;
    dirX*=dirR;dirY*=dirR;
    len=len*0.5f;
    len*=len;
    const float stretch=(dirX*dirX+dirY*dirY)*prxLoRcp(fmaxN(std::fabs(dirX),std::fabs(dirY)));
    const float len2X=1.0f+(stretch-1.0f)*len,len2Y=1.0f+(-0.5f)*len;
    const float lob=0.5f+float((1.0/4.0-0.04)-0.5)*len;
    const float clp=prxLoRcp(lob);
    float mn[3],mx[3];
    for(int c=0;c<3;++c) { // min(AMin3F3(f, g, j), k), max(AMax3F3(f, g, j), k)
        mn[c]=fminN(fminN(t[F][c],fminN(t[G][c],t[J][c])),t[K][c]);
        mx[c]=fmaxN(fmaxN(t[F][c],fmaxN(t[G][c],t[J][c])),t[K][c]);
        if(lo)lo[c]=mn[c];
        if(hi)hi[c]=mx[c];
    }
    float aC[3]{0.0f,0.0f,0.0f},aW=0.0f;
    // The shader's tap order: b c i j f e k l h g o n.
#ifdef RECOMP_FSR1_MUTANT_TAP_ORDER
    static constexpr int order[Taps]{B,C,E,F,G,Hh,I,J,K,L,N,O};
#else
    static constexpr int order[Taps]{B,C,I,J,F,E,K,L,Hh,G,O,N};
#endif
    for(int k:order)easuTap(aC,aW,float(off[k][0])-ppX,float(off[k][1])-ppY,dirX,dirY,len2X,len2Y,lob,clp,t[k]);
    const float rw=1.0f/aW;
    Rgb p;
#ifdef RECOMP_FSR1_MUTANT_NO_DERING
    p.r=aC[0]*rw;p.g=aC[1]*rw;p.b=aC[2]*rw;return p;
#endif
    p.r=fminN(mx[0],fmaxN(mn[0],aC[0]*rw));
    p.g=fminN(mx[1],fmaxN(mn[1],aC[1]*rw));
    p.b=fminN(mx[2],fmaxN(mn[2],aC[2]*rw));
    return p;
}
// FsrRcasF at pixel (x, y) of EASU's picture (width x height RGBA8), loads clamped to the edge, con = FsrRcasCon's words.
inline Rgb rcasPixel(const std::uint8_t* picture,int width,int height,const std::uint32_t con[4],int x,int y) noexcept {
#pragma clang fp contract(off)
    float b[3],d[3],e[3],f[3],h[3];
    for(int c=0;c<3;++c) {
        b[c]=texel(picture,width,height,x,y-1,c);d[c]=texel(picture,width,height,x-1,y,c);e[c]=texel(picture,width,height,x,y,c);
        f[c]=texel(picture,width,height,x+1,y,c);h[c]=texel(picture,width,height,x,y+1,c);
    }
    // (luma and the noise term only feed FSR_RCAS_DENOISE, which the shader does not define: no effect on the result)
    float lobeC[3];
    for(int c=0;c<3;++c) {
        const float mn4=fminN(fminN(b[c],fminN(d[c],f[c])),h[c]);
        const float mx4=fmaxN(fmaxN(b[c],fmaxN(d[c],f[c])),h[c]);
        const float hitMin=fminN(mn4,e[c])*(1.0f/(4.0f*mx4));
        const float hitMax=(1.0f-fmaxN(mx4,e[c]))*(1.0f/(4.0f*mn4+(-1.0f*4.0f)));
        lobeC[c]=fmaxN(-hitMin,hitMax);
    }
    const float lobe=fmaxN(float(-(0.25-(1.0/16.0))),fminN(fmaxN(lobeC[0],fmaxN(lobeC[1],lobeC[2])),0.0f))*asFloat(con[0]);
    const float rcpL=prxMedRcp(4.0f*lobe+1.0f);
    Rgb p;
#ifdef RECOMP_FSR1_MUTANT_RCAS_SUM
    p.r=(e[0]+lobe*b[0]+lobe*d[0]+lobe*h[0]+lobe*f[0])*rcpL;p.g=(e[1]+lobe*b[1]+lobe*d[1]+lobe*h[1]+lobe*f[1])*rcpL;
    p.b=(e[2]+lobe*b[2]+lobe*d[2]+lobe*h[2]+lobe*f[2])*rcpL;return p;
#endif
    p.r=(lobe*b[0]+lobe*d[0]+lobe*h[0]+lobe*f[0]+e[0])*rcpL;
    p.g=(lobe*b[1]+lobe*d[1]+lobe*h[1]+lobe*f[1]+e[1])*rcpL;
    p.b=(lobe*b[2]+lobe*d[2]+lobe*h[2]+lobe*f[2]+e[2])*rcpL;
    return p;
}
// A shader result in [0, 1] as the UNORM8 attachment stores it (round to nearest; NaN -> 0).
inline std::uint8_t unormByte(float v) noexcept {
    if(!(v>0.0f))return 0;
    if(v>=1.0f)return 255;
    return std::uint8_t(int(std::floor(v*255.0f+0.5f)));
}
// The map pass's picture: the source through the tables (1280x720 RGBA8; alpha unused).
// (lane fsr-stage-b: `pixels` = the source's width * height, 1280 * 720 by default)
inline void mappedPicture(const std::uint8_t* source,const Lut& lut,std::vector<std::uint8_t>& out,
                          std::size_t pixels=std::size_t{SourceWidth}*SourceHeight) {
    out.assign(pixels*4,255);
    for(std::size_t p=0;p<pixels;++p)
        for(unsigned c=0;c<3;++c)out[p*4+c]=lut[c*256+source[p*4+c]];
}
// Expected bytes; lo / hi: the accepted range of each byte beyond the +/-1 rule (= value except at EASU tie pixels: the 2x2
// clamp bounds), ties: the tie pixels.
struct Expected {std::vector<std::uint8_t> value,lo,hi;std::size_t ties=0;};
// EASU's picture of the image rect (constants c: its width x height), RGBA8, alpha 255 (the shader writes 1).
inline void easuPicture(const std::uint8_t* mapped,const Constants& c,Expected& out) {
    out.value.assign(std::size_t{c.width}*c.height*4,255);out.lo=out.value;out.hi=out.value;out.ties=0;
    for(std::uint32_t y=0;y<c.height;++y)for(std::uint32_t x=0;x<c.width;++x) {
        bool tie=false;float lo[3],hi[3];
        const Rgb p=easuPixel(mapped,c.easu,int(x),int(y),&tie,lo,hi,int(c.srcWidth),int(c.srcHeight));const auto d=(std::size_t(y)*c.width+x)*4;
        const std::uint8_t v[3]{unormByte(p.r),unormByte(p.g),unormByte(p.b)};
        out.ties+=tie;
        for(int k=0;k<3;++k){out.value[d+unsigned(k)]=v[k];out.lo[d+unsigned(k)]=tie?unormByte(lo[k]):v[k];out.hi[d+unsigned(k)]=tie?unormByte(hi[k]):v[k];}
    }
}
inline void easuPicture(const std::uint8_t* mapped,const Constants& c,std::vector<std::uint8_t>& out) {Expected e;easuPicture(mapped,c,e);out.swap(e.value);}
// The window (width x height, black border, alpha 255; BGRA byte order when bgra) with the rect filled from `pixel(x, y)`.
template<class F> inline bool windowFrom(std::uint32_t width,std::uint32_t height,bool bgra,std::vector<std::uint8_t>& out,F pixel) {
    const auto l=layout(width,height);
    if(!l)return false;
    out.assign(std::size_t{width}*height*4,0);
    for(std::size_t i=3;i<out.size();i+=4)out[i]=255;
    for(std::uint32_t y=0;y<l->imageHeight;++y)for(std::uint32_t x=0;x<l->imageWidth;++x) {
        const Rgb p=pixel(int(x),int(y));const auto d=(std::size_t(y+l->top)*width+x+l->left)*4;
        const std::uint8_t r=unormByte(p.r),g=unormByte(p.g),b=unormByte(p.b);
        out[d]=bgra?b:r;out[d+1]=g;out[d+2]=bgra?r:b;
    }
    return true;
}
// Stage B / the EASU-only window: RCAS of `picture` (the image rect's EASU picture) or `picture` itself (rcas false).
inline bool windowFromPicture(const std::uint8_t* picture,const Constants& c,bool rcas,std::uint32_t width,std::uint32_t height,bool bgra,
                              std::vector<std::uint8_t>& out) {
    const auto l=layout(width,height);
    if(!l || !picture || l->imageWidth!=c.width || l->imageHeight!=c.height)return false;
    return windowFrom(width,height,bgra,out,[&](int x,int y) {
        if(rcas)return rcasPixel(picture,int(c.width),int(c.height),c.rcas,x,y);
        const auto s=(std::size_t(unsigned(y))*c.width+unsigned(x))*4;
        return Rgb{float(picture[s])/255.0f,float(picture[s+1])/255.0f,float(picture[s+2])/255.0f};
    });
}
// The EASU-only window from an EASU picture with its tie ranges (the rect placed, border black, alpha 255, BGRA order).
inline bool windowFromEasu(const Expected& picture,const Constants& c,std::uint32_t width,std::uint32_t height,bool bgra,Expected& out) {
    const auto l=layout(width,height);
    if(!l || l->imageWidth!=c.width || l->imageHeight!=c.height || picture.value.size()!=std::size_t{c.width}*c.height*4)return false;
    out.value.assign(std::size_t{width}*height*4,0);
    for(std::size_t i=3;i<out.value.size();i+=4)out.value[i]=255;
    out.lo=out.value;out.hi=out.value;out.ties=picture.ties;
    for(std::uint32_t y=0;y<l->imageHeight;++y)for(std::uint32_t x=0;x<l->imageWidth;++x) {
        const auto s=(std::size_t(y)*c.width+x)*4,d=(std::size_t(y+l->top)*width+x+l->left)*4;
        for(const auto* a:{&picture.value,&picture.lo,&picture.hi}) {
            auto& o=a==&picture.value?out.value:a==&picture.lo?out.lo:out.hi;
            o[d]=(*a)[bgra?s+2:s];o[d+1]=(*a)[s+1];o[d+2]=(*a)[bgra?s:s+2];
        }
    }
    return true;
}
// The audit's comparison (as direct_present.h's filtered modes): a colour channel may differ by 1, alpha (and so the border's
// alpha) must match; any other difference is a mismatch. With tie ranges (Expected): a byte of a tie pixel may also lie anywhere
// in [lo - 1, hi + 1] (counted in tieRange).
struct Diff {std::size_t bytes=0,mismatches=0,offByOne=0,tieRange=0,first=0;unsigned worst=0,got=0,expected=0;};
inline Diff compare(const std::uint8_t* got,const std::uint8_t* expected,const std::uint8_t* lo,const std::uint8_t* hi,std::size_t bytes) noexcept {
    Diff r;r.bytes=bytes;
    for(std::size_t b=0;b<bytes;++b) {
        const unsigned d=unsigned(std::abs(int(got[b])-int(expected[b])));
        if(!d)continue;
        r.worst=std::max(r.worst,d);
        if(d==1 && b%4!=3)++r.offByOne;
        else if(lo && b%4!=3 && (lo[b]!=expected[b] || hi[b]!=expected[b]) && int(got[b])>=int(lo[b])-1 && int(got[b])<=int(hi[b])+1)++r.tieRange;
        else if(!r.mismatches++){r.first=b;r.got=got[b];r.expected=expected[b];}
    }
    return r;
}
inline Diff compare(const std::uint8_t* got,const std::vector<std::uint8_t>& expected) noexcept {return compare(got,expected.data(),nullptr,nullptr,expected.size());}
inline Diff compare(const std::uint8_t* got,const Expected& e) noexcept {return compare(got,e.value.data(),e.lo.data(),e.hi.data(),e.value.size());}
}
