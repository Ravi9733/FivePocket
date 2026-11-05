// Actual stopped T1 resolve (160x90 RT3 base768 -> EXPAND29, colour clear
// flag0x100) on THIS GPU's ABI41 physical words, never captured pixels. Prior
// destination bytes are authored; the renderer contract then makes the clear
// footprint UNKNOWN (no invented clear word, no pre-clear pixels reused).
{
    const auto t1Word=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];};
    const auto t1Meta=read(p+"/t1-resolve/call.bin",48);
    Gta5TileResolveCapture t1;
    for(unsigned i=0;i<9;++i)t1.arguments[i]=t1Word(t1Meta,i*4);
    t1.stack=t1Word(t1Meta,36);t1.f1=(std::uint64_t{t1Word(t1Meta,40)}<<32)|t1Word(t1Meta,44);
    const auto t1Field=[&](const char* name,unsigned bytes){return read(p+"/t1-resolve/native-resolve-failure-"+name+".bin",bytes);};
    t1.state=t1Field("state",13860);t1.destinationHeader=t1Field("destination-header",52);t1.surfaces.emplace();
    for(unsigned i=0;i<5;++i)t1.surfaces->surfaces[i].object=t1Word(*t1.state,12816+i*4);
    t1.surfaces->surfaces[0].header=t1Field("surface-0",40);
    t1.pointed.rectangle=t1Field("rectangle",16);t1.pointed.stack92=t1Field("stack92",4);t1.pointed.stack100=t1Field("stack100",4);
    t1.pointed.color=t1Field("clear-color",16);
    const char* twhy=nullptr;const auto tplan=planNativeRgb7e3FloatResolve(t1,&twhy);
    if(!tplan)std::printf("T1_RESOLVE_PLAN_REFUSED %s\n",twhy);assert(tplan);
    assert(tplan->width==160 && tplan->height==90 && tplan->pitch==160 && tplan->base==768 &&
        tplan->clearsSource && tplan->clearTiles==12 && t1.arguments[1]==0x100);
    assert(tplan->sourceObject==cplan->target.object && *t1.surfaces->surfaces[0].header==*cb.surfaces[0].header);
    const auto t1Serial=cowner.nextSerial();
    const std::vector<std::uint8_t> previousT1(tplan->storage.sourceBytes,0xA9);
    auto expectedT1=previousT1;std::vector<std::uint8_t> expectedT1Linear(160*90*8);
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x) {
        const auto floats=unpackColor7e3((*cwords)[y*160+x]);
        for(unsigned c=0;c<4;++c) {
            const _Float16 reference=colorFloat(floats[c]);std::uint16_t h=0;std::memcpy(&h,&reference,2);
            for(unsigned byte=0;byte<2;++byte) {
                expectedT1[(rgb7e3_test::offset(x,y,160)+c*2+byte)^1]=h>>(byte*8);
                expectedT1Linear[(y*160+x)*8+c*2+byte]=h>>(byte*8);
            }
        }
    }
    auto destinationT1=previousT1;unsigned t1Publications=0;
    const auto publishT1=[&](const auto& bytes){++t1Publications;if(destinationT1!=previousT1)return false;destinationT1=bytes;return true;};
    assert(publishNativeRgb7e3FloatResolve(t1,cowner,previousT1,[&]{return sameNativeRgb7e3FloatInputs(t1,t1);},publishT1,&twhy));
    assert(std::string(twhy)=="rgb7e3-float-published" && destinationT1==expectedT1 && t1Publications==1 && cowner.nextSerial()==t1Serial);
    auto t1Texture=NativeTextureImage::create(device,tplan->storage.description,destinationT1);
    assert(t1Texture && t1Texture->readback()==std::optional{expectedT1Linear});
    // A changed input between pack and publication never publishes.
    assert(!publishNativeRgb7e3FloatResolve(t1,cowner,previousT1,[]{return false;},publishT1) && t1Publications==1);
    // Renderer contract after publication: the recorded clear. t1Cleared is the
    // T1 EDRAM state later draws see: all 12 tiles known RB_COLOR_CLEAR words
    // (rows 0..95), the ABI41 pixels gone. cowner keeps the pre-clear words for
    // the T1 texture reproduction below.
    auto t1Cleared=cowner;assert(recordNativeRgb7e3FloatResolveClear(t1Cleared,*tplan) && t1Cleared.knownTiles()==12);
    {const auto words=t1Cleared.readColorWords(768,160,160,96);
     assert(words && std::all_of(words->begin(),words->end(),[](auto v){return v==NativeRgb7e3FloatResolveClearWord;}));}
    assert(packNativeRgb7e3FloatResolve(t1,t1Cleared,previousT1,&twhy)!=std::optional{expectedT1});
    std::printf("T1_RESOLVE_CONNECTION_PASS variant=%u pixels=14400 original_call=1 abi41_producer=1 current_physical=1 "
        "binary16=1 independent_tiling=1 gpu_import=1 stale_refused=1 clear_words_recorded=1 clear_word=c0000000 "
        "next_game_shader=0 gameplay=0\n",variant);
#ifdef RECOMP_PRIVATE_T2_DOWNSAMPLE
#include "t2_downsample_connection.inc"
#endif
}
