// PostFX pass82 (T3 80x45 EXPAND -> T2 80x45 RT3 base780, five VERTICAL
// LINEAR taps) consumes THIS GPU's pass81 words, never the captured source.
// The live T3 resolve itself succeeded (not captured); its call has exactly
// the T2 call's row (same 80x45 base780 flags0x100 profile, same destination
// layout), so the T2 call packs our pass81 words with previous = the captured
// slot5 bytes and variant0 must reproduce the live T3 texture byte-exactly.
{
    auto vFixture=loadPostfxBlurFixture(p+"/postfx-vertical-blur",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& vd=vFixture.draw;auto& vb=vFixture.bound;auto& vprog=vFixture.program;vprog.abiVersion=43;
    vprog.vertexCode=code(p+"/postfx-vertical-blur/original.vert.spv");vprog.pixelCode=code(p+"/postfx-vertical-blur/original.frag.spv");
    assert(validateNativePostfxVerticalBlurProgram(vprog) && !validateNativePostfxBlurProgram(vprog) &&
        vprog.vertexIdentity==bprog.vertexIdentity && vprog.vertexCode==bprog.vertexCode && vprog.pixelIdentity!=bprog.pixelIdentity);
    const auto liveT3=vd.textures[0].source;
    const auto t3Description=XenosTextureDescription::decode(vd.textures[0].fetch);
    assert(t3Description && t3Description->width==80 && t3Description->height==45 && t3Description->pitchTexels==96 &&
        t3Description->baseAddress==0x06552000 && liveT3.size()==rplan->storage.sourceBytes);
    const char* vwhy=nullptr;
    const auto producedT3=packNativeRgb7e3FloatResolve(t2,bowner,liveT3,&vwhy);
    if(!producedT3)std::printf("POSTFX_VERTICAL_BLUR_T3_PACK_REFUSED %s\n",vwhy);assert(producedT3);
    unsigned t3Differing=0;for(std::size_t i=0;i<liveT3.size();++i)t3Differing+=(*producedT3)[i]!=liveT3[i];
    std::printf("POSTFX_VERTICAL_BLUR_SOURCE variant=%u differing_bytes=%u\n",variant,t3Differing);
    if(!variant)assert(!t3Differing);
    // Independent Python untiling of the live T3 texture equals the C++ linearizer,
    // and our packed view equals the binary16 conversion of the pass81 words.
    const auto t3SourceLinear=read(p+"/postfx-vertical-blur/source-linear-rgba16le.bin",80*45*8);
    assert(linearizeXenosExpanded16FloatStorage(*t3Description,liveT3)==t3SourceLinear);
    const auto t3Now=linearizeXenosExpanded16FloatStorage(*t3Description,*producedT3);assert(t3Now);
    for(unsigned i=0;i<80*45;++i) {
        const auto floats=unpackColor7e3((*bwords)[i]);
        for(unsigned c=0;c<4;++c) {
            const _Float16 reference=colorFloat(floats[c]);std::uint16_t h=0;std::memcpy(&h,&reference,2);
            assert((*t3Now)[i*8+c*2]==(h&255) && (*t3Now)[i*8+c*2+1]==(h>>8));
        }
    }
    vd.textures[0].source=*producedT3;
    const auto vplan=planNativePostfxBlurDraw(vd,vb,vprog,vprog.vertexIdentity,vprog.pixelIdentity,false,true,&vwhy);
    if(!vplan)std::printf("POSTFX_VERTICAL_BLUR_PLAN_REFUSED %s\n",vwhy);assert(vplan);
    assert(vplan->target.width==80 && vplan->target.height==45 && vplan->target.pitch==80 && vplan->target.baseTile==780 &&
        vplan->target.tileCount==3 && vplan->target.object==vb.surfaces[0].object && vplan->submitted.pitchTexels==96);
    // Pass82 writes T2 again (the second pass71 target object) over T3's rectangle.
    assert(vplan->target.object==db.surfaces[0].object && vplan->target.object!=bb.surfaces[0].object &&
        *vb.surfaces[0].header==*db.surfaces[0].header);
    const auto vblurred=renderNativePostfxBlur(device,vprog,vd,*vplan,&vwhy);
    if(!vblurred)std::printf("POSTFX_VERTICAL_BLUR_RENDER_REFUSED %s\n",vwhy);assert(vblurred && vblurred->size()==80*45*4);
    auto vUvProgram=vprog;vUvProgram.pixelCode=code(p+"/postfx-vertical-blur/coordinates.frag.spv");
    const auto vUv=renderNativePostfxBlur(device,vUvProgram,vd,*vplan,&vwhy);assert(vUv && vUv->size()==80*45*4);
    const auto vHalf=[](unsigned h){_Float16 value;std::memcpy(&value,&h,2);return double(float(value));};
    const auto vTexel=[&](int x,int y,unsigned c){x=std::clamp(x,0,79);y=std::clamp(y,0,44);const auto at=(y*80+x)*8+c*2;
        return vHalf((*t3Now)[at]|(unsigned{(*t3Now)[at+1]}<<8));};
    const auto vConstants=read(p+"/postfx-vertical-blur/pixel-constants.bin",4096);
    const auto vConstant=[&](unsigned r,unsigned i){
        const auto at=(r*4+i)*4;
        const unsigned bits=(unsigned{vConstants[at]}<<24)|(unsigned{vConstants[at+1]}<<16)|(unsigned{vConstants[at+2]}<<8)|vConstants[at+3];
        assert(bits==vd.pixelConstants[r*4+i]);float value;std::memcpy(&value,&bits,4);assert(std::isfinite(value));return value;
    };
    const float texelStepY=vConstant(189,1);
    const std::array<float,5> vScale{vConstant(254,2),vConstant(255,0),0.0f,vConstant(255,2),vConstant(255,1)};
    const std::array<float,5> vWeight{vConstant(255,3),vConstant(254,0),vConstant(254,1),vConstant(254,0),vConstant(255,3)};
    const auto vMul=[](float a,float b){return (a==0 || b==0)?0.0f:float(double(a)*double(b));};
    unsigned vOutside=0,vNonzero=0,vClampedTaps=0;double vMaxRatio=0;
    for(unsigned y=0;y<45;++y)for(unsigned x=0;x<80;++x) {
        const auto at=(y*80+x)*4;const float u=(*vUv)[at],v=(*vUv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*vUv)[at+2]==0 && (*vUv)[at+3]==0);
        for(unsigned c=0;c<4;++c) {
            std::array<double,5> ideal{},tapBound{};
            for(unsigned k=0;k<5;++k) {
                const float vk=k==2?v:float(double(vMul(texelStepY,vScale[k]))+double(v));
                const double tx=double(u)*80-.5,ty=double(vk)*45-.5;const int ix=std::floor(tx),iy=std::floor(ty);
                vClampedTaps+=!c && (iy<0 || iy+1>44);
                const double wx=tx-ix,wy=ty-iy;
                const double t00=vTexel(ix,iy,c),t10=vTexel(ix+1,iy,c),t01=vTexel(ix,iy+1,c),t11=vTexel(ix+1,iy+1,c);
                ideal[k]=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
                const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal[k]),1e-30))))-10));
                tapBound[k]=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            }
            double reference=ideal[4]*vWeight[4];
            reference+=ideal[3]*vWeight[3];reference+=ideal[2]*vWeight[2];
            reference+=ideal[1]*vWeight[1];reference+=ideal[0]*vWeight[0];
            double tapTerm=0,magnitude=0;
            for(unsigned k=0;k<5;++k) {
                tapTerm+=std::abs(double(vWeight[k]))*tapBound[k];
                magnitude+=std::abs(double(vWeight[k]))*(std::abs(ideal[k])+tapBound[k]);
            }
            const double bound=tapTerm+9*std::ldexp(magnitude,-24)+9*std::ldexp(1.0,-126);
            const double deviation=std::abs(double((*vblurred)[at+c])-reference);
            vMaxRatio=std::max(vMaxRatio,deviation/bound);vOutside+=deviation>bound;
        }
        vNonzero+=(*vblurred)[at]!=0 || (*vblurred)[at+1]!=0 || (*vblurred)[at+2]!=0;
    }
    std::printf("POSTFX_VERTICAL_BLUR_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g clamped_taps=%u "
        "model=5tap_ideal_bilinear+1ulp16+weight_step+fp32_accumulation\n",variant,vOutside,vMaxRatio,vClampedTaps);
    assert(!vOutside && (variant || vNonzero) && vClampedTaps);
    NativeEdram32Bands vowner;const auto vserial=vowner.nextSerial();
    {NativeEdram32Mutation tx(vowner);
     assert(executeNativePostfxBlurDraw(device,vprog,vd,vb,vprog.vertexIdentity,vprog.pixelIdentity,true,tx,&vwhy));}
    assert(vowner.nextSerial()==vserial+1 && std::string(vwhy)=="fenced-postfx-blur-physical7e3");
    const auto vwords=vowner.readKnownColorRows(780,80,80,45);assert(vwords && vwords->size()==80*45);
    for(unsigned i=0;i<80*45;++i) {
        std::array<std::uint32_t,4> bits{};for(unsigned c=0;c<4;++c)bits[c]=colorBits((*vblurred)[i*4+c]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound) && (*vwords)[i]==packed);
    }
    assert(!vowner.readKnownColorRows(780,80,80,46) && !vowner.readColorWords(783,80,80,16));
    NativeEdram32Bands vpending;
    {NativeEdram32Mutation tx(vpending);
     assert(!executeNativePostfxBlurDraw(device,vprog,vd,vb,vprog.vertexIdentity,vprog.pixelIdentity,false,tx));}
    assert(!vpending.knownTiles());
    // The horizontal (pass81) program cannot take the vertical draw: exact pixel identity.
    NativeEdram32Bands vcross;
    {NativeEdram32Mutation tx(vcross);
     assert(!executeNativePostfxBlurDraw(device,bprog,vd,vb,vprog.vertexIdentity,vprog.pixelIdentity,true,tx));}
    assert(!vcross.knownTiles());
    std::printf("POSTFX_VERTICAL_BLUR_CONNECTION_PASS variant=%u pass81_producer=1 t3_resolve_row_reproduced=%u actual_shader=1 "
        "independent_source_linear=1 measured_uv=1 runtime_constants=1 taps=5 axis=y sample_bound=1 nonzero=%u physical7e3=1 "
        "base=780 tiles=3 pending_query_refused=1 horizontal_program_refused=1 gameplay=0\n",variant,unsigned(!variant),vNonzero);
#ifdef RECOMP_PRIVATE_POSTFX_UPSAMPLE
#include "postfx_upsample_connection.inc"
#endif
}
