    // ---- framegen-1b (frame generation, stage 1b: the generator; fg_gen.h): the renderer's part. Included INSIDE class
    // Renderer as its last members (after fg_burst_renderer.inc), so the members above keep their offsets. Our own code.
    // What the renderer gives the presenter's generator for every real frame while files/private-trace/diag-fg-between exists:
    // the present image's view, the frame's camera (the tonemap draw's pixel c8-c15 and c130) and the view of the frame's
    // scene depth (a depth target of pitch 1280 that this frame drew into: the main depth target, FEASIBILITY.md 1.2; the
    // rule is at fgGenPresent). Nothing is recorded into the frame's commands, no game object is changed, no view is created.
    // The hooks ride on lane fg-burst's (fg_burst_renderer.inc: three one-line additions there), so that draw() and present()
    // are not touched at all:
    //   fgGenWindow()   from fgbWindow(), the 60-present switch poll: the switch files, the tuning files, the presenter's word
    //   fgGenTonemap()  from fgbTonemap(), i.e. inside the block of the renderer's own RENDER_POSTFX log line. As the capture
    //                   does while armed, the generator makes that block run for the tonemap draw of EVERY frame by rewinding
    //                   postfxLogged[0] at every present: while diag-fg-between exists the renderer logs one
    //                   "RENDER_POSTFX name=tonemap" line per frame (about 390 characters). That is the price of not adding an
    //                   instruction to draw(); the one piece of renderer state written is postfxLogged[0]
    //   fgGenPresent()  from fgbPresent(), after the blit into the present image: this frame's inputs to the presenter
    // Switch files (files/private-trace/, polled at the 60-present window):
    //   diag-fg-between    the generator, in-between order (what the app's Settings row writes under the 30 fps cap)
    //   diag-fg-show       refused blocks red, blocks that did not move blue
    //   diag-fg-audit      one made frame per window is audited (its block classes counted: RENDER_DIRECT_FG_GEN audit=)
    //   diag-fg-fail       text: the refusal threshold, 4 .. 32 (default 10)
    //   diag-fg-late-max   text: the slot keeper's limit in percent, 5 .. 95 (default 40)
    //   diag-fg-gpu-max    text: the slot keeper's GPU-time limit in microseconds, 2000 .. 6000 (default 3000; fg-gpu-max, v639)
    // fg-scaled (2026-10-10; fgGenPresent only, every edit is marked fg-scaled): with a render scale applied
    // (files/settings/render-scale.txt, render_scale.h) the frame's picture and its scene depth are handed too, when they
    // are the SAME frame at the SAME scale: the present image has the scaled frame's size (stage B hands the frame over at
    // its render size) and the depth target is drawn at the run's scale. At 100 % both tests are v513's.
    bool fgGenOn=false;                 // the cached flag every hook tests first
    bool fgGenSeen=false;               // diag-fg-between was seen in this run (the window line is logged from then on; check c2)
    unsigned fgGenFile=0,fgGenAuditToken=0,fgGenDepthMoves=0;
    recomp::fgg::Camera fgGenCamera;std::uint64_t fgGenCameraInterval=0,fgGenCameraFrame=0;unsigned fgGenCameraDraws=0;
    std::uint64_t fgGenTonemapBase=0;   // postfxDraws[0] (the renderer's count of tonemap draws) at the previous present
    std::array<std::uint32_t,48> fgGenVariantFrames{}; // fixer2: frames whose camera came from tonemap postfx#(20 + k), since the last window line
    std::uint64_t fgGenDepthKey=~0ull;  // the depth target that has the role of the scene depth (fgGenDepthOf; ~0: none yet)
    std::uint64_t fgGenDepthClock=0;    // the renderer's write clock at the previous present with the generator on

    static bool fgGenHas(const char* name) noexcept {
        char path[160];std::snprintf(path,sizeof path,RECOMP_APP_PATH("/private-trace/%s"),name);
        return access(path,F_OK)==0;
    }
    // A tuning file's text (null when absent or unreadable).
    static const char* fgGenText(const char* name,char* out,std::size_t cap) noexcept {
        char path[160];std::snprintf(path,sizeof path,RECOMP_APP_PATH("/private-trace/%s"),name);
        FILE* f=std::fopen(path,"r");
        if(!f)return nullptr;
        const std::size_t n=std::fread(out,1,cap-1,f);std::fclose(f);out[n]=0;
        return out;
    }
    void fgGenEveryFrame() noexcept {postfxLogged[0]=std::uint64_t{0}-300;} // (frame >= postfxLogged[0] + 300 holds for any frame: the tonemap block runs)
    // ---- hook: the 60-present switch poll (after fgWindow(), which has just written stage 1a's bits of the presenter's word)
    [[gnu::noinline]] void fgGenWindow() noexcept {
        namespace fgg=recomp::fgg;
        // (a path that ends with the switch's name: the app's Settings screen looks for exactly this string in the library)
        const bool between=access(RECOMP_APP_PATH("/private-trace/diag-fg-between"),F_OK)==0 &&
                           access(RECOMP_APP_PATH("/private-trace/diag-measure-unpaced"),F_OK)!=0; // v558 (measurement only): unpaced = no generation either
        const bool show=between && fgGenHas("diag-fg-show");
        const unsigned window=fgGenFile;
        fgGenFile=(between?fgg::SwGenBetween:0u)|(show?fgg::SwShow:0u);
        auto& presenter=recomp::gpu::direct::presenter();
        // (absent now and at the last poll: the presenter's word is stage 1a's own, untouched)
        if(fgGenFile || window)presenter.fgSwitches(fgg::switchWord(presenter.fgSwitchWord(),between,show));
        if(between) {
            char text[32];
            const float fail=fgg::tuned(fgGenText("diag-fg-fail",text,sizeof text),fgg::K::FailLo,fgg::K::FailHi,fgg::K::Fail);
            const float late=fgg::tuned(fgGenText("diag-fg-late-max",text,sizeof text),float(fgg::K::LateLo),float(fgg::K::LateHi),float(fgg::K::LateMaxPct));
            if(fgGenHas("diag-fg-audit"))++fgGenAuditToken;
#if defined(RECOMP_THOR_EDITION) // Thor RC1 (v806): the Thor's GPU-time limit without the file is 3500 us (the owner's setting since v639's
            constexpr float GpuMaxDefault=3500.0f;  // device runs; files/private-trace/diag-fg-gpu-max still overrides it within 2000 .. 6000)
#else
            constexpr float GpuMaxDefault=float(fgg::K::GpuMaxUs);
#endif
            const float gpu=fgg::tuned(fgGenText("diag-fg-gpu-max",text,sizeof text),float(fgg::K::GpuMaxLo),float(fgg::K::GpuMaxHi),GpuMaxDefault); // fg-gpu-max
            presenter.fgGenTune(fail,unsigned(late),fgGenAuditToken,gpu);
            if(!fgGenOn){fgGenEveryFrame();fgGenDepthKey=~0ull;fgGenDepthClock=writeClock;fgGenDepthMoves=0;fgGenTonemapBase=postfxDraws[0];}
            fgGenSeen=true;
        }
        fgGenOn=between;
        // (check c2: no line while the file has never been there, as lane fg-burst's own poll: a run without the switch logs
        // what v465 logs. From the first poll that saw it: one line per window, also after it is removed.)
        if(!fgGenSeen)return;
        const unsigned w=window&fgGenFile;
        GLOG("RENDER_GENERIC_V210Z fg_between=%u fg_show=%u",unsigned((w&fgg::SwGenBetween)!=0),unsigned((w&fgg::SwShow)!=0));
        { // fixer2: which tonemap variants gave this window's cameras (only when another than postfx#20p did; "one" = diag-fg-tonemap-one)
            std::string v;bool other=false;
            for(unsigned k=0;k<fgGenVariantFrames.size();++k)if(fgGenVariantFrames[k]){char b[24];std::snprintf(b,sizeof b,"%s%u:%u",v.empty()?"":",",20+k,fgGenVariantFrames[k]);v+=b;other=other || k;}
            if(other)GLOG("RENDER_DIAG_FG_TONEMAP variants=%s%s",v.c_str(),post::tonemapOneOnly.load(std::memory_order_relaxed)?" one":"");
            fgGenVariantFrames={};
        }
    }
    static std::uint64_t fgGenDepthOf(const Target& t) noexcept {
        return (std::uint64_t(t.base)<<40)|(std::uint64_t(t.xenosFormat)<<32)|(std::uint64_t(t.image.width)<<16)|t.image.height;
    }
    // ---- hook: draw(), where the renderer logs RENDER_POSTFX for a postfx shader's draw: the tonemap draw's camera constants
    // (the frame's FIRST tonemap draw; how many the frame had is the renderer's own count, taken at the present).
    [[gnu::noinline]] void fgGenTonemap(std::uint64_t psHash,const DrawInput& in) noexcept {
        // fixer2 (2026-10-22): any member of the tonemap family (post_fidelity.h TonemapVariants), not postfx#20p alone
        if(!fgGenOn || post::postfxShader(psHash)!=&post::PostfxShaders[0] || !in.pixelConstants)return;
        if(fgGenCameraInterval!=presentCount){fgGenCameraInterval=presentCount;fgGenCameraDraws=0;}
        if(!fgGenCameraDraws++) {
            recomp::fgg::takeCamera(fgGenCamera,*in.pixelConstants);fgGenCameraFrame=frame;
            const unsigned v=post::tonemapVariant(psHash);if(v>=20 && v<20+fgGenVariantFrames.size())++fgGenVariantFrames[v-20]; // (the window line)
            ffTonemapNow=std::uint16_t(v); // (fixer2: the frame facts)
        }
    }
    // ---- hook: present(), after the blit into the present image: this frame's inputs for the presenter's generator.
    [[gnu::noinline]] void fgGenPresent(const Image& presentImage) noexcept {
        namespace fgg=recomp::fgg;
        fgg::FrameIn in;in.tag=presentTag;in.frame=fgGenCameraFrame;
        // the picture: the game's 1280x720 UNORM present image (not the sRGB image of diag-gamma-on, the mosaic, a census frame)
        // fg-scaled: ... or, with a render scale, the present image that IS the scaled frame (frameW x frameH: 960x540 at
        // 75 %; stage B's present image of a frame that goes to the presenter on the GPU). A scaled run's 1280x720 present
        // images (stage A's blit: diag-rs-stage-a; a frame that is read back) are not handed: their pixels are not the
        // scaled depth's texels. At 100 % frameW x frameH is 1280x720 and the test is v513's.
        const unsigned frameW=rscale::extent(fgg::Width,rsK),frameH=rscale::extent(fgg::Height,rsK);
        const bool picture=presentImage.view && fgg::pictureIsFrame(presentImage.width,presentImage.height,frameW,frameH) &&
            presentImage.format==VK_FORMAT_R8G8B8A8_UNORM && !v369Mosaic && !census.active;
        if(picture){in.sourceView=reinterpret_cast<std::uint64_t>(presentImage.view);in.sourceW=std::uint16_t(frameW);in.sourceH=std::uint16_t(frameH);}
        // the camera: taken while this frame was rendered (presentCount was advanced at the top of this present), and only
        // from a frame with exactly one tonemap draw (the renderer's own count; the hook sees a frame's first draw only)
        in.tonemapDraws=std::uint32_t(postfxDraws[0]-fgGenTonemapBase);fgGenTonemapBase=postfxDraws[0];
        const bool taken=fgGenCameraDraws && fgGenCameraInterval+1==presentCount;
        if(taken)in.cam=fgGenCamera;
        in.why=fgg::cameraVerdict(in.tonemapDraws,taken,fgGenCamera);
        // the scene depth, read as it is. Nothing here can see what a target HOLDS; what can be checked is checked (check c2):
        //   candidates  depth targets of pitch 1280, not scaled, that THIS frame drew into (a write stamp newer than the
        //               previous present's: a target the frame did not touch holds another frame's depth and is never handed);
        //               (fg-scaled: "not scaled" is now "at the run's scale": with a render scale every pitch-1280 target is
        //               drawn at rsK (render_scale.h targetK), so its texel (x, y) is the scaled frame's pixel (x, y);
        //               a target at any other scale is never handed. The pitch stays the guest's 1280 at every scale.)
        //   the role    the candidate that had it at the previous present keeps it, so a later pass of the frame that draws
        //               into another pitch-1280 depth target (a phone, a 3D menu) does not take it away; otherwise the newest.
        // No candidate: no depth is handed and the frame gets no made picture (nogen=no-depth). Every change of the role is
        // logged with the number of candidates and of changes since the switch came on (DEVICE-PLAN.md G1: one line, 0:17:1280).
        const Target* best=nullptr;const Target* same=nullptr;unsigned candidates=0;
        for(const auto& [key,t]:targets) {
            if(!t.depth || t.pitch!=fgg::Width || !t.image.image || !t.image.view || t.image.k!=rsK || t.written<=fgGenDepthClock)continue; // (fg-scaled: rsK; was rscale::One)
            ++candidates;
            if(fgGenDepthOf(t)==fgGenDepthKey)same=&t;
            if(!best || t.written>best->written)best=&t;
        }
        if(same)best=same;
        fgGenDepthClock=writeClock;
        if(best) {
            // (fg-scaled: the depth image's size in ITS texels: Image::width / height are guest pixels, the Vulkan image is their scaled extent; the same numbers at 100 %)
            in.depthView=reinterpret_cast<std::uint64_t>(best->image.view);in.depthW=rscale::extent(best->image.width,best->image.k);in.depthH=rscale::extent(best->image.height,best->image.k);
            if(const std::uint64_t key=fgGenDepthOf(*best);key!=fgGenDepthKey) { // (once, and whenever another target takes the role)
                if(fgGenDepthKey!=~0ull)++fgGenDepthMoves;
                fgGenDepthKey=key;
                // (fg-scaled: size= is the depth image's own size, in.depthW x in.depthH: 960x576 at 75 %; at 100 % the numbers it always printed)
                GLOG("RENDER_DIAG_FG_GEN depth=%u:%u:%u size=%ux%u vkformat=%d pending_clears=%zu residue=%u tonemap_draws=%u camera=%s candidates=%u moves=%u",best->base,best->xenosFormat,best->pitch,
                     in.depthW,in.depthH,int(best->image.format),best->pendingClears.size(),unsigned(best->residuePending),in.tonemapDraws,fgg::text(in.why),candidates,fgGenDepthMoves);
            }
        }
        recomp::gpu::direct::presenter().fgGenFrame(in);
        fgGenCameraDraws=0;
        fgGenEveryFrame(); // the next frame's tonemap draw reaches fgGenTonemap()
    }
