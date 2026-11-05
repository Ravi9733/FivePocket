// ABI40 consumes the GPU exposure COPY (destination), never the captured texel.
// Variant0 must reproduce the live game's published texel bytes exactly.
{
    auto bright=loadBrightPassFixture(p+"/bright-pass",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& bd=bright.draw;auto& bb=bright.bound;auto& bp=bright.program;
    bp.vertexCode=code(p+"/bright-pass/original.vert.spv");bp.pixelCode=code(p+"/bright-pass/original.frag.spv");
    assert(validateNativeBrightPassProgram(bp));
    // Allocation addresses differ between runs (component copy capture vs this
    // run's copy at 1F063000). Connect by layout and, for variant0, by bytes.
    const auto texelDescription=XenosTextureDescription::decode(bd.textures[0].fetch);
    const auto texelStorage=texelDescription?planXenosExpanded16FloatStorage(*texelDescription):std::nullopt;
    assert(texelStorage && texelDescription->width==cp->destination.width && texelDescription->height==cp->destination.height &&
           texelDescription->format==cp->destination.format && texelStorage->sourceBytes==destination.size());
    const auto liveTexel=bd.textures[0].source;
    std::printf("BRIGHT_PASS_TEXEL variant=%u copy=%02x%02x%02x%02x%02x%02x%02x%02x live=%02x%02x%02x%02x%02x%02x%02x%02x\n",variant,
        destination[0],destination[1],destination[2],destination[3],destination[4],destination[5],destination[6],destination[7],
        liveTexel[0],liveTexel[1],liveTexel[2],liveTexel[3],liveTexel[4],liveTexel[5],liveTexel[6],liveTexel[7]);
    // Only the eight texel bytes are sampled; padding is the test's 0xA7 fill.
    if(!variant)assert(std::equal(destination.begin(),destination.begin()+8,liveTexel.begin()));
    bd.textures[0].source=bd.textures[2].source=destination;
    const char* bwhy=nullptr;
    const auto bplan=planNativeBrightPassDraw(bd,bb,bp,bp.vertexIdentity,bp.pixelIdentity,false,true,&bwhy);
    if(!bplan)std::printf("BRIGHT_PASS_PLAN_REFUSED %s\n",bwhy);assert(bplan);
    // Independent tiling/endian: the Python fixture's linear scene equals C++.
    const auto sceneLinear=read(p+"/bright-pass/scene-linear-rgba16le.bin",640*360*8);
    assert(linearizeXenosExpanded16FloatStorage(bplan->scene,bd.textures[1].source)==sceneLinear);
    const auto rgba=renderNativeBrightPass(device,bp,bd,*bplan,&bwhy);
    if(!rgba)std::printf("BRIGHT_PASS_RENDER_REFUSED %s\n",bwhy);assert(rgba && rgba->size()==320*180*4);
    // Measured interpolated UV of the ORIGINAL vertex stage and the driver's
    // own slot10 LINEAR result (diagnostic pixel stages only). Part A bounds
    // that result against ideal bilinear filtering; part B requires the
    // original arithmetic, applied to those measured samples, to match.
    auto uvProgram=bp;uvProgram.pixelCode=code(p+"/bright-pass/coordinates.frag.spv");
    const auto uv=renderNativeBrightPass(device,uvProgram,bd,*bplan,&bwhy);
    if(!uv)std::printf("BRIGHT_PASS_UV_REFUSED %s\n",bwhy);assert(uv && uv->size()==320*180*4);
    auto sampleProgram=bp;sampleProgram.pixelCode=code(p+"/bright-pass/sample.frag.spv");
    const auto sampled=renderNativeBrightPass(device,sampleProgram,bd,*bplan,&bwhy);
    if(!sampled)std::printf("BRIGHT_PASS_SAMPLE_REFUSED %s\n",bwhy);assert(sampled && sampled->size()==320*180*4);
    const auto halfBits=[](unsigned h){_Float16 f;std::memcpy(&f,&h,2);return float(f);};
    const auto channel=[&](int x,int y,unsigned c){
        x=std::clamp(x,0,639);y=std::clamp(y,0,359);const auto at=(y*640+x)*8+c*2;
        return double(halfBits(sceneLinear[at]|(unsigned{sceneLinear[at+1]}<<8)));};
    // Guest storage is 8-in-16: the red binary16 is bytes0..1 big-endian.
    const float texelRed=halfBits((unsigned{destination[0]}<<8)|destination[1]);
    const auto k=[&](unsigned c,unsigned lane){return colorFloat(bd.pixelConstants[c*4+lane]);};
    const auto mul=[](float a,float b){volatile float r=a*b;return (a==0 || b==0)?0.f:float(r);};
    const auto add=[](float a,float b){volatile float r=a+b;return float(r);};
    double maxAbsolute=0,maxRelative=0,maxSampleBound=0;unsigned mismatches=0,nonzero=0,sampleOutside=0;
    for(unsigned y=0;y<180;++y)for(unsigned x=0;x<320;++x) {
        const auto at=(y*320+x)*4;const float u=(*uv)[at],v=(*uv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*uv)[at+2]==0 && (*uv)[at+3]==0);
        const double tx=double(u)*640-.5,ty=double(v)*360-.5;const int ix=std::floor(tx),iy=std::floor(ty);
        const double wx=tx-ix,wy=ty-iy;
        for(unsigned c=0;c<3;++c) {
            const double t00=channel(ix,iy,c),t10=channel(ix+1,iy,c),t01=channel(ix,iy+1,c),t11=channel(ix+1,iy+1,c);
            const double ideal=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
            // One binary16 ULP of the result plus one 1/256 weight step on each axis.
            const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal),1e-30))))-10));
            const double bound=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            const double deviation=std::abs(double((*sampled)[at+c])-ideal);
            maxSampleBound=std::max(maxSampleBound,deviation/bound);sampleOutside+=deviation>bound;
        }
        // Original graph in instruction order (n265..n299 of the analyzed PS).
        const float sr=mul((*sampled)[at],k(18,1)),sg=mul((*sampled)[at+1],k(18,1)),sb=mul((*sampled)[at+2],k(18,1));
        const float lr=mul(mul(sr,texelRed),k(255,1)),lg=mul(mul(sg,texelRed),k(255,1)),lb=mul(mul(sb,texelRed),k(255,1));
        const float lum=add(add(mul(lb,k(136,3)),mul(lr,k(136,3))),mul(lg,k(136,3)));
        const float m=lum>=k(255,0)?lum:k(255,0);
        const float t=std::clamp(mul(k(135,2),add(-k(135,0),m)),0.f,1.f);
        volatile float reciprocal=1.f/m;const float f=mul(mul(t,m),float(reciprocal));
        const float expected[4]{mul(mul(sr,f),texelRed),mul(mul(sg,f),texelRed),mul(mul(sb,f),texelRed),0.f};
        for(unsigned c=0;c<4;++c) {
            const float got=(*rgba)[at+c];const double error=std::abs(double(got)-expected[c]);
            maxAbsolute=std::max(maxAbsolute,error);
            if(expected[c])maxRelative=std::max(maxRelative,error/std::abs(double(expected[c])));
            if(error>2e-5*std::max(1e-3,std::abs(double(expected[c])))) {
                if(mismatches<8)std::printf("BRIGHT_PASS_MISMATCH variant=%u x=%u y=%u c=%u got=%.9g want=%.9g\n",variant,x,y,c,got,expected[c]);
                ++mismatches;
            }
        }
    }
    std::printf("BRIGHT_PASS_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g model=ideal_bilinear+1ulp16+weight_step\n",
        variant,sampleOutside,maxSampleBound);
    assert(!sampleOutside);
    std::printf("BRIGHT_PASS_COMPARE variant=%u mismatches=%u max_absolute=%.9g max_relative=%.9g measured_original_uv=1 measured_driver_samples=1 arithmetic_tolerance=2e-5\n",
        variant,mismatches,maxAbsolute,maxRelative);
    assert(!mismatches);
    for(unsigned i=0;i<320*180;++i)nonzero+=(*rgba)[i*4]!=0 || (*rgba)[i*4+1]!=0 || (*rgba)[i*4+2]!=0;
    assert(nonzero);
    if(!variant) {
        // Exact-centre CPU reference (independent untiling): threshold coverage
        // may differ only by pixels whose luminance straddles the knee.
        const auto reference=read(p+"/bright-pass/reference-rgba32le.bin",320*180*16);
        unsigned referenceNonzero=0;
        for(unsigned i=0;i<320*180;++i){float r[3];std::memcpy(r,reference.data()+i*16,12);referenceNonzero+=r[0]!=0 || r[1]!=0 || r[2]!=0;}
        assert(referenceNonzero==7568 && std::abs(int(nonzero)-int(referenceNonzero))<=int(referenceNonzero/100));
    }
    // Physical publication: the exact 7e3 words of this fenced GPU result.
    NativeEdram32Bands bowner;const auto bserial=bowner.nextSerial();
    {NativeEdram32Mutation tx(bowner);
     assert(executeNativeBrightPassDraw(device,bp,bd,bb,bp.vertexIdentity,bp.pixelIdentity,true,tx,&bwhy));}
    assert(bowner.nextSerial()==bserial+1 && std::string(bwhy)=="fenced-bright-pass-physical7e3");
    const auto words=bowner.readKnownColorRows(720,320,320,180);assert(words && words->size()==320*180);
    for(unsigned i=0;i<320*180;++i) {
        std::array<std::uint32_t,4> bits{};for(unsigned c=0;c<4;++c)bits[c]=colorBits((*rgba)[i*4+c]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound) && (*words)[i]==packed);
    }
    assert(!bowner.readColorWords(720+48,80,80,16)); // Nothing beyond the 48-tile footprint.
#ifdef RECOMP_PRIVATE_BLOOM_DOWNSAMPLE
#include "expanded16_copy_connection.inc"
#endif
    NativeEdram32Bands bpending;
    {NativeEdram32Mutation tx(bpending);
     assert(!executeNativeBrightPassDraw(device,bp,bd,bb,bp.vertexIdentity,bp.pixelIdentity,false,tx));}
    assert(!bpending.knownTiles());
    auto stale=bd;stale.textures[2].source[0]^=1;
    NativeEdram32Bands bstale;
    {NativeEdram32Mutation tx(bstale);
     assert(!executeNativeBrightPassDraw(device,bp,stale,bb,bp.vertexIdentity,bp.pixelIdentity,true,tx));}
    assert(!bstale.knownTiles());
    std::printf("BRIGHT_PASS_CONNECTION_PASS variant=%u original_gpu_producer=1 actual_copy_consumed=1 live_texel_reproduced=%u "
        "actual_shader=1 independent_scene_linear=1 measured_uv=1 sample_bound=1 exact_centre_coverage_checked=%u max_absolute=%.9g nonzero=%u physical7e3=1 tiles=48 "
        "pending_query_refused=1 texel_mismatch_refused=1 gameplay=0\n",
        variant,unsigned(!variant),unsigned(!variant),maxAbsolute,nonzero);
}
