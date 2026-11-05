// PostFX pass81 (T2 80x45 EXPAND -> T3 80x45 RT3 base780, five horizontal
// LINEAR taps) consumes THIS GPU's T2 resolve of the second pass71 words, never
// the captured source. The pack keeps the live destination padding (previous =
// the captured slot5 bytes), so variant0 must reproduce the live T2 texture.
{
    auto blurFixture=loadPostfxBlurFixture(p+"/postfx-blur",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& bd=blurFixture.draw;auto& bb=blurFixture.bound;auto& bprog=blurFixture.program;
    bprog.vertexCode=code(p+"/postfx-blur/original.vert.spv");bprog.pixelCode=code(p+"/postfx-blur/original.frag.spv");
    // Shared post VS (identity and translation) with pass71; a different pixel program.
    assert(validateNativePostfxBlurProgram(bprog) && !validateNativeExpanded16CopyProgram(bprog) &&
        bprog.vertexIdentity==dprog.vertexIdentity && bprog.vertexCode==dprog.vertexCode &&
        bprog.pixelIdentity!=dprog.pixelIdentity);
    const auto liveT2=bd.textures[0].source;
    const auto t2Description=XenosTextureDescription::decode(bd.textures[0].fetch);
    // The sampled fetch names the resolve destination's physical base (header A654605D -> 06546000).
    const auto t2Physical=rplan->destination.baseAddress<0x20000000u?rplan->destination.baseAddress:rplan->destination.baseAddress-0xA0000000u;
    assert(t2Description && t2Description->width==80 && t2Description->height==45 && t2Description->pitchTexels==96 &&
        t2Description->baseAddress==0x06546000 && t2Description->baseAddress==t2Physical &&
        t2Description->pitchTexels==rplan->destination.pitchTexels && liveT2.size()==rplan->storage.sourceBytes);
    const char* bwhy=nullptr;
    const auto producedT2=packNativeRgb7e3FloatResolve(t2,downer,liveT2,&bwhy);
    if(!producedT2)std::printf("POSTFX_BLUR_T2_PACK_REFUSED %s\n",bwhy);assert(producedT2);
    unsigned t2Differing=0;for(std::size_t i=0;i<liveT2.size();++i)t2Differing+=(*producedT2)[i]!=liveT2[i];
    std::printf("POSTFX_BLUR_SOURCE variant=%u differing_bytes=%u\n",variant,t2Differing);
    if(!variant)assert(!t2Differing);
    // Independent Python untiling of the live T2 texture equals the C++ linearizer,
    // and the sampled view of our pack equals the independent CPU packing above.
    const auto t2SourceLinear=read(p+"/postfx-blur/source-linear-rgba16le.bin",80*45*8);
    assert(linearizeXenosExpanded16FloatStorage(*t2Description,liveT2)==t2SourceLinear);
    const auto t2Now=linearizeXenosExpanded16FloatStorage(*t2Description,*producedT2);
    assert(t2Now && *t2Now==expectedT2Linear);
    bd.textures[0].source=*producedT2;
    const auto bplan=planNativePostfxBlurDraw(bd,bb,bprog,bprog.vertexIdentity,bprog.pixelIdentity,false,true,&bwhy);
    if(!bplan)std::printf("POSTFX_BLUR_PLAN_REFUSED %s\n",bwhy);assert(bplan);
    assert(bplan->target.width==80 && bplan->target.height==45 && bplan->target.pitch==80 && bplan->target.baseTile==780 &&
        bplan->target.tileCount==3 && bplan->target.object==bb.surfaces[0].object && bplan->submitted.width==80 &&
        bplan->submitted.height==45 && bplan->submitted.pitchTexels==96);
    // T3 aliases T2's EDRAM rectangle: same header words, a different surface object.
    assert(bplan->target.baseTile==dplan->target.baseTile && bplan->target.object!=dplan->target.object &&
        *bb.surfaces[0].header==*db.surfaces[0].header);
    const auto blurred=renderNativePostfxBlur(device,bprog,bd,*bplan,&bwhy);
    if(!blurred)std::printf("POSTFX_BLUR_RENDER_REFUSED %s\n",bwhy);assert(blurred && blurred->size()==80*45*4);
    auto bUvProgram=bprog;bUvProgram.pixelCode=code(p+"/postfx-blur/coordinates.frag.spv");
    const auto bUv=renderNativePostfxBlur(device,bUvProgram,bd,*bplan,&bwhy);assert(bUv && bUv->size()==80*45*4);
    const auto bHalf=[](unsigned h){_Float16 v;std::memcpy(&v,&h,2);return double(float(v));};
    const auto bTexel=[&](int x,int y,unsigned c){x=std::clamp(x,0,79);y=std::clamp(y,0,44);const auto at=(y*80+x)*8+c*2;
        return bHalf((*t2Now)[at]|(unsigned{(*t2Now)[at+1]}<<8));};
    // c189/c254/c255 straight from the fixture's pixel-constants.bin (BE word (reg*4+comp)*4).
    const auto bConstants=read(p+"/postfx-blur/pixel-constants.bin",4096);
    const auto bConstant=[&](unsigned r,unsigned i){
        const auto at=(r*4+i)*4;
        const unsigned bits=(unsigned{bConstants[at]}<<24)|(unsigned{bConstants[at+1]}<<16)|(unsigned{bConstants[at+2]}<<8)|bConstants[at+3];
        assert(bits==bd.pixelConstants[r*4+i]);float value;std::memcpy(&value,&bits,4);assert(std::isfinite(value));return value;
    };
    const float texelStep=bConstant(189,0);
    // Fetch order s0..s4 (translated n259/n267/uv/n269/n265) and their weights.
    const std::array<float,5> tapScale{bConstant(254,2),bConstant(255,0),0.0f,bConstant(255,2),bConstant(255,1)};
    const std::array<float,5> tapWeight{bConstant(255,3),bConstant(254,0),bConstant(254,1),bConstant(254,0),bConstant(255,3)};
    // Xenos MUL: a zero factor gives 0, otherwise the fp32 product (exact in double, rounded once).
    const auto xenosMul=[](float a,float b){return (a==0 || b==0)?0.0f:float(double(a)*double(b));};
    unsigned bOutside=0,bNonzero=0,bClampedTaps=0;double bMaxRatio=0;
    for(unsigned y=0;y<45;++y)for(unsigned x=0;x<80;++x) {
        const auto at=(y*80+x)*4;const float u=(*bUv)[at],v=(*bUv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*bUv)[at+2]==0 && (*bUv)[at+3]==0);
        for(unsigned c=0;c<4;++c) {
            std::array<double,5> ideal{},tapBound{};
            for(unsigned k=0;k<5;++k) {
                // The module's fp32 tap coordinate: uv.x + c189.x*k (s2 samples uv itself);
                // a float+float sum rounded once from double is the IEEE fp32 sum.
                const float uk=k==2?u:float(double(xenosMul(texelStep,tapScale[k]))+double(u));
                const double tx=double(uk)*80-.5,ty=double(v)*45-.5;const int ix=std::floor(tx),iy=std::floor(ty);
                bClampedTaps+=!c && (ix<0 || ix+1>79);
                const double wx=tx-ix,wy=ty-iy;
                const double t00=bTexel(ix,iy,c),t10=bTexel(ix+1,iy,c),t01=bTexel(ix,iy+1,c),t11=bTexel(ix+1,iy+1,c);
                ideal[k]=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
                const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal[k]),1e-30))))-10));
                tapBound[k]=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            }
            // Same weighted sum in the same order: (((s4*w4+s3*w3)+s2*w2)+s1*w1)+s0*w0.
            double reference=ideal[4]*tapWeight[4];
            reference+=ideal[3]*tapWeight[3];reference+=ideal[2]*tapWeight[2];
            reference+=ideal[1]*tapWeight[1];reference+=ideal[0]*tapWeight[0];
            double tapTerm=0,magnitude=0;
            for(unsigned k=0;k<5;++k) {
                tapTerm+=std::abs(double(tapWeight[k]))*tapBound[k];
                magnitude+=std::abs(double(tapWeight[k]))*(std::abs(ideal[k])+tapBound[k]);
            }
            // Five fp32 products and four fp32 sums, each rounded to nearest (<= 2^-24
            // of the running magnitude), plus a flushed-denormal allowance per operation.
            const double accumulation=9*std::ldexp(magnitude,-24)+9*std::ldexp(1.0,-126);
            const double bound=tapTerm+accumulation;
            const double deviation=std::abs(double((*blurred)[at+c])-reference);
            bMaxRatio=std::max(bMaxRatio,deviation/bound);bOutside+=deviation>bound;
        }
        bNonzero+=(*blurred)[at]!=0 || (*blurred)[at+1]!=0 || (*blurred)[at+2]!=0;
    }
    std::printf("POSTFX_BLUR_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g clamped_taps=%u "
        "model=5tap_ideal_bilinear+1ulp16+weight_step+fp32_accumulation\n",variant,bOutside,bMaxRatio,bClampedTaps);
    assert(!bOutside && (variant || bNonzero) && bClampedTaps);
    NativeEdram32Bands bowner;const auto bserial=bowner.nextSerial();
    {NativeEdram32Mutation tx(bowner);
     assert(executeNativePostfxBlurDraw(device,bprog,bd,bb,bprog.vertexIdentity,bprog.pixelIdentity,true,tx,&bwhy));}
    assert(bowner.nextSerial()==bserial+1 && std::string(bwhy)=="fenced-postfx-blur-physical7e3");
    const auto bwords=bowner.readKnownColorRows(780,80,80,45);assert(bwords && bwords->size()==80*45);
    for(unsigned i=0;i<80*45;++i) {
        std::array<std::uint32_t,4> bits{};for(unsigned c=0;c<4;++c)bits[c]=colorBits((*blurred)[i*4+c]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound) && (*bwords)[i]==packed);
    }
    assert(!bowner.readKnownColorRows(780,80,80,46) && !bowner.readColorWords(783,80,80,16)); // 3-tile footprint only.
    NativeEdram32Bands bpending;
    {NativeEdram32Mutation tx(bpending);
     assert(!executeNativePostfxBlurDraw(device,bprog,bd,bb,bprog.vertexIdentity,bprog.pixelIdentity,false,tx));}
    assert(!bpending.knownTiles());
    // The pass71 route cannot take the blur draw (same VS, different pixel identity).
    NativeEdram32Bands bcross;
    {NativeEdram32Mutation tx(bcross);
     assert(!executeNativeExpanded16CopyDraw(device,dprog,bd,bb,bprog.vertexIdentity,bprog.pixelIdentity,true,tx));}
    assert(!bcross.knownTiles());
    std::printf("POSTFX_BLUR_CONNECTION_PASS variant=%u t2_resolve_producer=1 live_source_reproduced=%u actual_shader=1 "
        "independent_source_linear=1 measured_uv=1 runtime_constants=1 taps=5 sample_bound=1 nonzero=%u physical7e3=1 "
        "base=780 tiles=3 pending_query_refused=1 abi41_route_refused=1 gameplay=0\n",variant,unsigned(!variant),bNonzero);
#ifdef RECOMP_PRIVATE_POSTFX_VERTICAL_BLUR
#include "postfx_vertical_blur_connection.inc"
#endif
}
