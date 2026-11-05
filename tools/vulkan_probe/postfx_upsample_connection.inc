// PostFX pass83 (T2 80x45 EXPAND -> T1 160x90 RT3 base768, ONE LINEAR slot5
// sample exported as (rgb,0), RB_BLENDCONTROL 0x01010101 = ONE/ONE add)
// consumes THIS GPU's pass82 words (vowner), never the captured source. The
// live T2 resolve after pass82 was not captured; its call has the T2 call's
// row (80x45 base780 flags0x100, destination 06546000 pitch96), so the T2 call
// packs our pass82 words with previous = the captured slot5 bytes and
// variant0 must reproduce the live T2 texture byte-exactly. The destination
// is T1 after its resolve's recorded clear (t1Cleared: [768,780) known
// C0000000, rows 0..95); the add executes into a COPY of it.
{
    auto uFixture=loadPostfxUpsampleFixture(p+"/postfx-upsample",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& ud=uFixture.draw;auto& ub=uFixture.bound;auto& uprog=uFixture.program;
    uprog.vertexCode=code(p+"/postfx-upsample/original.vert.spv");uprog.pixelCode=code(p+"/postfx-upsample/original.frag.spv");
    // Shared post VS (identity and translation); ABI41's pixel size, not its identity.
    assert(validateNativePostfxUpsampleProgram(uprog) && !validateNativeExpanded16CopyProgram(uprog) &&
        !validateNativePostfxBlurFamilyProgram(uprog) && uprog.vertexIdentity==vprog.vertexIdentity &&
        uprog.vertexCode==vprog.vertexCode && uprog.pixelIdentity.size()==cprog.pixelIdentity.size() &&
        uprog.pixelIdentity!=cprog.pixelIdentity && uprog.pixelCode!=cprog.pixelCode);
    const auto liveT2u=ud.textures[0].source;
    const auto uSourceDescription=XenosTextureDescription::decode(ud.textures[0].fetch);
    assert(uSourceDescription && uSourceDescription->width==80 && uSourceDescription->height==45 &&
        uSourceDescription->pitchTexels==96 && uSourceDescription->baseAddress==0x06546000 &&
        uSourceDescription->baseAddress==t2Physical && uSourceDescription->pitchTexels==rplan->destination.pitchTexels &&
        liveT2u.size()==rplan->storage.sourceBytes);
    const char* uwhy=nullptr;
    const auto producedT2u=packNativeRgb7e3FloatResolve(t2,vowner,liveT2u,&uwhy);
    if(!producedT2u)std::printf("POSTFX_UPSAMPLE_T2_PACK_REFUSED %s\n",uwhy);assert(producedT2u);
    unsigned uDiffering=0;for(std::size_t i=0;i<liveT2u.size();++i)uDiffering+=(*producedT2u)[i]!=liveT2u[i];
    std::printf("POSTFX_UPSAMPLE_SOURCE variant=%u differing_bytes=%u\n",variant,uDiffering);
    if(!variant)assert(!uDiffering);
    // Independent Python untiling of the live T2 texture equals the C++ linearizer,
    // and our packed view equals the binary16 conversion of the pass82 words.
    const auto uSourceLinear=read(p+"/postfx-upsample/source-linear-rgba16le.bin",80*45*8);
    assert(linearizeXenosExpanded16FloatStorage(*uSourceDescription,liveT2u)==uSourceLinear);
    const auto uNow=linearizeXenosExpanded16FloatStorage(*uSourceDescription,*producedT2u);assert(uNow);
    for(unsigned i=0;i<80*45;++i) {
        const auto floats=unpackColor7e3((*vwords)[i]);
        for(unsigned c=0;c<4;++c) {
            const _Float16 reference=colorFloat(floats[c]);std::uint16_t h=0;std::memcpy(&h,&reference,2);
            assert((*uNow)[i*8+c*2]==(h&255) && (*uNow)[i*8+c*2+1]==(h>>8));
        }
    }
    ud.textures[0].source=*producedT2u;
    const auto uplan=planNativePostfxUpsampleDraw(ud,ub,uprog,uprog.vertexIdentity,uprog.pixelIdentity,false,true,&uwhy);
    if(!uplan)std::printf("POSTFX_UPSAMPLE_PLAN_REFUSED %s\n",uwhy);assert(uplan);
    assert(uplan->target.width==160 && uplan->target.height==90 && uplan->target.pitch==160 && uplan->target.baseTile==768 &&
        uplan->target.tileCount==12 && uplan->target.object==ub.surfaces[0].object && uplan->submitted.width==80 &&
        uplan->submitted.height==45 && uplan->submitted.pitchTexels==96);
    // Pass83 writes T1 again: ABI41 row0's target object/header, the T1 resolve's source.
    assert(uplan->target.object==cplan->target.object && uplan->target.object==tplan->sourceObject &&
        *ub.surfaces[0].header==*cb.surfaces[0].header);
    const auto upsampled=renderNativePostfxUpsample(device,uprog,ud,*uplan,&uwhy);
    if(!upsampled)std::printf("POSTFX_UPSAMPLE_RENDER_REFUSED %s\n",uwhy);assert(upsampled && upsampled->size()==160*90*4);
    auto uUvProgram=uprog;uUvProgram.pixelCode=code(p+"/postfx-upsample/coordinates.frag.spv");
    const auto uUv=renderNativePostfxUpsample(device,uUvProgram,ud,*uplan,&uwhy);assert(uUv && uUv->size()==160*90*4);
    const auto uHalf=[](unsigned h){_Float16 value;std::memcpy(&value,&h,2);return double(float(value));};
    const auto uTexel=[&](int x,int y,unsigned c){x=std::clamp(x,0,79);y=std::clamp(y,0,44);const auto at=(y*80+x)*8+c*2;
        return uHalf((*uNow)[at]|(unsigned{(*uNow)[at+1]}<<8));};
    unsigned uOutside=0,uNonzero=0,uClamped=0;double uMaxRatio=0,uMaxCentre=0;
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x) {
        const auto at=(y*160+x)*4;const float u=(*uUv)[at],v=(*uUv)[at+1];
        assert(u>0 && u<1 && v>0 && v<1 && (*uUv)[at+2]==0 && (*uUv)[at+3]==0);
        // Diagnostic: distance of the measured UV from the target pixel centre, in source texels.
        uMaxCentre=std::max({uMaxCentre,std::abs(double(u)-(x+.5)/160)*80,std::abs(double(v)-(y+.5)/90)*45});
        // One LINEAR tap at uv on the half-size source: weights 1/4, 3/4 on each axis.
        const double tx=double(u)*80-.5,ty=double(v)*45-.5;const int ix=std::floor(tx),iy=std::floor(ty);
        uClamped+=ix<0 || ix+1>79 || iy<0 || iy+1>44;
        const double wx=tx-ix,wy=ty-iy;
        for(unsigned c=0;c<3;++c) {
            const double t00=uTexel(ix,iy,c),t10=uTexel(ix+1,iy,c),t01=uTexel(ix,iy+1,c),t11=uTexel(ix+1,iy+1,c);
            const double ideal=(1-wx)*(1-wy)*t00+wx*(1-wy)*t10+(1-wx)*wy*t01+wx*wy*t11;
            const double ulp=std::ldexp(1.0,std::max(-24,int(std::floor(std::log2(std::max(std::abs(ideal),1e-30))))-10));
            const double bound=ulp+(std::abs(t10-t00)+std::abs(t11-t01)+std::abs(t01-t00)+std::abs(t11-t10))/256;
            const double deviation=std::abs(double((*upsampled)[at+c])-ideal);
            uMaxRatio=std::max(uMaxRatio,deviation/bound);uOutside+=deviation>bound;
        }
        assert((*upsampled)[at+3]==0 && !std::signbit((*upsampled)[at+3])); // literal alpha 0.0
        uNonzero+=(*upsampled)[at]!=0 || (*upsampled)[at+1]!=0 || (*upsampled)[at+2]!=0;
    }
    std::printf("POSTFX_UPSAMPLE_SAMPLE_BOUND variant=%u outside=%u max_ratio=%.6g clamped_pixels=%u max_centre_texels=%.9g "
        "model=ideal_bilinear+1ulp16+weight_step alpha=0\n",variant,uOutside,uMaxRatio,uClamped,uMaxCentre);
    assert(!uOutside && (variant || uNonzero) && uClamped);
    // Destination: the recorded T1 clear, every word of the 12 tiles known.
    const auto uCleared=t1Cleared.readKnownColorRows(768,160,160,96);
    assert(uCleared && std::all_of(uCleared->begin(),uCleared->end(),[](auto w){return w==NativeRgb7e3FloatResolveClearWord;}));
    const auto clearFloats=unpackColor7e3(NativeRgb7e3FloatResolveClearWord);
    assert(colorFloat(clearFloats[0])==0 && colorFloat(clearFloats[1])==0 && colorFloat(clearFloats[2])==0 && colorFloat(clearFloats[3])==1);
    NativeEdram32Bands uowner=t1Cleared;const auto userial=uowner.nextSerial();
    {NativeEdram32Mutation tx(uowner);
     assert(executeNativePostfxUpsampleDraw(device,uprog,ud,ub,uprog.vertexIdentity,uprog.pixelIdentity,true,tx,&uwhy));}
    if(std::string(uwhy)!="fenced-postfx-upsample-additive-physical7e3")std::printf("POSTFX_UPSAMPLE_EXECUTOR %s\n",uwhy);
    assert(uowner.nextSerial()==userial+1 && std::string(uwhy)=="fenced-postfx-upsample-additive-physical7e3");
    const auto uwords=uowner.readKnownColorRows(768,160,160,96);assert(uwords && uwords->size()==160*96);
    unsigned uAdded=0;
    for(unsigned y=0;y<96;++y)for(unsigned x=0;x<160;++x) {
        const auto w=(*uwords)[y*160+x];
        if(y>=90){assert(w==NativeRgb7e3FloatResolveClearWord);continue;} // rows 90..95: still the known clear
        const auto* s=&(*upsampled)[(y*160+x)*4];
        std::array<std::uint32_t,4> added{},alone{};
        for(unsigned c=0;c<4;++c)added[c]=colorBits(colorFloat(clearFloats[c])+s[c]);
        for(unsigned c=0;c<3;++c)alone[c]=colorBits(s[c]);alone[3]=colorBits(1.0f);
        unsigned wantAdded=0,wantAlone=0;
        assert(packColor7e3(added,wantAdded,Color7e3Rounding::XeniaShiftRound) &&
               packColor7e3(alone,wantAlone,Color7e3Rounding::XeniaShiftRound));
        assert(w==wantAdded && w==wantAlone && (w>>30)==3);                 // rgb = rendered, alpha = 1.0
        uAdded+=w!=NativeRgb7e3FloatResolveClearWord;
    }
    assert(uowner.knownTiles()==t1Cleared.knownTiles() && (variant || uAdded));
    // One unknown destination tile refuses before any GPU work, and leaves the owner unknown.
    NativeEdram32Bands uhole=t1Cleared;assert(uhole.invalidate(779,1));
    {NativeEdram32Mutation tx(uhole);
     assert(!executeNativePostfxUpsampleDraw(device,uprog,ud,ub,uprog.vertexIdentity,uprog.pixelIdentity,true,tx,&uwhy));
     assert(std::string(uwhy)=="destination-colour-unknown");}
    assert(!uhole.knownTiles());
    // AUTHORED nonzero destination (not the game's T1 state, which is the recorded
    // clear): a copy of the pre-clear ABI41 words, known rows 0..89 only. The add
    // consumes exactly those words; rows 90..95 of tiles 778/779 stay unknown.
    NativeEdram32Bands usum=cowner;const auto usumSerial=usum.nextSerial();
    {NativeEdram32Mutation tx(usum);
     assert(executeNativePostfxUpsampleDraw(device,uprog,ud,ub,uprog.vertexIdentity,uprog.pixelIdentity,true,tx,&uwhy));}
    const auto usumWords=usum.readKnownColorRows(768,160,160,90);
    assert(usumWords && usum.nextSerial()==usumSerial+1 && usum.knownTiles()==cowner.knownTiles() &&
        !usum.readKnownColorRows(768,160,160,91));
    unsigned uSaturated=0;
    for(unsigned i=0;i<160*90;++i) {
        const auto before=unpackColor7e3((*cwords)[i]);std::array<std::uint32_t,4> added{};
        for(unsigned c=0;c<4;++c)added[c]=colorBits(colorFloat(before[c])+(*upsampled)[i*4+c]);
        unsigned want=0;assert(packColor7e3(added,want,Color7e3Rounding::XeniaShiftRound) && (*usumWords)[i]==want);
        assert(((*usumWords)[i]>>30)==((*cwords)[i]>>30));                    // destination alpha unchanged
        uSaturated+=colorFloat(added[0])>31.875f || colorFloat(added[1])>31.875f || colorFloat(added[2])>31.875f;
    }
    NativeEdram32Bands upending=t1Cleared;
    {NativeEdram32Mutation tx(upending);
     assert(!executeNativePostfxUpsampleDraw(device,uprog,ud,ub,uprog.vertexIdentity,uprog.pixelIdentity,false,tx,&uwhy));
     assert(std::string(uwhy)=="query-owner-not-idle");}
    assert(!upending.knownTiles());
    // The pass71 route cannot take the upsample draw (same VS, same PS size, different identity).
    NativeEdram32Bands ucross=t1Cleared;
    {NativeEdram32Mutation tx(ucross);
     assert(!executeNativeExpanded16CopyDraw(device,cprog,ud,ub,uprog.vertexIdentity,uprog.pixelIdentity,true,tx));}
    assert(!ucross.knownTiles());
    std::printf("POSTFX_UPSAMPLE_CONNECTION_PASS variant=%u pass82_producer=1 t2_resolve_row_reproduced=%u actual_shader=1 "
        "independent_source_linear=1 measured_uv=1 sample_bound=1 alpha_zero=1 nonzero=%u destination=t1_recorded_clear "
        "blend=add physical7e3=1 added_words=%u base=768 tiles=12 rows90_95_preserved=1 authored_nonzero_destination=1 "
        "saturated=%u unknown_destination_refused=1 pending_query_refused=1 abi41_route_refused=1 gameplay=0\n",
        variant,unsigned(!variant),uNonzero,uAdded,uSaturated);
#ifdef RECOMP_PRIVATE_T1B_RESOLVE
#include "t1b_resolve_connection.inc"
#endif
}
