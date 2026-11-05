// Actual stopped T1 re-resolve (99jco_dd: 160x90 RT3 base768 -> the same T1
// EXPAND29 texture, flags0 = NO clear) on THIS GPU's pass83 additive words,
// never captured pixels. Prior destination bytes are authored; without a clear
// the physical owner must stay exactly as pass83 left it.
{
    const auto t1bWord=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];};
    const auto t1bMeta=read(p+"/t1b-resolve/call.bin",48);
    Gta5TileResolveCapture t1b;
    for(unsigned i=0;i<9;++i)t1b.arguments[i]=t1bWord(t1bMeta,i*4);
    t1b.stack=t1bWord(t1bMeta,36);t1b.f1=(std::uint64_t{t1bWord(t1bMeta,40)}<<32)|t1bWord(t1bMeta,44);
    const auto t1bField=[&](const char* name,unsigned bytes){return read(p+"/t1b-resolve/native-resolve-failure-"+name+".bin",bytes);};
    t1b.state=t1bField("state",13860);t1b.destinationHeader=t1bField("destination-header",52);t1b.surfaces.emplace();
    for(unsigned i=0;i<5;++i)t1b.surfaces->surfaces[i].object=t1bWord(*t1b.state,12816+i*4);
    t1b.surfaces->surfaces[0].header=t1bField("surface-0",40);
    t1b.pointed.rectangle=t1bField("rectangle",16);t1b.pointed.stack92=t1bField("stack92",4);t1b.pointed.stack100=t1bField("stack100",4);
    t1b.pointed.color=t1bField("clear-color",16);
    const char* bwhy2=nullptr;const auto t1bPlan=planNativeRgb7e3FloatResolve(t1b,&bwhy2);
    if(!t1bPlan)std::printf("T1B_RESOLVE_PLAN_REFUSED %s\n",bwhy2);assert(t1bPlan);
    assert(t1bPlan->width==160 && t1bPlan->height==90 && t1bPlan->base==768 && !t1bPlan->clearsSource && t1b.arguments[1]==0 &&
        t1bPlan->destination.baseAddress==tplan->destination.baseAddress && t1bPlan->sourceObject==tplan->sourceObject &&
        t1bPlan->storage.sourceBytes==122880);
    // The same call with the clear flag is the first T1 resolve's row; without it, this row.
    {auto withClear=t1b;withClear.arguments[1]=0x100;const auto q=planNativeRgb7e3FloatResolve(withClear);assert(q && q->clearsSource);}
    const auto t1bKnown=uowner.knownTiles();const auto t1bSerial=uowner.nextSerial();
    const std::vector<std::uint8_t> previousT1b(t1bPlan->storage.sourceBytes,0xAD);
    auto expectedT1b=previousT1b;std::vector<std::uint8_t> expectedT1bLinear(160*90*8);
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x) {
        const auto floats=unpackColor7e3((*uwords)[y*160+x]);
        for(unsigned c=0;c<4;++c) {
            const _Float16 reference=colorFloat(floats[c]);std::uint16_t h=0;std::memcpy(&h,&reference,2);
            for(unsigned byte=0;byte<2;++byte) {
                expectedT1b[(rgb7e3_test::offset(x,y,160)+c*2+byte)^1]=h>>(byte*8);
                expectedT1bLinear[(y*160+x)*8+c*2+byte]=h>>(byte*8);
            }
        }
    }
    auto destinationT1b=previousT1b;unsigned t1bPublications=0;
    const auto publishT1b=[&](const auto& bytes){++t1bPublications;if(destinationT1b!=previousT1b)return false;destinationT1b=bytes;return true;};
    assert(publishNativeRgb7e3FloatResolve(t1b,uowner,previousT1b,[&]{return sameNativeRgb7e3FloatInputs(t1b,t1b);},publishT1b,&bwhy2));
    assert(std::string(bwhy2)=="rgb7e3-float-published" && destinationT1b==expectedT1b && t1bPublications==1);
    // No clear: the owner keeps every pass83 word and its knownness.
    assert(uowner.nextSerial()==t1bSerial && uowner.knownTiles()==t1bKnown && uowner.readKnownColorRows(768,160,160,96)==uwords);
    {auto o=uowner;assert(!recordNativeRgb7e3FloatResolveClear(o,*t1bPlan) && o.knownTiles()==t1bKnown);}
    auto t1bTexture=NativeTextureImage::create(device,t1bPlan->storage.description,destinationT1b);
    assert(t1bTexture && t1bTexture->readback()==std::optional{expectedT1bLinear});
    assert(!publishNativeRgb7e3FloatResolve(t1b,uowner,previousT1b,[]{return false;},publishT1b) && t1bPublications==1);
    std::printf("T1B_RESOLVE_CONNECTION_PASS variant=%u pixels=14400 original_call=1 pass83_producer=1 current_physical=1 "
        "binary16=1 independent_tiling=1 gpu_import=1 stale_refused=1 clear=0 owner_unchanged=1 next_game_shader=0 gameplay=0\n",variant);
#ifdef RECOMP_PRIVATE_POSTFX_BLUR160
#include "postfx_blur160_connection.inc"
#endif
}
