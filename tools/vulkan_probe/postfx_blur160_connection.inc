// PostFX pass81 at 160x90 (cnb086dj: T1 texture -> T4 RT3 base768, the ABI42
// program's second row) consumes THIS GPU's T1 re-resolve of the pass83 words.
// The pack keeps the live destination padding (previous = the captured slot5
// bytes), so variant0 must reproduce the live T1 texture byte-exactly.
{
    auto hFixture=loadPostfxBlurFixture(p+"/postfx-blur160",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& hd=hFixture.draw;auto& hb=hFixture.bound;auto& hprog=hFixture.program;
    hprog.vertexCode=code(p+"/postfx-blur160/original.vert.spv");hprog.pixelCode=code(p+"/postfx-blur160/original.frag.spv");
    assert(hprog.vertexIdentity==bprog.vertexIdentity && hprog.pixelIdentity==bprog.pixelIdentity &&
        hprog.vertexCode==bprog.vertexCode && hprog.pixelCode==bprog.pixelCode && validateNativePostfxBlurProgram(hprog));
    const auto liveT1b=hd.textures[0].source;
    const auto t1bDescription=XenosTextureDescription::decode(hd.textures[0].fetch);
    assert(t1bDescription && t1bDescription->width==160 && t1bDescription->height==90 && t1bDescription->pitchTexels==160 &&
        t1bDescription->baseAddress==0x0650A000 && liveT1b.size()==t1bPlan->storage.sourceBytes);
    const char* hwhy=nullptr;
    const auto producedT1b=packNativeRgb7e3FloatResolve(t1b,uowner,liveT1b,&hwhy);
    if(!producedT1b)std::printf("POSTFX_BLUR160_T1_PACK_REFUSED %s\n",hwhy);assert(producedT1b);
    unsigned hDiffering=0;for(std::size_t i=0;i<liveT1b.size();++i)hDiffering+=(*producedT1b)[i]!=liveT1b[i];
    std::printf("POSTFX_BLUR160_SOURCE variant=%u differing_bytes=%u\n",variant,hDiffering);
    if(!variant)assert(!hDiffering);
    const auto hSourceLinear=read(p+"/postfx-blur160/source-linear-rgba16le.bin",160*90*8);
    assert(linearizeXenosExpanded16FloatStorage(*t1bDescription,liveT1b)==hSourceLinear);
    const auto hNow=linearizeXenosExpanded16FloatStorage(*t1bDescription,*producedT1b);
    assert(hNow && *hNow==expectedT1bLinear);
    hd.textures[0].source=*producedT1b;
    const auto hplan=planNativePostfxBlurDraw(hd,hb,hprog,hprog.vertexIdentity,hprog.pixelIdentity,false,true,&hwhy);
    if(!hplan)std::printf("POSTFX_BLUR160_PLAN_REFUSED %s\n",hwhy);assert(hplan);
    assert(hplan->target.width==160 && hplan->target.height==90 && hplan->target.pitch==160 && hplan->target.baseTile==768 &&
        hplan->target.tileCount==12 && hplan->target.object==hb.surfaces[0].object && hplan->submitted.pitchTexels==160);
    const auto hblurred=renderNativePostfxBlur(device,hprog,hd,*hplan,&hwhy);
    if(!hblurred)std::printf("POSTFX_BLUR160_RENDER_REFUSED %s\n",hwhy);assert(hblurred && hblurred->size()==160*90*4);
    auto hUvProgram=hprog;hUvProgram.pixelCode=code(p+"/postfx-blur160/coordinates.frag.spv");
    const auto hUv=renderNativePostfxBlur(device,hUvProgram,hd,*hplan,&hwhy);assert(hUv && hUv->size()==160*90*4);
    const auto hHalf=[](unsigned h){_Float16 value;std::memcpy(&value,&h,2);return double(float(value));};
    const auto hTexel=[&](int x,int y,unsigned c){x=std::clamp(x,0,159);y=std::clamp(y,0,89);const auto at=(y*160+x)*8+c*2;
        return hHalf((*hNow)[at]|(unsigned{(*hNow)[at+1]}<<8));};
    const auto hConstants=read(p+"/postfx-blur160/pixel-constants.bin",4096);
    const auto hConstant=[&](unsigned r,unsigned i){
        const auto at=(r*4+i)*4;
        const unsigned bits=(unsigned{hConstants[at]}<<24)|(unsigned{hConstants[at+1]}<<16)|(unsigned{hConstants[at+2]}<<8)|hConstants[at+3];
        assert(bits==hd.pixelConstants[r*4+i]);float value;std::memcpy(&value,&bits,4);assert(std::isfinite(value));return value;
    };
    const float hStep=hConstant(189,0);
    const std::array<float,5> hScale{hConstant(254,2),hConstant(255,0),0.0f,hConstant(255,2),hConstant(255,1)};
    const std::array<float,5> hWeight{hConstant(255,3),hConstant(254,0),hConstant(254,1),hConstant(254,0),hConstant(255,3)};
    const auto hMul=[](float a,float b){return (a==0 || b==0)?0.0f:float(double(a)*double(b));};
    unsigned hOutside=0,hNonzero=0,hClampedTaps=0;double hMaxRatio=0;
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x) {
        const auto at=(y*160+x)*4;const float u=(*hUv)[at],v=(*hUv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*hUv)[at+2]==0 && (*hUv)[at+3]==0);
        for(unsigned c=0;c<4;++c) {
            std::array<double,5> ideal{},tapBound{};
            for(unsigned k=0;k<5;++k) {
                const float uk=k==2?u:float(double(hMul(hStep,hScale[k]))+double(u));
                const double tx=double(uk)*160-.5,ty=double(v)*90-.5;const int ix=std::floor(tx),iy=std::floor(ty);
                hClampedTaps+=!c && (ix<0 || ix+1>159);
                const double wx=tx-ix,wy=ty-iy;
                const double t00=hTexel(ix,iy,c),t10=hTexel(ix+1,iy,c),t01=hTexel(ix,iy+1,c),t11=hTexel(ix+1,iy+1,c);
                ideal[k]=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
                const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal[k]),1e-30))))-10));
                tapBound[k]=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            }
            double reference=ideal[4]*hWeight[4];
            reference+=ideal[3]*hWeight[3];reference+=ideal[2]*hWeight[2];
            reference+=ideal[1]*hWeight[1];reference+=ideal[0]*hWeight[0];
            double tapTerm=0,magnitude=0;
            for(unsigned k=0;k<5;++k) {
                tapTerm+=std::abs(double(hWeight[k]))*tapBound[k];
                magnitude+=std::abs(double(hWeight[k]))*(std::abs(ideal[k])+tapBound[k]);
            }
            const double bound=tapTerm+9*std::ldexp(magnitude,-24)+9*std::ldexp(1.0,-126);
            const double deviation=std::abs(double((*hblurred)[at+c])-reference);
            hMaxRatio=std::max(hMaxRatio,deviation/bound);hOutside+=deviation>bound;
        }
        hNonzero+=(*hblurred)[at]!=0 || (*hblurred)[at+1]!=0 || (*hblurred)[at+2]!=0;
    }
    std::printf("POSTFX_BLUR160_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g clamped_taps=%u "
        "model=5tap_ideal_bilinear+1ulp16+weight_step+fp32_accumulation\n",variant,hOutside,hMaxRatio,hClampedTaps);
    assert(!hOutside && (variant || hNonzero) && hClampedTaps);
    NativeEdram32Bands howner;const auto hserial=howner.nextSerial();
    {NativeEdram32Mutation tx(howner);
     assert(executeNativePostfxBlurDraw(device,hprog,hd,hb,hprog.vertexIdentity,hprog.pixelIdentity,true,tx,&hwhy));}
    assert(howner.nextSerial()==hserial+1 && std::string(hwhy)=="fenced-postfx-blur-physical7e3");
    const auto hwords=howner.readKnownColorRows(768,160,160,90);assert(hwords && hwords->size()==160*90);
    for(unsigned i=0;i<160*90;++i) {
        std::array<std::uint32_t,4> bits{};for(unsigned c=0;c<4;++c)bits[c]=colorBits((*hblurred)[i*4+c]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound) && (*hwords)[i]==packed);
    }
    assert(!howner.readKnownColorRows(768,160,160,91) && !howner.readColorWords(780,80,80,16)); // 12-tile footprint only.
    NativeEdram32Bands hpending;
    {NativeEdram32Mutation tx(hpending);
     assert(!executeNativePostfxBlurDraw(device,hprog,hd,hb,hprog.vertexIdentity,hprog.pixelIdentity,false,tx));}
    assert(!hpending.knownTiles());
    std::printf("POSTFX_BLUR160_CONNECTION_PASS variant=%u t1b_resolve_producer=1 live_source_reproduced=%u actual_shader=1 "
        "independent_source_linear=1 measured_uv=1 runtime_constants=1 taps=5 sample_bound=1 nonzero=%u physical7e3=1 "
        "base=768 tiles=12 pending_query_refused=1 gameplay=0\n",variant,unsigned(!variant),hNonzero);
#ifdef RECOMP_PRIVATE_POSTFX_UPSAMPLE320
#include "postfx_upsample320_connection.inc"
#endif
}
