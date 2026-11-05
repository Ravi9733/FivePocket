// Second pass71 (T1 160x90 -> T2 80x45 base780) consumes THIS GPU's T1 resolve
// of the ABI41 words, never the captured source. The pack keeps the live
// destination padding, so variant0 must reproduce the live T1 texture exactly.
{
    auto t2Fixture=loadExpanded16CopyFixture(p+"/bloom-downsample-t2",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& dd=t2Fixture.draw;auto& db=t2Fixture.bound;auto& dprog=t2Fixture.program;
    dprog.vertexCode=code(p+"/bloom-downsample-t2/original.vert.spv");dprog.pixelCode=code(p+"/bloom-downsample-t2/original.frag.spv");
    assert(validateNativeExpanded16CopyProgram(dprog) && dprog.vertexIdentity==cprog.vertexIdentity &&
        dprog.pixelIdentity==cprog.pixelIdentity && dprog.vertexCode==cprog.vertexCode && dprog.pixelCode==cprog.pixelCode);
    const auto liveT1=dd.textures[0].source;
    const auto t1Description=XenosTextureDescription::decode(dd.textures[0].fetch);
    assert(t1Description && t1Description->width==160 && t1Description->height==90 && t1Description->baseAddress==0x0650A000 &&
        liveT1.size()==tplan->storage.sourceBytes);
    const auto producedT1=packNativeRgb7e3FloatResolve(t1,cowner,liveT1,&twhy);assert(producedT1);
    unsigned t1Differing=0;for(std::size_t i=0;i<liveT1.size();++i)t1Differing+=(*producedT1)[i]!=liveT1[i];
    std::printf("T2_DOWNSAMPLE_SOURCE variant=%u differing_bytes=%u\n",variant,t1Differing);
    if(!variant)assert(!t1Differing);
    // Independent Python untiling of the live T1 texture equals the C++ linearizer.
    const auto t1Linear=read(p+"/bloom-downsample-t2/source-linear-rgba16le.bin",160*90*8);
    assert(linearizeXenosExpanded16FloatStorage(*t1Description,liveT1)==t1Linear);
    dd.textures[0].source=*producedT1;
    const char* dwhy=nullptr;
    const auto dplan=planNativeExpanded16CopyDraw(dd,db,dprog,dprog.vertexIdentity,dprog.pixelIdentity,false,true,&dwhy);
    if(!dplan)std::printf("T2_DOWNSAMPLE_PLAN_REFUSED %s\n",dwhy);assert(dplan);
    assert(dplan->target.width==80 && dplan->target.height==45 && dplan->target.pitch==80 && dplan->target.baseTile==780 &&
        dplan->target.tileCount==3 && dplan->target.object==db.surfaces[0].object && dplan->submitted.width==160);
    const auto copiedT2=renderNativeExpanded16Copy(device,dprog,dd,*dplan,&dwhy);
    if(!copiedT2)std::printf("T2_DOWNSAMPLE_RENDER_REFUSED %s\n",dwhy);assert(copiedT2 && copiedT2->size()==80*45*4);
    auto dUvProgram=dprog;dUvProgram.pixelCode=code(p+"/bloom-downsample-t2/coordinates.frag.spv");
    const auto dUv=renderNativeExpanded16Copy(device,dUvProgram,dd,*dplan,&dwhy);assert(dUv && dUv->size()==80*45*4);
    const auto t1Now=linearizeXenosExpanded16FloatStorage(*t1Description,*producedT1);assert(t1Now);
    const auto dHalf=[](unsigned h){_Float16 v;std::memcpy(&v,&h,2);return double(float(v));};
    const auto dTexel=[&](int x,int y,unsigned c){x=std::clamp(x,0,159);y=std::clamp(y,0,89);const auto at=(y*160+x)*8+c*2;
        return dHalf((*t1Now)[at]|(unsigned{(*t1Now)[at+1]}<<8));};
    unsigned dOutside=0,dNonzero=0;double dMaxRatio=0;
    for(unsigned y=0;y<45;++y)for(unsigned x=0;x<80;++x) {
        const auto at=(y*80+x)*4;const float u=(*dUv)[at],v=(*dUv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*dUv)[at+2]==0 && (*dUv)[at+3]==0);
        const double tx=double(u)*160-.5,ty=double(v)*90-.5;const int ix=std::floor(tx),iy=std::floor(ty);
        const double wx=tx-ix,wy=ty-iy;
        for(unsigned c=0;c<4;++c) {
            const double t00=dTexel(ix,iy,c),t10=dTexel(ix+1,iy,c),t01=dTexel(ix,iy+1,c),t11=dTexel(ix+1,iy+1,c);
            const double ideal=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
            const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal),1e-30))))-10));
            const double bound=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            const double deviation=std::abs(double((*copiedT2)[at+c])-ideal);
            dMaxRatio=std::max(dMaxRatio,deviation/bound);dOutside+=deviation>bound;
        }
        dNonzero+=(*copiedT2)[at]!=0 || (*copiedT2)[at+1]!=0 || (*copiedT2)[at+2]!=0;
    }
    std::printf("T2_DOWNSAMPLE_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g model=ideal_bilinear+1ulp16+weight_step\n",variant,dOutside,dMaxRatio);
    assert(!dOutside && (variant || dNonzero));
    NativeEdram32Bands downer;const auto dserial=downer.nextSerial();
    {NativeEdram32Mutation tx(downer);
     assert(executeNativeExpanded16CopyDraw(device,dprog,dd,db,dprog.vertexIdentity,dprog.pixelIdentity,true,tx,&dwhy));}
    assert(downer.nextSerial()==dserial+1 && std::string(dwhy)=="fenced-expanded16-copy-physical7e3");
    const auto dwords=downer.readKnownColorRows(780,80,80,45);assert(dwords && dwords->size()==80*45);
    for(unsigned i=0;i<80*45;++i) {
        std::array<std::uint32_t,4> bits{};for(unsigned c=0;c<4;++c)bits[c]=colorBits((*copiedT2)[i*4+c]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound) && (*dwords)[i]==packed);
    }
    assert(!downer.readKnownColorRows(780,80,80,46) && !downer.readColorWords(783,80,80,16)); // 3-tile footprint only.
    NativeEdram32Bands dpending;
    {NativeEdram32Mutation tx(dpending);
     assert(!executeNativeExpanded16CopyDraw(device,dprog,dd,db,dprog.vertexIdentity,dprog.pixelIdentity,false,tx));}
    assert(!dpending.knownTiles());
    std::printf("T2_DOWNSAMPLE_CONNECTION_PASS variant=%u t1_resolve_producer=1 live_source_reproduced=%u actual_shader=1 "
        "independent_source_linear=1 measured_uv=1 sample_bound=1 nonzero=%u physical7e3=1 base=780 tiles=3 "
        "pending_query_refused=1 gameplay=0\n",variant,unsigned(!variant),dNonzero);
#ifdef RECOMP_PRIVATE_T2_RESOLVE
#include "t2_resolve_connection.inc"
#endif
}
