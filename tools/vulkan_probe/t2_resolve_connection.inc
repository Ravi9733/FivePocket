// Actual stopped T2 resolve (80x45 RT3 base780 -> EXPAND29 tiled pitch96,
// colour clear flag0x100) on THIS GPU's second pass71 words, never captured
// pixels. Prior destination bytes are authored; the renderer contract then
// makes the 3-tile clear footprint UNKNOWN (no invented clear word).
{
    const auto t2Word=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];};
    const auto t2Meta=read(p+"/t2-resolve/call.bin",48);
    Gta5TileResolveCapture t2;
    for(unsigned i=0;i<9;++i)t2.arguments[i]=t2Word(t2Meta,i*4);
    t2.stack=t2Word(t2Meta,36);t2.f1=(std::uint64_t{t2Word(t2Meta,40)}<<32)|t2Word(t2Meta,44);
    const auto t2Field=[&](const char* name,unsigned bytes){return read(p+"/t2-resolve/native-resolve-failure-"+name+".bin",bytes);};
    t2.state=t2Field("state",13860);t2.destinationHeader=t2Field("destination-header",52);t2.surfaces.emplace();
    for(unsigned i=0;i<5;++i)t2.surfaces->surfaces[i].object=t2Word(*t2.state,12816+i*4);
    t2.surfaces->surfaces[0].header=t2Field("surface-0",40);
    t2.pointed.rectangle=t2Field("rectangle",16);t2.pointed.stack92=t2Field("stack92",4);t2.pointed.stack100=t2Field("stack100",4);
    t2.pointed.color=t2Field("clear-color",16);
    const char* rwhy=nullptr;const auto rplan=planNativeRgb7e3FloatResolve(t2,&rwhy);
    if(!rplan)std::printf("T2_RESOLVE_PLAN_REFUSED %s\n",rwhy);assert(rplan);
    assert(rplan->width==80 && rplan->height==45 && rplan->pitch==80 && rplan->base==780 && rplan->clearsSource &&
        rplan->clearTiles==3 && t2.arguments[1]==0x100 && rplan->destination.pitchTexels==96 && rplan->storage.sourceBytes==49152);
    assert(rplan->sourceObject==dplan->target.object && *t2.surfaces->surfaces[0].header==*db.surfaces[0].header);
    const auto t2Serial=downer.nextSerial();
    const std::vector<std::uint8_t> previousT2(rplan->storage.sourceBytes,0xAB);
    auto expectedT2=previousT2;std::vector<std::uint8_t> expectedT2Linear(80*45*8);
    for(unsigned y=0;y<45;++y)for(unsigned x=0;x<80;++x) {
        const auto floats=unpackColor7e3((*dwords)[y*80+x]);
        for(unsigned c=0;c<4;++c) {
            const _Float16 reference=colorFloat(floats[c]);std::uint16_t h=0;std::memcpy(&h,&reference,2);
            for(unsigned byte=0;byte<2;++byte) {
                expectedT2[(rgb7e3_test::offset(x,y,96)+c*2+byte)^1]=h>>(byte*8);
                expectedT2Linear[(y*80+x)*8+c*2+byte]=h>>(byte*8);
            }
        }
    }
    auto destinationT2=previousT2;unsigned t2Publications=0;
    const auto publishT2=[&](const auto& bytes){++t2Publications;if(destinationT2!=previousT2)return false;destinationT2=bytes;return true;};
    assert(publishNativeRgb7e3FloatResolve(t2,downer,previousT2,[&]{return sameNativeRgb7e3FloatInputs(t2,t2);},publishT2,&rwhy));
    assert(std::string(rwhy)=="rgb7e3-float-published" && destinationT2==expectedT2 && t2Publications==1 && downer.nextSerial()==t2Serial);
    auto t2Texture=NativeTextureImage::create(device,rplan->storage.description,destinationT2);
    assert(t2Texture && t2Texture->readback()==std::optional{expectedT2Linear});
    assert(!publishNativeRgb7e3FloatResolve(t2,downer,previousT2,[]{return false;},publishT2) && t2Publications==1);
    // Renderer contract after publication: the recorded clear (3 tiles, rows 0..47).
    auto t2Cleared=downer;assert(recordNativeRgb7e3FloatResolveClear(t2Cleared,*rplan) && t2Cleared.knownTiles()==3);
    {const auto words=t2Cleared.readColorWords(780,80,80,48);
     assert(words && std::all_of(words->begin(),words->end(),[](auto v){return v==NativeRgb7e3FloatResolveClearWord;}));}
    assert(packNativeRgb7e3FloatResolve(t2,t2Cleared,previousT2,&rwhy)!=std::optional{expectedT2});
    std::printf("T2_RESOLVE_CONNECTION_PASS variant=%u pixels=3600 original_call=1 t2_downsample_producer=1 current_physical=1 "
        "binary16=1 independent_tiling=1 tiled_pitch=96 gpu_import=1 stale_refused=1 clear_words_recorded=1 "
        "clear_word=c0000000 next_game_shader=0 gameplay=0\n",variant);
#ifdef RECOMP_PRIVATE_POSTFX_BLUR
#include "postfx_blur_connection.inc"
#endif
}
