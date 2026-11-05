// Pass83 at 160x90 -> 320x180 (1if67slu: T1 texture -> A RT3 base720, ONE/ONE
// add onto A's bright-pass words). The T1 source is rebuilt in the game's order
// from THIS GPU's 160x90 pass81 words (howner): the T4 clearing resolve
// (AUTHORED: the captured T1 call retargeted to T4's object 10271210 and
// destination base 06528000, live serial91), pass82 at 160x90 (AUTHORED: the
// captured 160x90 pass81 draw with the vertical program, source T4, target T1
// 102711D0) and T1's clearing resolve (the captured T1 call). Variant0 must
// reproduce the live T1 texture byte-exactly, which validates both authored steps.
{
    const auto aPut=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    const char* awhy=nullptr;
    auto t4=t1;t4.arguments[3]=0xA83937A4;
    aPut(*t4.state,12816,0x10271210);t4.surfaces->surfaces[0].object=0x10271210;
    aPut(*t4.destinationHeader,32,0xA652805D);
    const auto t4Plan=planNativeRgb7e3FloatResolve(t4,&awhy);
    if(!t4Plan)std::printf("POSTFX_UPSAMPLE320_T4_PLAN_REFUSED %s\n",awhy);
    // Plans carry the virtual (A-prefixed) destination address, like the captured T1 call's A650A000.
    assert(t4Plan && t4Plan->clearsSource && t4Plan->base==768 && t4Plan->destination.baseAddress==0xA6528000u &&
        t4Plan->sourceObject==0x10271210);
    const std::vector<std::uint8_t> t4Previous(t4Plan->storage.sourceBytes,0xAE);
    const auto t4Texture=packNativeRgb7e3FloatResolve(t4,howner,t4Previous,&awhy);assert(t4Texture);
    auto vd2=hd;auto vb2=hb;
    vd2.textures[0].fetch[1]=(vd2.textures[0].fetch[1]&0xFFFu)|0x06528000u;vd2.textures[0].source=*t4Texture;
    aPut(vd2.deviceState,12816,0x102711D0);vb2.surfaces[0].object=0x102711D0;
    const auto v2plan=planNativePostfxBlurDraw(vd2,vb2,vprog,vprog.vertexIdentity,vprog.pixelIdentity,false,true,&awhy);
    if(!v2plan)std::printf("POSTFX_UPSAMPLE320_V160_PLAN_REFUSED %s\n",awhy);
    assert(v2plan && v2plan->target.object==0x102711D0 && v2plan->target.baseTile==768 && v2plan->submitted.baseAddress==0x06528000);
    NativeEdram32Bands v2owner=howner;assert(recordNativeRgb7e3FloatResolveClear(v2owner,*t4Plan));
    {NativeEdram32Mutation tx(v2owner);
     assert(executeNativePostfxBlurDraw(device,vprog,vd2,vb2,vprog.vertexIdentity,vprog.pixelIdentity,true,tx,&awhy));}
    auto aFixture=loadPostfxUpsampleFixture(p+"/postfx-upsample320",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& ad=aFixture.draw;auto& ab=aFixture.bound;auto& aprog=aFixture.program;
    aprog.vertexCode=code(p+"/postfx-upsample320/original.vert.spv");aprog.pixelCode=code(p+"/postfx-upsample320/original.frag.spv");
    assert(aprog.vertexIdentity==uprog.vertexIdentity && aprog.pixelIdentity==uprog.pixelIdentity &&
        aprog.vertexCode==uprog.vertexCode && aprog.pixelCode==uprog.pixelCode);
    const auto liveT1c=ad.textures[0].source;
    const auto t1cDescription=XenosTextureDescription::decode(ad.textures[0].fetch);
    assert(t1cDescription && t1cDescription->width==160 && t1cDescription->height==90 && t1cDescription->baseAddress==0x0650A000 &&
        liveT1c.size()==tplan->storage.sourceBytes);
    const auto producedT1c=packNativeRgb7e3FloatResolve(t1,v2owner,liveT1c,&awhy);assert(producedT1c);
    unsigned aDiffering=0;for(std::size_t i=0;i<liveT1c.size();++i)aDiffering+=(*producedT1c)[i]!=liveT1c[i];
    std::printf("POSTFX_UPSAMPLE320_SOURCE variant=%u differing_bytes=%u\n",variant,aDiffering);
    if(!variant)assert(!aDiffering);
    const auto aSourceLinear=read(p+"/postfx-upsample320/source-linear-rgba16le.bin",160*90*8);
    assert(linearizeXenosExpanded16FloatStorage(*t1cDescription,liveT1c)==aSourceLinear);
    const auto aNow=linearizeXenosExpanded16FloatStorage(*t1cDescription,*producedT1c);assert(aNow);
    ad.textures[0].source=*producedT1c;
    const auto aplan=planNativePostfxUpsampleDraw(ad,ab,aprog,aprog.vertexIdentity,aprog.pixelIdentity,false,true,&awhy);
    if(!aplan)std::printf("POSTFX_UPSAMPLE320_PLAN_REFUSED %s\n",awhy);
    assert(aplan && aplan->target.width==320 && aplan->target.height==180 && aplan->target.baseTile==720 &&
        aplan->target.tileCount==48 && aplan->target.object==0x10271110);
    const auto aUp=renderNativePostfxUpsample(device,aprog,ad,*aplan,&awhy);
    if(!aUp)std::printf("POSTFX_UPSAMPLE320_RENDER_REFUSED %s\n",awhy);assert(aUp && aUp->size()==320*180*4);
    auto aUvProgram=aprog;aUvProgram.pixelCode=code(p+"/postfx-upsample320/coordinates.frag.spv");
    const auto aUv=renderNativePostfxUpsample(device,aUvProgram,ad,*aplan,&awhy);assert(aUv && aUv->size()==320*180*4);
    const auto aHalf=[](unsigned h){_Float16 value;std::memcpy(&value,&h,2);return double(float(value));};
    const auto aTexel=[&](int x,int y,unsigned c){x=std::clamp(x,0,159);y=std::clamp(y,0,89);const auto at=(y*160+x)*8+c*2;
        return aHalf((*aNow)[at]|(unsigned{(*aNow)[at+1]}<<8));};
    unsigned aOutside=0,aNonzero=0,aClamped=0;double aMaxRatio=0;
    for(unsigned y=0;y<180;++y)for(unsigned x=0;x<320;++x) {
        const auto at=(y*320+x)*4;const float u=(*aUv)[at],v=(*aUv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*aUv)[at+2]==0 && (*aUv)[at+3]==0);
        const double tx=double(u)*160-.5,ty=double(v)*90-.5;const int ix=std::floor(tx),iy=std::floor(ty);
        aClamped+=ix<0 || ix+1>159 || iy<0 || iy+1>89;
        const double wx=tx-ix,wy=ty-iy;
        for(unsigned c=0;c<3;++c) {
            const double t00=aTexel(ix,iy,c),t10=aTexel(ix+1,iy,c),t01=aTexel(ix,iy+1,c),t11=aTexel(ix+1,iy+1,c);
            const double ideal=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
            const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal),1e-30))))-10));
            const double bound=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            const double deviation=std::abs(double((*aUp)[at+c])-ideal);
            aMaxRatio=std::max(aMaxRatio,deviation/bound);aOutside+=deviation>bound;
        }
        assert((*aUp)[at+3]==0 && !std::signbit((*aUp)[at+3]));
        aNonzero+=(*aUp)[at]!=0 || (*aUp)[at+1]!=0 || (*aUp)[at+2]!=0;
    }
    std::printf("POSTFX_UPSAMPLE320_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g clamped_pixels=%u "
        "model=ideal_bilinear+1ulp16+weight_step alpha=0\n",variant,aOutside,aMaxRatio,aClamped);
    assert(!aOutside && (variant || aNonzero) && aClamped);
    // Destination: A's bright-pass words from this chain (ABI40 at base720), rows 0..179.
    std::vector<std::uint8_t> aBright(std::size_t{320}*180*4);
    for(std::size_t i=0;i<brightWords->size();++i)for(unsigned k=0;k<4;++k)aBright[i*4+k]=(*brightWords)[i]>>(8*k);
    NativeEdram32Bands aowner;assert(aowner.recordCompletedRawColorRows(720,320,320,180,aBright,aowner.nextSerial()));
    auto asum=aowner;const auto asumSerial=asum.nextSerial();
    {NativeEdram32Mutation tx(asum);
     assert(executeNativePostfxUpsampleDraw(device,aprog,ad,ab,aprog.vertexIdentity,aprog.pixelIdentity,true,tx,&awhy));}
    assert(asum.nextSerial()==asumSerial+1 && std::string(awhy)=="fenced-postfx-upsample-additive-physical7e3" &&
        asum.knownTiles()==aowner.knownTiles());
    const auto asumWords=asum.readKnownColorRows(720,320,320,180);assert(asumWords);
    unsigned aChanged=0;
    for(unsigned i=0;i<320*180;++i) {
        const auto before=unpackColor7e3((*brightWords)[i]);std::array<std::uint32_t,4> added{};
        for(unsigned c=0;c<4;++c)added[c]=colorBits(colorFloat(before[c])+(*aUp)[i*4+c]);
        unsigned want=0;assert(packColor7e3(added,want,Color7e3Rounding::XeniaShiftRound) && (*asumWords)[i]==want);
        assert(((*asumWords)[i]>>30)==((*brightWords)[i]>>30));
        aChanged+=(*asumWords)[i]!=(*brightWords)[i];
    }
    assert(variant || aChanged);
    NativeEdram32Bands ahole=aowner;assert(ahole.invalidate(767,1));
    {NativeEdram32Mutation tx(ahole);
     assert(!executeNativePostfxUpsampleDraw(device,aprog,ad,ab,aprog.vertexIdentity,aprog.pixelIdentity,true,tx,&awhy));
     assert(std::string(awhy)=="destination-colour-unknown");}
    NativeEdram32Bands apending=aowner;
    {NativeEdram32Mutation tx(apending);
     assert(!executeNativePostfxUpsampleDraw(device,aprog,ad,ab,aprog.vertexIdentity,aprog.pixelIdentity,false,tx,&awhy));}
    std::printf("POSTFX_UPSAMPLE320_CONNECTION_PASS variant=%u pass81_160_producer=1 authored_t4_resolve=1 authored_pass82_160=1 "
        "live_t1_reproduced=%u actual_shader=1 independent_source_linear=1 measured_uv=1 sample_bound=1 alpha_zero=1 "
        "destination=bright_pass_words blend=add changed=%u base=720 tiles=48 unknown_destination_refused=1 "
        "pending_query_refused=1 gameplay=0\n",variant,unsigned(!variant),aChanged);
#ifdef RECOMP_PRIVATE_POSTFX_BLUR320
#include "postfx_blur320_connection.inc"
#endif
}
