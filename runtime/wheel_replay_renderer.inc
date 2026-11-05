// thor-wheel (v601-v619, Thor edition only: included under RECOMP_THOR_EDITION after hud_replay_renderer.inc, as the LAST members of
// Renderer): the game's weapon wheel moved to the lower screen (wheel_replay.h says what the game draws and why this works).
// Reuses hud4's replay machinery (Hud4Op, hud4Op(), hud4Shadow, hud4Redirect, hud4Bank, hud4CodeCopy); its own list, shadows, budget
// and statistics. The existing functions gain only calls (execute(): one before a draw item's draw() - the only call that can stop a
// draw: a held ring candidate or the confirmed wheel - one before a clear item's clear(), one before a resolve and before a work item,
// one after hud4Present at the present; the synchronous present: one after hud4Present).
//
// wheelDraw  (before draw(), renderer thread): classifies the operation for wheelC (wheel_replay.h Decision). flush: the held copies
//            are executed on the real surfaces first (wheelExecuteHeld: through draw(), the redirect off: what the game drew, in its
//            order). Defer / Skip: the operation is copied and not executed (returns true). A copy that fails: the held copies and the
//            operation itself are executed; the frame is not replayed.
// wheelClear / wheelBeforeWork (before a clear / a resolve / a work item): a main-frame clear ends the layer; anything that may read
//            the main frame sees the held copies executed first.
// wheelPresent (at the frame's present, after hud4Present): held copies executed (or settled as the wheel), then the replay when the
//            frame drew the wheel: the overlay cleared to wheelrep::Background, the wheel's operations; published to
//            hud2::screen().publishWheel.
// Off for the run after WheelFailLimit consecutive failed replays (then the main screen keeps its wheel: nothing is held or skipped).
wheelrep::Collector wheelC;Hud4List wheelList;
Target wheelColor,wheelDepth;int wheelK=0;std::uint64_t wheelSerial=0,wheelBytes=0;unsigned wheelFails=0;bool wheelOff=false,wheelLogged=false;
static constexpr std::uint64_t WheelSpanCopy=1u<<20,WheelFrameBudget=24ull<<20;static constexpr unsigned WheelFailLimit=30;
struct WheelStats {std::uint64_t frames=0,replays=0,broken=0,skippedOps=0,heldOps=0,flushedOps=0,flushes=0,refused=0,draws=0,copiedKb=0,inPlaceKb=0,budget=0,
    unexpected=0,cpuNs=0,cpuNsMax=0,gpuNs=0,gpuN=0,exceptions=0,copyNs=0,flushNs=0,copyKbFrames=0;const char* firstRefusal=nullptr;} wheelStats; // v624: copyNs / flushNs
VkQueryPool wheelPool=VK_NULL_HANDLE;unsigned wheelPoolSlot=0;std::array<bool,4> wheelPoolPending{};
// v624: the lower screen's background = the game's own graded frame's mean colour (the main frame after its final post-process,
// ps 08dfab6c, and the backdrop copy, ps 816a99bc: the wheel's green look exactly), brought to the luminance of wheelrep::Background
// (dark). Measured every WheelHueEvery-th wheel replay: one linear blit of the main frame to 16x9 texels (each the filtered mean of its
// 80x80-guest block), copied to a host buffer and read WheelHueLatency presents later; the first measurement replaces the constant.
static constexpr unsigned WheelHueW=16,WheelHueH=9,WheelHueEvery=8,WheelHueLatency=4;
Image wheelHueImage;Buffer wheelHueBuf;bool wheelHuePending=false,wheelHueLogged=false,wheelHueKnown=false;std::uint64_t wheelPresents=0,wheelHueAt=0,wheelHueTick=0;
std::array<float,4> wheelBg{wheelrep::Background[0],wheelrep::Background[1],wheelrep::Background[2],wheelrep::Background[3]};
std::uint64_t wheelFlushLogs=0,wheelFlushLogs2=0;
// v641 (wheel-dedupe; owner 2026-10-23: the wheel breaks after the weapons cheat): every draw copied its texture spans again, so a
// wheel of 84 draws (all weapons; census wheel-cheat-2) passed WheelFrameBudget, the copy failed after the ring was already skipped,
// and the icons fell through to the main screen with no replay (25 draws / 6.4 MB before the cheat). A span already copied in this
// frame (same address and size) whose bytes are still equal (memcmp) is not copied again: the op points at the earlier copy.
// wheelSeen: (address, size, the copy, the op's slot); cleared with the list, pruned when a slot is given back.
struct WheelSeen {std::uint32_t address;std::uint64_t bytes;const std::uint8_t* copy;unsigned slot;};
std::vector<WheelSeen> wheelSeen;std::uint64_t wheelDedupKb=0,wheelBrokenLogs=0;
static_assert(std::is_nothrow_move_constructible_v<Hud4Op>,"wheel-dedupe: a slot's copies must keep their buffers when the list grows");
void wheelSeenPrune(unsigned n) noexcept {std::erase_if(wheelSeen,[n](const WheelSeen& w){return w.slot>=n;});}
void wheelBrokenLog(const char* why) noexcept { // v641: the first 32 broken wheel frames, with the reason (bounded)
    if(wheelBrokenLogs>=32)return;++wheelBrokenLogs;
    GLOG("RENDER_WHEEL_BROKEN n=%llu why=%s list=%u bytes_kb=%llu budget_kb=%llu dedup_kb=%llu",(unsigned long long)wheelBrokenLogs,why,wheelList.n,
         (unsigned long long)(wheelBytes/1024),(unsigned long long)(WheelFrameBudget/1024),(unsigned long long)wheelDedupKb);
}
// v624 (diagnostic for the device's stale lower screen, run gpv_ybmg: 40 identical lower captures while RENDER_HUD4 replays and the
// published replica serial advanced): every 64th present a 64x64 block of the minimap overlay's centre is read back and hashed;
// RENDER_HUD4_PROBE logs the hash with the replica serial. A changing hash under a frozen lower picture = the display path; a
// constant hash while the game's minimap changes = the replay's output.
Buffer mapProbeBuf;bool mapProbePending=false;std::uint64_t mapProbeAt=0,mapProbeSerial=0,mapProbeLogs=0;std::uint32_t mapProbeW=0,mapProbeH=0;

bool wheelWanted() const noexcept {return !wheelOff && recomp::gpu::hud2::screen().wheelWanted() && !census.active && !census.armed;}
// thor-radio (v651): the radio wheel (wheel_replay.h RadioCollector) on the lower screen. It shares wheelList (the copies), the
// execution of held copies and the replay machinery with the weapon wheel; the two collectors are exclusive within a frame (the radio is
// asked only while the weapon wheel holds nothing and was not confirmed; the weapon wheel only while the radio holds nothing and was not
// confirmed), so the list always belongs to one of them. Its own overlay targets, scale and statistics.
wheelrep::RadioCollector radioC;Target radioColor,radioDepth;int radioK=0;
struct RadioStats {std::uint64_t frames=0,replays=0,broken=0,skippedOps=0,heldOps=0,flushes=0,flushedOps=0;} radioStats;
std::uint64_t radioFlushLogs=0;bool radioLogged=false;
// v652 (device v651, run j99jgq5h: 34 radio candidates flushed in ~700 radio frames, 7 with 7 logos = the radio's text and logos on the
// main screen for a frame; every one why=resolve-or-work): a guest WORK item (v183: deferred fence stores and other guest-visible
// writes, run in queue order) arrives inside the radio's run. A work item never reads the main frame; the only reason to execute held
// copies before it is a held copy that still reads guest memory in place (a span over WheelSpanCopy: its bytes may be reused once
// the fence is seen). wheelInPlace = the list holds such a span; without one the radio's candidate stays held across the work item.
bool wheelInPlace=false;std::uint64_t radioWorkKept=0,radioCopyNs=0,radioCopiedKb=0;
bool radioWanted() const noexcept {return !wheelOff && recomp::gpu::hud2::screen().radioWanted() && !census.active && !census.armed;}
// thor-char: the character switch wheel (wheel_replay.h CharCollector): its own list of copies (never held: colour draws skipped, stencil
// masks and clears executed and copied), its own dedupe set, budget, overlay targets and scale; replayed at the present into CharArea.
// wheelSpans copies into the lane wheelLane* points at (the wheel list, or the character list while a character op is copied).
wheelrep::CharCollector charC;Hud4List charList;std::vector<WheelSeen> charSeen;std::uint64_t charBytes=0;Target charColor,charDepth;int charK=0;
Hud4List* wheelLaneList=&wheelList;std::vector<WheelSeen>* wheelLaneSeen=&wheelSeen;std::uint64_t* wheelLaneBytes=&wheelBytes;
std::uint64_t charReplays=0,charBroken=0,charCopyNs=0,charLogs=0;bool charLogged=false;
static constexpr int CharK=64; // (4 x: the wheel 500 px, the stats panel 720 x 1080 texels scaled into the lower screen's left half)
bool charWanted() const noexcept {return !wheelOff && recomp::gpu::hud2::screen().charWanted() && !census.active && !census.armed;}
// v815 (thor-radio): the game's phone on the lower screen (wheel_replay.h PhoneAct): the main frame skips its screen quad, glass and the
// body's composite; the screen's own resolved movie target is copied (phoneImage) and published as a Wheel with phone=true.
bool phoneWanted() const noexcept {return !wheelOff && recomp::gpu::hud2::screen().phoneWanted() && !census.active && !census.armed;}
std::uint64_t qsRunsLogged=0,qsEndsLogged=0; // (v820)
// v824 (thor-radio, phone_pov.h): the phone's floor backdrop - while the phone is up, this frame's present image's patch (the floor beside
// the feet; in a vehicle the view ahead) is blitted into the phone atlas beside the screen movie; the next published Wheel names it.
// v828 (thor-radio, phone_touch.h): a tap on a phone item - this frame's screen movie is read back once (host-visible buffer, read when
// this frame's fence has signalled) and planned: the D-pad steps from the highlight to the tapped item, then A; the pad machine presses them.
Buffer phoneTapBuffer;bool phoneTapFailed=false;std::uint64_t phoneTaps=0;
void phoneTapProbe(const Image& src,bool turned) noexcept {
    namespace mt=recomp::gpu::maptap;namespace pt=recomp::gpu::phonetouch;
    std::int16_t at=0;
    if(!mt::machine().takeTap(wheelNowMs(),at))return;
    if(recomp::phonecall::callActive(recomp::phonecall::state())) { // v852: ringing / in a call - A or B by where, no read-back
        float x=0,y=0,s=-1,t=-1;pt::unpack(at,x,y);
        const auto e=recomp::gpu::hud2::screen().extentHint();
        const auto L=recomp::gpu::hud::phoneLayout(e.width,e.height,turned);
        if(L.ok) {
            const float X=x*float(e.width),Y=y*float(e.height);
            s=(X-L.screen.x0)/(L.screen.x1-L.screen.x0);t=(Y-L.screen.y0)/(L.screen.y1-L.screen.y0);
        }
        const auto p=pt::plan(pt::Movie{},s,t,true);
        ++phoneTaps;
        GLOG("RENDER_PHONE_TAP %s n=%llu at=%.3f,%.3f s=%.3f t=%.3f call=%u kind=%u keys=%zu",mt::PhoneTouchMarker,(unsigned long long)phoneTaps,
             double(x),double(y),double(s),double(t),recomp::phonecall::state(),unsigned(p.kind),p.keys.size());
        if(!p.keys.empty())mt::machine().setSteps(p.keys,wheelNowMs());
        return;
    }
    const VkDeviceSize bytes=VkDeviceSize(src.width)*src.height*4;
    if(phoneTapFailed || src.format!=VK_FORMAT_R8G8B8A8_UNORM || !src.width || !src.height) { // (a movie this reader cannot read: a plain A, as v815)
        mt::machine().setSteps({pt::Key::A},wheelNowMs());GLOG("RENDER_PHONE_TAP plain=1 why=format vkfmt=%d",int(src.format));return;
    }
    if(!phoneTapBuffer.buffer || phoneTapBuffer.size<bytes) {
        if(phoneTapBuffer.buffer)destroyBuffer(phoneTapBuffer);
        if(!makeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,phoneTapBuffer)){phoneTapFailed=true;mt::machine().setSteps({pt::Key::A},wheelNowMs());GLOG("RENDER_PHONE_TAP buffer failed");return;}
    }
    endPass(__LINE__);hazards({src.image},{});
    VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={src.width,src.height,1};
    vkCmdCopyImageToBuffer(cmd,src.image,VK_IMAGE_LAYOUT_GENERAL,phoneTapBuffer.buffer,1,&c);
    VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
    const std::uint32_t mw=src.width,mh=src.height;
    garbage.push_back([this,at,turned,mw,mh]{phoneTapPlan(at,turned,mw,mh);});
}
// v834 (quick_save.h state-driven fallback): while the Quick Save macro waits on the phone, every captured phone frame is read back and its
// grid highlight handed to the pad machine (phone_touch.h gridSelection; -1 none / a landscape app). Nothing otherwise.
Buffer phoneSelBuffer;bool phoneSelFailed=false;std::uint64_t phoneSelReads=0;
void phoneSelProbe(const Image& src,bool turned) noexcept {
    namespace mt=recomp::gpu::maptap;
    if(!mt::machine().quickSaveWantsSel())return;
    if(turned || phoneSelFailed || src.format!=VK_FORMAT_R8G8B8A8_UNORM || !src.width || !src.height){mt::machine().setPhoneSel(-1);return;}
    const VkDeviceSize bytes=VkDeviceSize(src.width)*src.height*4;
    if(!phoneSelBuffer.buffer || phoneSelBuffer.size<bytes) {
        if(phoneSelBuffer.buffer)destroyBuffer(phoneSelBuffer);
        if(!makeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,phoneSelBuffer)){phoneSelFailed=true;GLOG("RENDER_QSAVE sel buffer failed");return;}
    }
    endPass(__LINE__);hazards({src.image},{});
    VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={src.width,src.height,1};
    vkCmdCopyImageToBuffer(cmd,src.image,VK_IMAGE_LAYOUT_GENERAL,phoneSelBuffer.buffer,1,&c);
    VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
    const std::uint32_t mw=src.width,mh=src.height;
    garbage.push_back([this,mw,mh]{
        if(!phoneSelBuffer.mapped)return;
        const recomp::gpu::phonetouch::Movie m{static_cast<const std::uint8_t*>(phoneSelBuffer.mapped),mw,mh,mw*4,false};
        const int sel=recomp::gpu::phonetouch::gridSelection(m);
        recomp::gpu::maptap::machine().setPhoneSel(sel);
        if(!(phoneSelReads++%16))GLOG("RENDER_QSAVE sel=%d reads=%llu",sel,(unsigned long long)phoneSelReads);
    });
}
void phoneTapPlan(std::int16_t at,bool turned,std::uint32_t mw,std::uint32_t mh) noexcept {
    namespace mt=recomp::gpu::maptap;namespace pt=recomp::gpu::phonetouch;
    if(!phoneTapBuffer.mapped)return;
    try {
        float x=0,y=0;pt::unpack(at,x,y);
        const auto e=recomp::gpu::hud2::screen().extentHint();
        const auto L=recomp::gpu::hud::phoneLayout(e.width,e.height,turned);
        if(!L.ok)return;
        const float X=x*float(e.width),Y=y*float(e.height);
        const float s=(X-L.screen.x0)/(L.screen.x1-L.screen.x0),t=(Y-L.screen.y0)/(L.screen.y1-L.screen.y0);
        const pt::Movie m{static_cast<const std::uint8_t*>(phoneTapBuffer.mapped),mw,mh,mw*4,turned};
        const auto p=pt::plan(m,s,t);
        ++phoneTaps;
        GLOG("RENDER_PHONE_TAP %s n=%llu at=%.3f,%.3f s=%.3f t=%.3f turned=%u kind=%u from=%d,%d to=%d,%d keys=%zu",mt::PhoneTouchMarker,(unsigned long long)phoneTaps,
             double(x),double(y),double(s),double(t),unsigned(turned),unsigned(p.kind),p.fromRow,p.fromCol,p.toRow,p.toCol,p.keys.size());
        if(!p.keys.empty())mt::machine().setSteps(p.keys,wheelNowMs());
    } catch(...) {}
}
std::uint32_t phoneScreenW=0,phoneScreenH=0;recomp::gpu::phonepov::Rect povDst{};recomp::gpu::phonepov::Params povParams{};
std::int64_t povParamsAt=-(std::int64_t(1)<<40);std::uint64_t povBlits=0;bool povLogged=false,povCar=false;
void povPresent(const Image& shown,unsigned pw,unsigned ph) noexcept {
    namespace pp=recomp::gpu::phonepov;
    if(!phoneT.up() || !phoneWanted() || !phoneImage.image || !phoneScreenW || !shown.image || census.active)return;
    const std::int64_t now=wheelNowMs();
    if(now-povParamsAt>=1000) { // (the tunables, once a second while the phone is up)
        povParamsAt=now;pp::Params p{};
        if(FILE* f=std::fopen(RECOMP_APP_PATH("/private-trace/diag-phone-pov"),"r")){char b[256]{};const auto n=std::fread(b,1,sizeof b-1,f);b[n]=0;std::fclose(f);pp::parse(b,p);}
        povParams=p;
    }
    const auto* base=recomp::gpu::hud::guestBase().load(std::memory_order_acquire);
    const auto ctx=base?recomp::touchctx::read(recomp::touchctx::GuestMemory{base}).context:recomp::touchctx::None;
    const bool car=ctx!=recomp::touchctx::None && ctx!=recomp::touchctx::Foot;
    const auto c=pp::choose(povParams,car,pw,ph,phoneScreenW);
    if(c.src.empty() || c.dst.empty() || std::uint32_t(c.dst.x1)>phoneImage.width || std::uint32_t(c.dst.y1)>phoneImage.height){povDst={};return;}
    hazards({shown.image},{phoneImage.image});
    VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    b.srcOffsets[0]={c.src.x0,c.src.y0,0};b.srcOffsets[1]={c.src.x1,c.src.y1,1};b.dstOffsets[0]={c.dst.x0,c.dst.y0,0};b.dstOffsets[1]={c.dst.x1,c.dst.y1,1};
    vkCmdBlitImage(cmd,shown.image,VK_IMAGE_LAYOUT_GENERAL,phoneImage.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_LINEAR);
    barrier();
    povDst=c.dst;++povBlits;
    if(!povLogged || car!=povCar){povLogged=true;povCar=car;GLOG("RENDER_PHONE_POV car=%u src=%d,%d..%d,%d dst=%d,%d..%d,%d frame=%ux%u blits=%llu",unsigned(car),
        c.src.x0,c.src.y0,c.src.x1,c.src.y1,c.dst.x0,c.dst.y0,c.dst.x1,c.dst.y1,pw,ph,(unsigned long long)povBlits);}
}
wheelrep::PhoneTracker phoneT;Image phoneImage;std::vector<Image> phoneRetired;std::uint64_t phoneSkipped=0,phoneCaptures=0,phoneMisses=0;bool phoneLogged=false,phoneWas=false;
void phoneCapture(const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask) {
    phoneT.screen();
    std::optional<XenosTextureDescription> d;
    for(unsigned k=0;k<32 && !d;++k)if((fetchMask>>k)&1u)d=XenosTextureDescription::decode(fetch[k]);
    if(!d){++phoneMisses;return;}
    const auto it=resolved.find(physicalAddress(d->baseAddress));
    if(it==resolved.end() || !it->second.image.image || it->second.image.k!=rscale::One || it->second.image.layers!=1){++phoneMisses;return;}
    const Image& src=it->second.image;
    namespace pp=recomp::gpu::phonepov;
    const std::uint32_t aw=pp::atlasW(src.width),ah=pp::atlasH(src.height); // (v824: the screen movie at 0,0, the floor patch beside it)
    if(!phoneImage.image || phoneImage.width!=aw || phoneImage.height!=ah || phoneImage.format!=src.format || phoneScreenW!=src.width || phoneScreenH!=src.height) {
        if(phoneImage.image)phoneRetired.push_back(phoneImage); // (a published image is never freed while the process runs)
        phoneImage={};
        const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
        if(!makeImage(aw,ah,1,src.format,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT,false,identity,phoneImage)) {
            phoneImage={};++phoneMisses;return;
        }
        toGeneral(phoneImage);phoneScreenW=src.width;phoneScreenH=src.height;povDst={}; // (v824)
    }
    endPass(__LINE__);hazards({src.image},{phoneImage.image});barrier();
    VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={src.width,src.height,1};
    vkCmdCopyImage(cmd,src.image,VK_IMAGE_LAYOUT_GENERAL,phoneImage.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
    hazards({},{phoneImage.image});afterWrite();
    recomp::gpu::hud2::Wheel w;
    w.image=phoneImage.image;w.view=phoneImage.view;w.format=phoneImage.format;w.width=phoneImage.width;w.height=phoneImage.height;
    w.ring[0]=0;w.ring[1]=0;w.ring[2]=float(src.width);w.ring[3]=float(src.height);w.k=16; // (v824: the movie's own texels in the atlas)
    if(!povDst.empty()){w.stats[0]=float(povDst.x0);w.stats[1]=float(povDst.y0);w.stats[2]=float(povDst.x1);w.stats[3]=float(povDst.y1);} // (v824: the floor patch)
    w.serial=++wheelSerial;w.phone=true;w.phoneTurn=phoneT.landscape; // (v822: phoneTurn)
    recomp::gpu::hud2::screen().publishWheel(w);++phoneCaptures;
    phoneTapProbe(src,phoneT.landscape); // (v828)
    phoneSelProbe(src,phoneT.landscape); // (v834)
    if(!phoneLogged){phoneLogged=true;GLOG("RENDER_PHONE first=1 texture=%08x %ux%u vkfmt=%d marker=%s",d->baseAddress,src.width,src.height,int(src.format),recomp::gpu::maptap::PhoneMarker);}
}
// v817 / v818: the help / message flags per present (RENDER_TEXT on a change). v818: the text lane - while the help box is shown, the
// frame's full-movie Scaleform draws are copied (wheelrep::textCopy) and replayed into textColor (wheelReplay text) for the lower screen.
bool textHelp=false,textMsg=false,textFrameOn=false,textBroken=false;
bool textCensusOn=false,textCensusPending=false;unsigned textCensuses=0;static constexpr unsigned TextCensusMax=6; // (v820: diag-text-census)
Hud4List textList;std::vector<WheelSeen> textSeen;std::uint64_t textBytes=0,textReplays=0,textBrokenFrames=0;Target textColor,textDepth;bool textLogged=false;
bool textWanted() const noexcept {return !wheelOff && recomp::gpu::hud2::screen().textWanted() && !census.active && !census.armed;}
bool textCopyDraw(const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask,
                  const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned) {
    if(!in.vertexConstants || !in.pixelConstants)return false;
    wheelLaneList=&textList;wheelLaneSeen=&textSeen;wheelLaneBytes=&textBytes;
    bool ok=false;
    try{ok=wheelCopy(textList.next(),in,fetch,fetchMask,spans,owned);}catch(...){ok=false;}
    wheelLaneList=&wheelList;wheelLaneSeen=&wheelSeen;wheelLaneBytes=&wheelBytes;
    if(!ok){--textList.n;std::erase_if(textSeen,[this](const WheelSeen& w){return w.slot>=textList.n;});}
    return ok;
}
void textPresent() noexcept {
    const auto* base=recomp::gpu::hud::guestBase().load(std::memory_order_acquire);
    if(!base)return;
    const auto s=recomp::gpu::hud::readTextState(recomp::gpu::hud::GuestMemory{base});
    if(s.help!=textHelp || s.message!=textMsg) {
        if(s.message && !textMsg && textCensusOn && textCensuses<TextCensusMax){textCensusPending=true;++textCensuses;} // (v820, diagnostic)
        textHelp=s.help;textMsg=s.message;GLOG("RENDER_TEXT help=%u message=%u replays=%llu broken=%llu census=%u",unsigned(s.help),unsigned(s.message),
            (unsigned long long)textReplays,(unsigned long long)textBrokenFrames,unsigned(textCensusPending));
    }
}
void phonePresent() noexcept {
    textPresent(); // (v817)
    phoneT.present();
    const bool up=phoneT.up() && phoneWanted();
    recomp::gpu::maptap::machine().setPhone(up);
    recomp::phonecall::setUp(up); // v852 (recomp_phone_state_v1 bit 0)
    if(!up)recomp::gpu::maptap::machine().setPhoneSel(-1); // (v834)
    {static bool hooked=false;if(!hooked){hooked=true;recomp::gpu::maptap::machine().setSaveMenuHook(&recomp_trainer_v1);}} // (v834: the native save menu)
    recomp::gpu::maptap::machine().setPhoneKnown(phoneWanted()); // (v820: the Quick Save macro waits for the phone only when it can see it)
    { // v820: one RENDER_QSAVE line per macro start and end
        const auto q=recomp::gpu::maptap::machine().quickSaveStatus();
        if(q.runs!=qsRunsLogged || q.ends!=qsEndsLogged){qsRunsLogged=q.runs;qsEndsLogged=q.ends;
            GLOG("RENDER_QSAVE %s runs=%llu ends=%llu busy=%u phone_known=%u last=%s ms=%lld",recomp::gpu::maptap::SaveMarker,(unsigned long long)q.runs,(unsigned long long)q.ends,
                unsigned(q.busy),unsigned(q.known),recomp::gpu::maptap::quickSaveEndName(q.last),(long long)recomp::gpu::maptap::machine().quickSaveMs());}
    }
    if(up!=phoneWas){phoneWas=up;GLOG("RENDER_PHONE up=%u captures=%llu skipped=%llu misses=%llu commands=%llu",unsigned(up),(unsigned long long)phoneCaptures,
        (unsigned long long)phoneSkipped,(unsigned long long)phoneMisses,(unsigned long long)recomp::gpu::maptap::machine().phoneCommands());}
}
// A copy of a draw (or clear) into the character list (the lane switched for wheelSpans' dedupe and budget).
bool charCopyDraw(const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask,
                  const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned) {
    if(!in.vertexConstants || !in.pixelConstants)return false;
    wheelLaneList=&charList;wheelLaneSeen=&charSeen;wheelLaneBytes=&charBytes;
    bool ok=false;
    try{ok=wheelCopy(charList.next(),in,fetch,fetchMask,spans,owned);}catch(...){ok=false;}
    wheelLaneList=&wheelList;wheelLaneSeen=&wheelSeen;wheelLaneBytes=&wheelBytes;
    if(!ok){--charList.n;std::erase_if(charSeen,[this](const WheelSeen& w){return w.slot>=charList.n;});}
    return ok;
}
static std::int64_t wheelNowMs() noexcept {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
// The collector's view of an operation's state.
wheelrep::Op wheelClass(const StateView& st) const noexcept {
    wheelrep::Op o;
    const auto ci=reg(st,0x2001);
    o.main=(reg(st,0x2000)&0x3FFFu)==hudrep::MainPitch && storageColorFormat((ci>>16)&15)==3;
    float v[4];for(unsigned k=0;k<4;++k){const auto w=reg(st,0x210F+k);std::memcpy(&v[k],&w,4);} // PA_CL_VPORT_XSCALE, XOFFSET, YSCALE, YOFFSET
    o.fullMovie=v[0]==640.0f && v[1]==640.0f && v[2]==-360.0f && v[3]==360.0f;
    for(unsigned k=0;k<4;++k)o.vp[k]=v[k];o.colour=(reg(st,0x2104)&0xFu)!=0; // (thor-char: the viewport; RB_COLOR_MASK target 0)
    return o;
}
bool wheelSpans(Hud4Op& op,const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned) {
    op.spans.clear();std::size_t used=0;const unsigned slot=unsigned(&op-wheelLaneList->ops.data()); // (wheel-dedupe)
    for(const auto& s:spans) {
        if(!s.host){op.spans.push_back(s);continue;}
        bool own=false;for(const auto& v:owned)if(s.host>=v.data() && s.host<v.data()+v.size()){own=true;break;}
        if(s.bytes<=WheelSpanCopy || own) {
            { // wheel-dedupe (v641): the same span, unchanged, copied earlier this frame
                const WheelSeen* hit=nullptr;
                for(const auto& w:(*wheelLaneSeen))if(w.address==s.address && w.bytes==s.bytes && std::memcmp(w.copy,s.host,s.bytes)==0){hit=&w;break;}
                if(hit){op.spans.push_back({s.address,s.bytes,hit->copy});wheelDedupKb+=s.bytes/1024;continue;}
            }
            if((*wheelLaneBytes)+s.bytes>WheelFrameBudget){++wheelStats.budget;wheelBrokenLog("budget");return false;}
            if(used==op.spanBytes.size())op.spanBytes.emplace_back();
            auto& b=op.spanBytes[used++];b.assign(s.host,s.host+s.bytes);
            op.spans.push_back({s.address,s.bytes,b.data()});(*wheelLaneBytes)+=s.bytes;wheelStats.copiedKb+=s.bytes/1024;
            (*wheelLaneSeen).push_back({s.address,s.bytes,b.data(),slot}); // (wheel-dedupe)
        } else {op.spans.push_back(s);wheelStats.inPlaceKb+=s.bytes/1024;if(wheelLaneList==&wheelList)wheelInPlace=true;} // (v652: wheelInPlace; thor-char: the wheel lane only)
    }
    return true;
}
// A copy of a draw about to be executed (or skipped): what hud4Op() needs to draw it again (hud4Draw's copy, the same fields).
bool wheelCopy(Hud4Op& op,const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask,
               const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned) {
    op.clear=false;op.rect=hudrep::drawScissor(in.scissor);
    op.state.assign(13860,0);for(const auto& [b,e]:Hud4StateRanges)std::memcpy(op.state.data()+b,in.stateBytes+b,e-b);
    op.vc=hud4Bank(in.vertexConstants,in.vertexConstantsVersion,hud4LastVc,hud4LastVcVersion);op.vcVersion=in.vertexConstantsVersion;
    op.pc=hud4Bank(in.pixelConstants,in.pixelConstantsVersion,hud4LastPc,hud4LastPcVersion);op.pcVersion=in.pixelConstantsVersion;
    op.fetch=fetch;op.fetchMask=fetchMask;op.scissor=in.scissor;
    op.identity=hud4CodeCopy(in.vertexIdentity,in.vertexIdentityVersion);
    op.effective=in.vertexEffective==in.vertexIdentity?op.identity:hud4CodeCopy(in.vertexEffective,in.vertexEffectiveVersion);
    op.pixel=hud4CodeCopy(in.pixelCode,in.pixelVersion);
    op.alternates=in.vertexAlternates?std::make_shared<const std::vector<std::vector<std::uint8_t>>>(*in.vertexAlternates):nullptr;
    op.identityVersion=in.vertexIdentityVersion;op.effectiveVersion=in.vertexEffectiveVersion;op.pixelVersion=in.pixelVersion;
    op.primitive=in.primitive;op.indicesFinal=in.indicesFinal;op.baseVertex=in.baseVertex;op.vertexCount=in.vertexCount;
    if(in.rawIndices && in.rawIndexCount){op.indices.resize(in.rawIndexCount);recomp::gpu::hot::bigEndianIndices(in.rawIndices,in.rawIndexCount,in.rawIndex32,op.indices.data());}
    else op.indices.assign(in.indices.begin(),in.indices.end());
    op.haveInline=in.inlineVertices!=nullptr;if(op.haveInline)op.inlineVertices=*in.inlineVertices;else op.inlineVertices.clear();
    op.inlineStride=in.inlineStride;op.source=in.source;
    return wheelSpans(op,spans,owned);
}
// The held copies (the list's first held() entries: a candidate is only held before the wheel is confirmed, so it is the whole list)
// executed on the real surfaces, in order, through draw() with the redirect off - the draws the game made, one candidate later.
// Renderer thread; the current item's spans and viewer are restored after.
void wheelExecuteHeld(const char* why="",const wheelrep::Op* by=nullptr,bool radio=false) noexcept {
    const unsigned count=wheelList.n;
    if(!count)return;
    if(radio) { // v651: a radio candidate executed on the main frame (logos>0 = radio content on the main screen: should not happen)
        ++radioStats.flushes;radioStats.flushedOps+=count;
        if(radioFlushLogs<64 || radioC.lastLogos){if(radioFlushLogs<256){++radioFlushLogs;
            GLOG("RENDER_RADIO_FLUSH n=%llu held=%u logos=%u why=%s by_sf=%d by_full=%d by_ps=%08llx by_indices=%u flushed_logos_total=%llu",(unsigned long long)radioFlushLogs,
                 count,radioC.lastLogos,why,by?int(by->scaleform):-1,by?int(by->fullMovie):-1,by?(unsigned long long)(by->ps>>32):0ull,by?by->indices:0u,(unsigned long long)radioC.flushedLogos);}}
    } else if(wheelFlushLogs<64 || (count>=2 && wheelFlushLogs2<64)) { // v624 (diagnostic, bounded): what the held candidate was and what ended it (v634: + 64 of 2+ ops)
        if(wheelFlushLogs<64)++wheelFlushLogs;else ++wheelFlushLogs2;char held[200];int at=0;
        for(unsigned i=0;i<count && at<int(sizeof held)-8;++i)at+=std::snprintf(held+at,sizeof held-std::size_t(at),"%s%zu",i?",":"",wheelList.ops[i].indices.size());
        GLOG("RENDER_WHEEL_FLUSH n=%llu held=%u at_pos=%u why=%s by_main=%d by_clear=%d by_sf=%d by_full=%d by_ring_ps=%d by_indices=%u held_indices=%s",
             (unsigned long long)(wheelFlushLogs+wheelFlushLogs2),count,wheelC.lastPos,why,by?int(by->main):-1,by?int(by->clear):-1,by?int(by->scaleform):-1,by?int(by->fullMovie):-1,
             by?int(wheelrep::ringShader(by->ps)):-1,by?by->indices:0u,held);
    }
    const auto tf=Clock::now();
    const auto* const spans=asyncSpans;
    if(!hud4Viewer)hud4Init();
    std::swap(viewer,hud4Viewer);
    const auto refusals=hud4Stats.refused,hdraws=hud4Stats.draws;const char* const firstRefusal=hud4Stats.firstRefusal;
    try{for(unsigned i=0;i<count;++i)hud4Op(wheelList.ops[i],0,0);}catch(...){++wheelStats.exceptions;}
    std::swap(viewer,hud4Viewer);asyncSpans=spans;
    if(hud4Stats.refused!=refusals){wheelStats.refused+=hud4Stats.refused-refusals;if(!wheelStats.firstRefusal)wheelStats.firstRefusal=hud4Stats.firstRefusal;
        hud4Stats.refused=refusals;hud4Stats.firstRefusal=firstRefusal;}
    hud4Stats.draws=hdraws;
    wheelStats.flushedOps+=count;++wheelStats.flushes;wheelList.n=0;wheelBytes=0;wheelSeen.clear();wheelInPlace=false;wheelStats.flushNs+=nsSince(tf);
}
[[gnu::noinline]] bool wheelDraw(const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask,
                                 const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned) noexcept {
    if(!in.stateBytes)return false;
    const bool ww=wheelWanted(),rw=radioWanted(),cw=charWanted(),pw=phoneWanted(),tw=textFrameOn && textWanted(); // (v651: each wheel its own setting; thor-char: cw; v815: pw; v818: tw)
    if(!ww && wheelC.held()){wheelC.fail();wheelExecuteHeld("unwanted");} // (the setting / the screen went away mid-frame)
    if(!rw && radioC.heldOps()){radioC.fail();wheelExecuteHeld("unwanted",nullptr,true);}
    if(!ww && !rw && !cw && !pw && !tw)return false;
    if((be32(in.stateBytes+Hud4SurfaceInfo)&0x3FFFu)!=hudrep::MainPitch)return false; // (the common case: not the main frame)
    bool radioActor=false;
    try {
        const StateView st{in.stateBytes,13860};
        auto o=wheelClass(st);
        if(o.main && in.pixelCode && in.vertexIdentity) {
            o.ps=codeKey(*in.pixelCode,in.pixelVersion).hash;o.scaleform=ui::scaleformPixelShader(o.ps)!=nullptr || wheelrep::ringShader(o.ps);
            o.indices=in.rawIndices?in.rawIndexCount:unsigned(in.indices.size());
        } else o.main=false; // (no code: not something the wheel draws)
        if(pw && o.main) { // v815: the phone (nothing held: its draws are known by their pixel shaders)
            const auto sc=hudrep::drawScissor(in.scissor);
            if(o.ps==wheelrep::PhoneCompositePs && sc.x0>=wheelrep::PhoneBoxX0)phoneT.composite(wheelrep::phoneLandscape(o.ps,sc)); // (v822)
            const auto pa=wheelrep::phoneAct(o.ps,sc,phoneT.active(wheelrep::phoneUpRecent(wheelNowMs())));
            if(pa!=wheelrep::PhoneAct::Draw){if(pa==wheelrep::PhoneAct::SkipCapture)phoneCapture(fetch,fetchMask);++phoneSkipped;return true;}
        }
        if(tw && !textBroken && wheelrep::textCopy(o,true)) { // v818: the help box (copied; drawn on the main frame as well)
            if(textList.n>=wheelrep::TextMaxOps || !textCopyDraw(in,fetch,fetchMask,spans,owned))textBroken=true;
        }
        if(!ww && !rw && !cw)return false;
        if(cw && o.main) { // thor-char: the character wheel's operations are known by their viewports (nothing is held)
            const auto part=wheelrep::charPart(o.vp);
            if(part!=wheelrep::CharPart::None) {
                const auto cd=charC.op(o,part,charC.open() || wheelrep::charButtonRecent(wheelNowMs()));
                if(cd.act!=wheelrep::Act::Draw) {
                    const auto tc=Clock::now();const bool ok=charCopyDraw(in,fetch,fetchMask,spans,owned);charCopyNs+=nsSince(tc);
                    if(!ok){charC.fail();return false;} // (not copied: drawn on the main frame; the frame is not replayed)
                    return cd.act==wheelrep::Act::Skip; // (Copy: executed as well)
                }
            }
        }
        if(!ww && !rw)return false;
        wheelrep::Decision d{};
        if(rw && (radioC.busy() || !(wheelC.held() || wheelC.wheel))) { // v651: the radio first (its candidate starts before any ring)
            const bool layerOp=o.main && o.scaleform && o.fullMovie && !o.clear;
            const bool gate=layerOp && !radioC.busy() && !radioC.open() && wheelrep::radioButtonRecent(wheelNowMs());
            d=radioC.op(o,gate);
            if(d.flush)wheelExecuteHeld("op",&o,true);
            if(d.act!=wheelrep::Act::Draw)radioActor=true;
            else if(radioC.radio)return false; // (the radio's frame: the weapon wheel's collector is not asked)
        }
        if(!radioActor) {
            if(!ww || radioC.busy())return false;
            d=wheelC.op(o,wheelrep::weaponButtonRecent(wheelNowMs())); // (v813: LB held = the wheel from its first ring-shaped draw)
            if(d.flush)wheelExecuteHeld("op",&o);
            if(d.act==wheelrep::Act::Draw)return false;
        }
        const auto tc=Clock::now();const auto kb0=wheelStats.copiedKb;
        const bool slot=in.vertexConstants && in.pixelConstants; // (v624: next() only with a slot to fill; v619-v623 decremented n without one)
        const bool copied=slot && wheelCopy(wheelList.next(),in,fetch,fetchMask,spans,owned);
        const auto cns=nsSince(tc);wheelStats.copyNs+=cns;
        if(radioActor){radioCopyNs+=cns;radioCopiedKb+=wheelStats.copiedKb-kb0;} // (v652)
        if(!copied) {
            if(slot){--wheelList.n;wheelSeenPrune(wheelList.n);} // (wheel-dedupe: the slot is reused; its copies are gone)
            const bool held=d.act==wheelrep::Act::Defer;
            if(radioActor){radioC.fail();if(held)wheelExecuteHeld("copy-failed",&o,true);} // (v651)
            else {wheelC.fail();if(held)wheelExecuteHeld("copy-failed",&o);} // (then this operation itself, by the caller)
            return false;
        }
        if(radioActor)++(d.act==wheelrep::Act::Skip?radioStats.skippedOps:radioStats.heldOps);
        else ++(d.act==wheelrep::Act::Skip?wheelStats.skippedOps:wheelStats.heldOps);
        return true;
    } catch(...) {
        if(radioC.heldOps()){radioC.fail();wheelExecuteHeld("exception",nullptr,true);}else radioC.fail(); // (v651)
        if(wheelC.held()){wheelC.fail();wheelExecuteHeld("exception");}else wheelC.fail();++wheelStats.exceptions;return false;}
}
// Before a clear executes: a main-frame clear ends the layer (held copies executed first, or the ring confirmed).
[[gnu::noinline]] void wheelClear(const ClearInput& in) noexcept {
    const bool ww=wheelWanted(),rw=radioWanted(),cw=charWanted();
    if((!ww && !rw && !cw) || !in.state || in.state->size()<13860)return;
    const StateView st{*in.state};auto o=wheelClass(st);o.clear=true;
    if(cw && o.main) { // thor-char: a clear of the wheel's (the stencil clear before each stats bar's mask; census C772: flags 0x20, rect =
                       // the stats panel's scissor): by its viewport, or by its rectangle being one of the two clips' scissors; copied, executed
        float vp[4];for(unsigned i=0;i<4;++i){const auto v=field(st,13024+4*i);std::memcpy(&vp[i],&v,4);}
        const hudrep::Rect sc{std::int32_t(field(st,13052)),std::int32_t(field(st,13056)),std::int32_t(field(st,13060)),std::int32_t(field(st,13064))};
        const auto rect=hudrep::clearRect(vp,in.hasRect?std::optional<std::array<std::int32_t,4>>(in.rect):std::nullopt,!legacyClears && field(st,12288)!=0,sc);
        auto part=wheelrep::charPart(o.vp);
        if(part==wheelrep::CharPart::None)part=rect==wheelrep::CharStatsClip?wheelrep::CharPart::Stats:rect==wheelrep::CharWheelClip?wheelrep::CharPart::Wheel:wheelrep::CharPart::None;
        if(part!=wheelrep::CharPart::None && charC.op(o,part,charC.open() || wheelrep::charButtonRecent(wheelNowMs())).act!=wheelrep::Act::Draw) {
            try {
                auto& op=charList.next();
                op.clear=true;op.rect=rect;
                op.state=*in.state;op.clearIn=in;op.clearIn.state=nullptr; // (pointed at op.state when replayed, as hud4Clear's)
                op.spans.clear();op.vc=op.pc=nullptr;op.identity=op.effective=op.pixel=nullptr;op.alternates=nullptr;op.scissor.reset();
            } catch(...) {charC.fail();}
        }
    }
    if(!ww && !rw)return;
    if(rw && (radioC.busy() || !(wheelC.held() || wheelC.wheel))) { // v651: a main-frame clear ends the radio's run too
        if(radioC.op(o,false).flush)wheelExecuteHeld("clear",&o,true);
        if(radioC.radio)return;
    }
    if(ww && wheelC.op(o).flush)wheelExecuteHeld("clear",&o);
}
// Before a resolve or a work item executes (either may read the main frame): held copies executed first.
// v652: work = a guest work item (not a resolve): a radio candidate whose copies own all their bytes stays held (wheelInPlace).
[[gnu::noinline]] void wheelBeforeWork(bool work=false) noexcept {
    if(work && radioC.heldOps() && !wheelInPlace){++radioWorkKept;} // (v652: nothing held reads guest memory in place)
    else if(radioC.beforeResolve())wheelExecuteHeld(work?"work":"resolve",nullptr,true); // v651 (the two never hold at once)
    if(wheelC.beforeResolve())wheelExecuteHeld(work?"work":"resolve");
}
[[gnu::noinline]] void wheelPresent() noexcept {
    const auto e=wheelC.present();
    const auto re=radioC.present(); // v651
    const auto ce=charC.present(); // thor-char
    if(re.flush)wheelExecuteHeld("present",nullptr,true);
    if(e.flush)wheelExecuteHeld("present");
    ++wheelPresents;wheelHueRead();mapProbe();phonePresent(); // (v815)
    try { // v818: the help box of the frame that ended (copied while textFrameOn)
        if(textFrameOn && textWanted() && textList.n && !textBroken)wheelReplay(false,false,true);
        else if(textFrameOn && textBroken)++textBrokenFrames;
    } catch(...) {++wheelStats.exceptions;}
    textList.n=0;textBytes=0;textSeen.clear();textBroken=false;textFrameOn=textHelp; // (the next frame copies while the game shows its help box)
    try {
        if(e.replay && wheelWanted() && wheelList.n){++wheelStats.frames;wheelReplay();}
        else if(re.replay && radioWanted() && wheelList.n){++radioStats.frames;wheelReplay(true);} // v651: the radio wheel
        if(ce.replay && charWanted() && charList.n && !e.replay && !re.replay)wheelReplay(false,true); // thor-char (one wheel per frame on the lower screen)
        else if(ce.replay)++charBroken;
    } catch(...) {++wheelStats.exceptions;}
    wheelList.n=0;wheelBytes=0;wheelSeen.clear();wheelInPlace=false; // (wheel-dedupe; v652: wheelInPlace)
    charList.n=0;charBytes=0;charSeen.clear(); // (thor-char)
    const auto& s=wheelStats;
    if(e.replay && s.frames && !(s.frames%128)) {
        const double n=double(std::max<std::uint64_t>(1,s.replays+s.broken));
        GLOG("RENDER_WHEEL replays=%llu broken=%llu refused=%llu first_refusal=%s skipped_ops=%llu held_ops=%llu flushed_ops=%llu flushes=%llu draws=%llu "
             "cpu_us=%.1f cpu_us_max=%.1f gpu_us=%.1f copied_kb=%.0f in_place_kb=%.0f budget=%llu unexpected=%llu exceptions=%llu k=%d off=%d copy_us=%.1f flush_us_total=%.0f loose_frames=%llu loose_ops=%llu",
             (unsigned long long)s.replays,(unsigned long long)s.broken,(unsigned long long)s.refused,s.firstRefusal?s.firstRefusal:"-",
             (unsigned long long)s.skippedOps,(unsigned long long)s.heldOps,(unsigned long long)s.flushedOps,(unsigned long long)s.flushes,(unsigned long long)s.draws,
             double(s.cpuNs)/1e3/n,double(s.cpuNsMax)/1e3,s.gpuN?double(s.gpuNs)/1e3/double(s.gpuN):0.0,double(s.copiedKb)/n,double(s.inPlaceKb)/n,
             (unsigned long long)s.budget,(unsigned long long)s.unexpected,(unsigned long long)s.exceptions,wheelK,int(wheelOff),double(s.copyNs)/1e3/n,double(s.flushNs)/1e3,(unsigned long long)wheelC.looseFrames,(unsigned long long)wheelC.looseOps);
        wheelStats.cpuNsMax=0;
    }
}
// A scissor word triple for a guest rectangle (no window offset: hud4Op adds the overlay's).
static std::array<std::uint32_t,3> wheelScissor(const hudrep::Rect& r) noexcept {
    return {0u,std::uint32_t(r.x0)|(std::uint32_t(r.y0)<<16),std::uint32_t(r.x1)|(std::uint32_t(r.y1)<<16)};
}
static float wheelHalf(std::uint16_t h) noexcept {
    const std::uint32_t sgn=(h>>15)&1u,exp=(h>>10)&31u,man=h&1023u;
    float v=exp==0?float(man)*(1.0f/16777216.0f):exp==31?0.0f:std::ldexp(float(man|1024u),int(exp)-25);
    return sgn?-v:v;
}
// The main frame (this replay's main colour surface) -> 16x9 filtered texels -> host buffer (read wheelHueLatency presents later).
void wheelHueRecord(const Hud4Keys& mk) noexcept {
    if(wheelHuePending)return;
    try {
        const auto it=targets.find({mk.colorBase,mk.colorFormat,mk.pitch,false});
        if(it==targets.end() || !it->second.image.image || it->second.image.format!=VK_FORMAT_R16G16B16A16_SFLOAT)return;
        const Image& src=it->second.image;
        if(!wheelHueImage.image) {
            const VkComponentMapping id{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
            if(!makeImage(WheelHueW,WheelHueH,1,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT,false,id,wheelHueImage))
                {wheelHueImage={};return;}
            endPass(__LINE__);toGeneral(wheelHueImage);
        }
        if(!wheelHueBuf.buffer && !makeBuffer(VkDeviceSize(WheelHueW)*WheelHueH*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT,wheelHueBuf)){wheelHueBuf={};return;}
        const auto sw=std::int32_t(rscale::extent(1280,src.k)),sh=std::int32_t(rscale::extent(720,src.k));
        if(sw<=0 || sh<=0 || std::uint32_t(sw)>rsHostW(src) || std::uint32_t(sh)>rsHostH(src))return;
        endPass(__LINE__);hazards({src.image},{wheelHueImage.image});barrier();
        VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        b.srcOffsets[1]={sw,sh,1};b.dstOffsets[1]={std::int32_t(WheelHueW),std::int32_t(WheelHueH),1};
        vkCmdBlitImage(cmd,src.image,VK_IMAGE_LAYOUT_GENERAL,wheelHueImage.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_LINEAR);
        barrier();
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={WheelHueW,WheelHueH,1};
        vkCmdCopyImageToBuffer(cmd,wheelHueImage.image,VK_IMAGE_LAYOUT_GENERAL,wheelHueBuf.buffer,1,&c);
        barrier();
        wheelHuePending=true;wheelHueAt=wheelPresents;
    } catch(...) {++wheelStats.exceptions;}
}
// The read-back, once its command buffer has surely finished: the mean colour, brought to wheelrep::Background's luminance.
void wheelHueRead() noexcept {
    if(!wheelHuePending || wheelPresents<wheelHueAt+WheelHueLatency || !wheelHueBuf.mapped)return;
    wheelHuePending=false;
    const auto* p=static_cast<const std::uint16_t*>(wheelHueBuf.mapped);
    double m[3]{};
    for(unsigned i=0;i<WheelHueW*WheelHueH;++i)for(unsigned c=0;c<3;++c)m[c]+=double(std::max(0.0f,wheelHalf(p[4*i+c])));
    for(auto& v:m)v/=double(WheelHueW*WheelHueH);
    const auto bg=wheelrep::hueBackground(float(m[0]),float(m[1]),float(m[2]));
    if(!bg.ok)return;
    for(unsigned c=0;c<3;++c)wheelBg[c]=wheelHueKnown?0.5f*wheelBg[c]+0.5f*bg.rgb[c]:bg.rgb[c];
    wheelHueKnown=true;
    if(!wheelHueLogged){wheelHueLogged=true;
        GLOG("RENDER_WHEEL_HUE first=1 mean=%.4f,%.4f,%.4f background=%.4f,%.4f,%.4f (constant %.3f,%.3f,%.3f)",m[0],m[1],m[2],double(wheelBg[0]),double(wheelBg[1]),
             double(wheelBg[2]),double(wheelrep::Background[0]),double(wheelrep::Background[1]),double(wheelrep::Background[2]));}
}
void mapProbe() noexcept {
    try {
        if(mapProbePending && wheelPresents>=mapProbeAt+WheelHueLatency && mapProbeBuf.mapped) {
            mapProbePending=false;
            const auto* p=static_cast<const std::uint8_t*>(mapProbeBuf.mapped);std::uint64_t h=0xCBF29CE484222325ull;
            for(std::size_t i=0;i<std::size_t(mapProbeW)*mapProbeH*8;++i){h^=p[i];h*=0x100000001B3ull;}
            if(mapProbeLogs<4096){++mapProbeLogs;GLOG("RENDER_HUD4_PROBE serial=%llu hash=%016llx block=%ux%u",(unsigned long long)mapProbeSerial,(unsigned long long)h,mapProbeW,mapProbeH);}
        }
        if(mapProbePending || (wheelPresents%64) || !hud4OverlayColor.image.image || hud4OverlayColor.image.format!=VK_FORMAT_R16G16B16A16_SFLOAT || !hud4Serial)return;
        const auto& im=hud4OverlayColor.image;const std::uint32_t w=std::min(64u,rsHostW(im)),h=std::min(64u,rsHostH(im));
        if(!w || !h)return;
        if(!mapProbeBuf.buffer && !makeBuffer(VkDeviceSize(64)*64*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT,mapProbeBuf)){mapProbeBuf={};return;}
        if(!begin())return;
        endPass(__LINE__);hazards({im.image},{});barrier();
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        c.imageOffset={std::int32_t((rsHostW(im)-w)/2),std::int32_t((rsHostH(im)-h)/2),0};c.imageExtent={w,h,1};
        vkCmdCopyImageToBuffer(cmd,im.image,VK_IMAGE_LAYOUT_GENERAL,mapProbeBuf.buffer,1,&c);
        barrier();
        mapProbePending=true;mapProbeAt=wheelPresents;mapProbeSerial=hud4Serial;mapProbeW=w;mapProbeH=h;
    } catch(...) {++wheelStats.exceptions;}
}
// v651: radio = the radio wheel's replay (RadioCrop, its own targets and scale, no stats panel); else the weapon wheel's (unchanged).
void wheelReplay(bool radio=false,bool character=false,bool text=false) { // (thor-char: character = the character wheel from charList; v818: text = the help box from textList)
    const auto t0=Clock::now();
    if(!hud4Viewer)hud4Init();
    std::uint64_t& brokenCount=text?textBrokenFrames:character?charBroken:radio?radioStats.broken:wheelStats.broken;
    Hud4List& L=text?textList:character?charList:wheelList;
    if(ocq::engine().counting() || !begin()){++brokenCount;return;}
    const auto mk=hud4Keys(L.ops[0].state);
    const auto& area=text?wheelrep::TextArea:character?wheelrep::CharArea:radio?wheelrep::RadioCrop:wheelrep::Area;
    const auto ov=hudrep::overlayFor(area);
    int textK=wheelrep::TextK;
    int& kk=text?textK:character?charK:radio?radioK:wheelK;
    if(!kk){const auto e=recomp::gpu::hud2::screen().extentHint();
        kk=character?CharK:radio?wheelrep::scaleFor(e.width,e.height,wheelrep::RadioCrop.y1-wheelrep::RadioCrop.y0):wheelrep::scaleFor(e.width,e.height);}
    const int k=kk;
    Target& oc=text?textColor:character?charColor:radio?radioColor:wheelColor;Target& od=text?textDepth:character?charDepth:radio?radioDepth:wheelDepth;
    const VkFormat ocf=hud4RealFormat(mk.colorBase,mk.colorFormat,mk.pitch,false,colorFormat(mk.colorFormat));
    const VkFormat odf=hud4RealFormat(mk.depthBase,mk.depthFormat,mk.pitch,true,depthFormat);
    if(!hud4Shadow(oc,ov.w,ov.h,k,ocf,false,mk.colorBase,mk.colorFormat,mk.pitch,true) ||
       !hud4Shadow(od,ov.w,ov.h,k,odf,true,mk.depthBase,mk.depthFormat,mk.pitch,false)){++brokenCount;return;}
    if(!wheelPool){VkQueryPoolCreateInfo q{};q.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=8;
        if(vkCreateQueryPool(d.device,&q,nullptr,&wheelPool)!=VK_SUCCESS)wheelPool=VK_NULL_HANDLE;}
    if(wheelPool)for(unsigned s=0;s<4;++s)if(wheelPoolPending[s]) {
        std::uint64_t t[2]{};
        if(vkGetQueryPoolResults(d.device,wheelPool,2*s,2,sizeof t,t,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            wheelStats.gpuNs+=std::uint64_t(double(((t[1]&timestampMask)-(t[0]&timestampMask))&timestampMask)*timestampPeriod);++wheelStats.gpuN;wheelPoolPending[s]=false;
        }
    }
    const unsigned slot=wheelPoolSlot++%4;const bool timed=wheelPool && !wheelPoolPending[slot] && timestampPeriod>0;
    endPass(__LINE__);
    if(timed){vkCmdResetQueryPool(cmd,wheelPool,2*slot,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,wheelPool,2*slot);}
    if(!(wheelHueTick++%WheelHueEvery))wheelHueRecord(mk);
    hud4SavedViewer=viewer;viewer=hud4Viewer;hud4K=k;hud4Unexpected=false;
    const auto refusals=hud4Stats.refused,hdraws=hud4Stats.draws,hclears=hud4Stats.clears;const char* const firstRefusal=hud4Stats.firstRefusal;
    bool ok=true;
    const auto restore=[&]{hud4RedirectOff();viewer=hud4SavedViewer;asyncSpans=nullptr;};
    try {
        hud4Redirect(mk,oc,od,ov.w,ov.h);
        {Hud4Op blank;blank.clear=true;blank.state=L.ops[0].state;blank.clearIn.flags=0x31;blank.clearIn.depth=1.0f;blank.clearIn.stencil=0;
         blank.clearIn.color={wheelBg[0],wheelBg[1],wheelBg[2],wheelBg[3]}; // (v624: the game's graded mean, dark)
         if(text)blank.clearIn.color={0.0f,0.0f,0.0f,0.0f}; // (v818: transparent - only the help box's own pixels over the map)
         blank.rect={-ov.ox,-ov.oy,std::int32_t(ov.w)-ov.ox,std::int32_t(ov.h)-ov.oy};
         const auto put=[&](unsigned at,std::uint32_t v){auto* p=blank.state.data()+at;p[0]=std::uint8_t(v>>24);p[1]=std::uint8_t(v>>16);p[2]=std::uint8_t(v>>8);p[3]=std::uint8_t(v);};
         put(12816,1);put(12832,1); // (the clear's colour / depth enables on the copy, as hud4's blank)
         ok=hud4Op(blank,ov.ox,ov.oy);}
        if(text)hud14Cover=true; // (v818: alpha = coverage, as the minimap's box layer)
        for(unsigned i=0;i<L.n && ok;++i) {
            auto& op=L.ops[i];
            if(text || (!op.clear && !op.scissor))op.scissor=wheelScissor(area); // (no scissor words: the replay's area; thor-char: clears keep their own; v818: the text area always)
            ok=hud4Op(op,ov.ox,ov.oy);
        }
        hud14Cover=false;
        restore();
    } catch(...) {hud14Cover=false;restore();++wheelStats.exceptions;ok=false;}
    if(timed){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,wheelPool,2*slot+1);wheelPoolPending[slot]=true;}
    if(hud4Scratch.image.image){dropFramebuffers(hud4Scratch.image.view);destroyLater(hud4Scratch.image);hud4Scratch=Target{};}
    // (hud4Op counts its draws and refusals in hud4Stats: moved to this module's statistics)
    wheelStats.draws+=hud4Stats.draws-hdraws;hud4Stats.draws=hdraws;hud4Stats.clears=hclears;
    if(hud4Stats.refused!=refusals){wheelStats.refused+=hud4Stats.refused-refusals;if(!wheelStats.firstRefusal)wheelStats.firstRefusal=hud4Stats.firstRefusal;
        hud4Stats.refused=refusals;hud4Stats.firstRefusal=firstRefusal;ok=false;}
    if(hud4Unexpected){++wheelStats.unexpected;ok=false;}
    const auto ns=nsSince(t0);wheelStats.cpuNs+=ns;wheelStats.cpuNsMax=std::max(wheelStats.cpuNsMax,ns);
    if(!ok) {
        ++brokenCount;
        if(++wheelFails>=WheelFailLimit){wheelOff=true;GLOG("RENDER_WHEEL off=1 why=%u-failed-replays first_refusal=%s",wheelFails,wheelStats.firstRefusal?wheelStats.firstRefusal:"-");}
        return;
    }
    wheelFails=0;
    if(text) { // v818: published to the lower screen's text channel (not a wheel)
        ++textReplays;
        const auto b=hudrep::hostBox(wheelrep::TextArea,ov,k);
        recomp::gpu::hud2::TextImg t;t.image=oc.image.image;t.view=oc.image.view;t.format=oc.image.format;
        t.width=rsHostW(oc.image);t.height=rsHostH(oc.image);t.box[0]=float(b.x0);t.box[1]=float(b.y0);t.box[2]=float(b.x1);t.box[3]=float(b.y1);t.serial=++wheelSerial;
        recomp::gpu::hud2::screen().publishText(t);
        if(!textLogged){textLogged=true;GLOG("RENDER_TEXT first=1 k=%d overlay=%ux%u box=%d,%d..%d,%d ops=%u",k,ov.w,ov.h,b.x0,b.y0,b.x1,b.y1,L.n);}
        return;
    }
    if(character)++charReplays;else ++(radio?radioStats.replays:wheelStats.replays);
    const auto ring=hudrep::hostBox(character?wheelrep::CharWheelCrop:radio?wheelrep::RadioCrop:wheelrep::RingCrop,ov,k),
               stats=hudrep::hostBox(character?wheelrep::CharStatsCrop:wheelrep::StatsCrop,ov,k);
    recomp::gpu::hud2::Wheel w;
    w.image=oc.image.image;w.view=oc.image.view;w.format=oc.image.format;
    w.width=rsHostW(oc.image);w.height=rsHostH(oc.image);
    w.ring[0]=float(ring.x0);w.ring[1]=float(ring.y0);w.ring[2]=float(ring.x1);w.ring[3]=float(ring.y1);
    if(!radio){w.stats[0]=float(stats.x0);w.stats[1]=float(stats.y0);w.stats[2]=float(stats.x1);w.stats[3]=float(stats.y1);} // (v651: the radio: none)
    w.k=k;w.serial=++wheelSerial;w.radio=radio;w.character=character;
    recomp::gpu::hud2::screen().publishWheel(w);
    if(character) { // thor-char
        if(!charLogged){charLogged=true;
            GLOG("RENDER_CHAR first=1 k=%d overlay=%ux%u offset=%d,%d wheel=%d,%d..%d,%d stats=%d,%d..%d,%d ops=%u",k,ov.w,ov.h,ov.ox,ov.oy,ring.x0,ring.y0,ring.x1,ring.y1,
                 stats.x0,stats.y0,stats.x1,stats.y1,L.n);}
        if(!(charReplays%64))GLOG("RENDER_CHAR replays=%llu broken=%llu frames=%llu ops=%llu copies=%llu copy_us=%.1f cpu_us_last=%.1f refused=%llu first_refusal=%s",
            (unsigned long long)charReplays,(unsigned long long)charBroken,(unsigned long long)charC.frames,(unsigned long long)charC.ops,(unsigned long long)charC.copies,
            double(charCopyNs)/1e3/double(charC.frames?charC.frames:1),double(ns)/1e3,(unsigned long long)wheelStats.refused,wheelStats.firstRefusal?wheelStats.firstRefusal:"-");
        return;
    }
    if(radio) {
        if(!radioLogged){radioLogged=true;
            GLOG("RENDER_RADIO first=1 k=%d overlay=%ux%u offset=%d,%d ring=%d,%d..%d,%d format=%d/%d layer_ops=%u",k,ov.w,ov.h,ov.ox,ov.oy,ring.x0,ring.y0,ring.x1,ring.y1,
                 int(ocf),int(odf),wheelList.n);}
        const auto& r=radioStats;
        if(!(r.replays%64))GLOG("RENDER_RADIO replays=%llu broken=%llu frames=%llu skipped_ops=%llu held_ops=%llu flushes=%llu flushed_ops=%llu flushed_logos=%llu cpu_us_last=%.1f "
            "work_kept=%llu copy_us=%.1f copied_kb=%.0f refused=%llu first_refusal=%s",
            (unsigned long long)r.replays,(unsigned long long)r.broken,(unsigned long long)r.frames,(unsigned long long)r.skippedOps,(unsigned long long)r.heldOps,
            (unsigned long long)r.flushes,(unsigned long long)r.flushedOps,(unsigned long long)radioC.flushedLogos,double(ns)/1e3,
            (unsigned long long)radioWorkKept,double(radioCopyNs)/1e3/double(r.frames?r.frames:1),double(radioCopiedKb)/double(r.frames?r.frames:1),
            (unsigned long long)wheelStats.refused,wheelStats.firstRefusal?wheelStats.firstRefusal:"-");
        return;
    }
    if(!wheelLogged){wheelLogged=true;
        GLOG("RENDER_WHEEL first=1 k=%d overlay=%ux%u offset=%d,%d ring=%d,%d..%d,%d stats=%d,%d..%d,%d format=%d/%d layer_ops=%u",
             k,ov.w,ov.h,ov.ox,ov.oy,ring.x0,ring.y0,ring.x1,ring.y1,stats.x0,stats.y0,stats.x1,stats.y1,int(ocf),int(odf),wheelList.n);}
}
