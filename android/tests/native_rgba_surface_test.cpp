#include "native_rgba_surface.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    std::vector<std::uint8_t> source(NativeSwapImagePlan::PixelBytes);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        const auto at=(y*1280+x)*4;source[at]=x*13+y*7;source[at+1]=x*3+y*19;
        source[at+2]=x*29+y*5;source[at+3]=255;
    }
    auto layout=nativeRgbaSurfaceLayout(1920,970);
    assert(layout && layout->left==98 && layout->top==0 && layout->imageWidth==1724 && layout->imageHeight==970);
    layout=nativeRgbaSurfaceLayout(720,1280);
    assert(layout && layout->left==0 && layout->top==437 && layout->imageWidth==720 && layout->imageHeight==405);
    std::vector<std::uint8_t> result{1,2,3};
    for(auto dimensions:{std::array<unsigned,2>{0,720},{1280,0},{4097,720},{1280,4097},{~0u,~0u}}){
        assert(!packNativeRgbaSurface(source,dimensions[0],dimensions[1],false,result));
        assert((result==std::vector<std::uint8_t>{1,2,3}));
    }
    assert(!packNativeRgbaSurface({},1280,720,false,result));
    source.back()=254;assert(!packNativeRgbaSurface(source,1280,720,false,result));source.back()=255;
    assert(packNativeRgbaSurface(source,1280,720,false,result) && result==source);
    for(auto dimensions:{std::array<unsigned,2>{1920,970},{720,1280},{640,360},{1,1}})for(bool bgra:{false,true}){
        const auto w=dimensions[0],h=dimensions[1];
        assert(packNativeRgbaSurface(source,w,h,bgra,result));
        // Independent rational fit and pixel mapping; compare every output pixel.
        const bool widthLimited=double(w)/1280.0<=double(h)/720.0;
        const auto activeW=widthLimited?w:std::max(1u,unsigned(h*1280.0/720.0));
        const auto activeH=widthLimited?std::max(1u,unsigned(w*720.0/1280.0)):h;
        const auto left=(w-activeW)/2,top=(h-activeH)/2;
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            const auto at=(y*w+x)*4;assert(result[at+3]==255);
            if(x<left || x>=left+activeW || y<top || y>=top+activeH){
                assert(!(result[at]|result[at+1]|result[at+2]));continue;
            }
            const auto sx=unsigned(double(x-left)*1280.0/activeW),sy=unsigned(double(y-top)*720.0/activeH);
            const auto r=std::uint8_t(sx*13+sy*7),g=std::uint8_t(sx*3+sy*19),b=std::uint8_t(sx*29+sy*5);
            assert(result[at]==(bgra?b:r) && result[at+1]==g && result[at+2]==(bgra?r:b));
        }
    }
    assert(packNativeRgbaSurface(source,1280,720,false,source));
    assert(packNativeRgbaSurface(source,4096,4096,false,result) && result.size()==64*1024*1024);
    std::puts("NATIVE_RGBA_SURFACE: RGBA/BGRA, landscape/portrait/downscale, letterbox, bounds and alias PASS");
}
