// ABI41 consumes THIS GPU bright pass through the existing 7e3->binary16
// resolve conversion, never the captured source. Variant0 must reproduce the
// live resolved source bytes exactly (padding included).
{
    auto copyFixture=loadExpanded16CopyFixture(p+"/bloom-downsample",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& cd=copyFixture.draw;auto& cb=copyFixture.bound;auto& cprog=copyFixture.program;
    cprog.vertexCode=code(p+"/bloom-downsample/original.vert.spv");cprog.pixelCode=code(p+"/bloom-downsample/original.frag.spv");
    assert(validateNativeExpanded16CopyProgram(cprog));
    const auto liveSource=cd.textures[0].source;
    const auto sourceDescription=XenosTextureDescription::decode(cd.textures[0].fetch);
    const auto sourceStorage=sourceDescription?planXenosExpanded16FloatStorage(*sourceDescription):std::nullopt;
    assert(sourceStorage && sourceDescription->width==320 && sourceDescription->height==180);
    const auto brightWords=bowner.readKnownColorRows(720,320,320,180);assert(brightWords && brightWords->size()==320*180);
    std::vector<std::uint8_t> converted(320*180*8);
    for(std::size_t i=0;i<brightWords->size();++i) {
        const auto pixel=color7e3ToBinary16((*brightWords)[i]);
        for(unsigned c=0;c<4;++c){converted[i*8+c*2]=pixel[c]&255;converted[i*8+c*2+1]=pixel[c]>>8;}
    }
    const auto stored=storeXenosTexture2D(sourceStorage->description,converted,liveSource);assert(stored);
    unsigned differingBytes=0;for(std::size_t i=0;i<stored->size();++i)differingBytes+=(*stored)[i]!=liveSource[i];
    std::printf("EXPANDED16_COPY_SOURCE variant=%u differing_bytes=%u\n",variant,differingBytes);
    if(!variant)assert(!differingBytes);
    // Independent Python untiling of the live source equals the C++ linearizer.
    const auto sourceLinear=read(p+"/bloom-downsample/source-linear-rgba16le.bin",320*180*8);
    assert(linearizeXenosExpanded16FloatStorage(*sourceDescription,liveSource)==sourceLinear);
    cd.textures[0].source=*stored;
    const char* cwhy=nullptr;
    const auto cplan=planNativeExpanded16CopyDraw(cd,cb,cprog,cprog.vertexIdentity,cprog.pixelIdentity,false,true,&cwhy);
    if(!cplan)std::printf("EXPANDED16_COPY_PLAN_REFUSED %s\n",cwhy);assert(cplan);
    const auto copied=renderNativeExpanded16Copy(device,cprog,cd,*cplan,&cwhy);
    if(!copied)std::printf("EXPANDED16_COPY_RENDER_REFUSED %s\n",cwhy);assert(copied && copied->size()==160*90*4);
    auto uvProgram=cprog;uvProgram.pixelCode=code(p+"/bloom-downsample/coordinates.frag.spv");
    const auto uv=renderNativeExpanded16Copy(device,uvProgram,cd,*cplan,&cwhy);assert(uv && uv->size()==160*90*4);
    const auto sourceNow=linearizeXenosExpanded16FloatStorage(*sourceDescription,*stored);assert(sourceNow);
    const auto halfBits=[](unsigned h){_Float16 v;std::memcpy(&v,&h,2);return double(float(v));};
    const auto texel=[&](int x,int y,unsigned c){x=std::clamp(x,0,319);y=std::clamp(y,0,179);const auto at=(y*320+x)*8+c*2;
        return halfBits((*sourceNow)[at]|(unsigned{(*sourceNow)[at+1]}<<8));};
    unsigned outside=0,nonzero=0;double maxRatio=0;
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x) {
        const auto at=(y*160+x)*4;const float u=(*uv)[at],v=(*uv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*uv)[at+2]==0 && (*uv)[at+3]==0);
        const double tx=double(u)*320-.5,ty=double(v)*180-.5;const int ix=std::floor(tx),iy=std::floor(ty);
        const double wx=tx-ix,wy=ty-iy;
        for(unsigned c=0;c<4;++c) {
            const double t00=texel(ix,iy,c),t10=texel(ix+1,iy,c),t01=texel(ix,iy+1,c),t11=texel(ix+1,iy+1,c);
            const double ideal=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
            const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal),1e-30))))-10));
            const double bound=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            const double deviation=std::abs(double((*copied)[at+c])-ideal);
            maxRatio=std::max(maxRatio,deviation/bound);outside+=deviation>bound;
        }
        nonzero+=(*copied)[at]!=0 || (*copied)[at+1]!=0 || (*copied)[at+2]!=0;
    }
    std::printf("EXPANDED16_COPY_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g model=ideal_bilinear+1ulp16+weight_step\n",variant,outside,maxRatio);
    assert(!outside && (variant || nonzero));
    NativeEdram32Bands cowner;const auto cserial=cowner.nextSerial();
    {NativeEdram32Mutation tx(cowner);
     assert(executeNativeExpanded16CopyDraw(device,cprog,cd,cb,cprog.vertexIdentity,cprog.pixelIdentity,true,tx,&cwhy));}
    assert(cowner.nextSerial()==cserial+1 && std::string(cwhy)=="fenced-expanded16-copy-physical7e3");
    const auto cwords=cowner.readKnownColorRows(768,160,160,90);assert(cwords && cwords->size()==160*90);
    for(unsigned i=0;i<160*90;++i) {
        std::array<std::uint32_t,4> bits{};for(unsigned c=0;c<4;++c)bits[c]=colorBits((*copied)[i*4+c]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound) && (*cwords)[i]==packed);
    }
    assert(!cowner.readColorWords(768+12,80,80,16)); // Nothing beyond the 12-tile footprint.
    NativeEdram32Bands cpending;
    {NativeEdram32Mutation tx(cpending);
     assert(!executeNativeExpanded16CopyDraw(device,cprog,cd,cb,cprog.vertexIdentity,cprog.pixelIdentity,false,tx));}
    assert(!cpending.knownTiles());
    std::printf("EXPANDED16_COPY_CONNECTION_PASS variant=%u bright_pass_producer=1 resolve_conversion=1 live_source_reproduced=%u "
        "actual_shader=1 independent_source_linear=1 measured_uv=1 sample_bound=1 nonzero=%u physical7e3=1 tiles=12 "
        "pending_query_refused=1 gameplay=0\n",variant,unsigned(!variant),nonzero);
#ifdef RECOMP_PRIVATE_T1_RESOLVE
#include "t1_resolve_connection.inc"
#endif
}
