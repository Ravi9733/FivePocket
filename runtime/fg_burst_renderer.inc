    // ---- fg-burst (frame generation, lane B, stage 1a; DESIGN-stage1.md section 11): the Vulkan part of the capture "fg-burst"
    // (fg_burst.h: the switch files, the ring, the part format, the frame record). Included INSIDE class Renderer as its last
    // members, so the members above keep v463's offsets. Our own code.
    // Four hooks, each an unconditional call to an out-of-line noexcept function, so their callers keep their control flow (PGO
    // records). The three per-frame hooks (fgbTonemap, fgbStencil, fgbPresent) are thin: they test fgbOn (false unless
    // files/private-trace/diag-fg-burst armed the capture) and return, or tail-call the out-of-line body (fgb...Armed); no
    // stack frame, no register save while the capture is off:
    //   fgbWindow()   the 60-present switch poll (present()), after fgWindow(): the two files, the state machine
    //   fgbTonemap()  draw(), inside the block of the renderer's own RENDER_POSTFX log line (a postfx shader's first draw, then
    //                 once per 300 frames; draw()'s per-draw path has no new instruction): the tonemap draw's pixel c8-c15
    //                 and c130. While armed the capture rewinds postfxLogged[0] at every present, so that block runs for the
    //                 tonemap draw of EVERY frame: the renderer's own RENDER_POSTFX name=tonemap line is logged per frame
    //   fgbStencil()  copyDepth(), after its endPass: at the frame's first depth resolve from a depth target of pitch 1280, the
    //                 target's stencil (rows 0-719) through the renderer's own depth-word pass into the ring's word picture
    //   fgbPresent()  present(), after the blit into the present image: the present image (one vkCmdCopyImage) and the main
    //                 depth target (the renderer's own plain depth-resolve pass: float32, not rounded) into the ring;
    //                 during a read-out: one ring frame per present into a host-visible buffer, written to part files once
    //                 that frame's fence has signalled (a slot-garbage entry: waitSlot() is untouched)
    // While armed: 24 images of 1280x720 (8 x RGBA8 colour, 8 x R32F depth, 8 x RGBA8 depth word) in GENERAL layout and one
    // descriptor pool of 16 sets, all OUTSIDE the renderer's resource accounting (they do not count for its eviction limits or
    // its resource log) and released through the frame garbage lists. Per frame: 1 image copy, 2 compute dispatches of 160 x 90
    // groups, 2 descriptor-set writes, a walk of the target map. A read-out adds one buffer of 3 x 3.7 MB per frame slot.
    // Nothing here changes a game object: no pending clear is applied, no target view is created, no pool of the renderer is
    // used. The one piece of renderer state it writes is postfxLogged[0], the frame of the last RENDER_POSTFX name=tonemap line.
    struct FgbSet {Image color,depth,word;VkDescriptorSet depthSet=VK_NULL_HANDLE,wordSet=VK_NULL_HANDLE;recomp::fgb::Meta meta;};
    struct FgbState {
        recomp::fgb::Control control;recomp::fgb::Ring ring;
        std::array<FgbSet,recomp::fgb::Frames> sets;
        VkDescriptorPool pool=VK_NULL_HANDLE;
        std::array<Buffer,FrameSlots> staging;std::array<bool,FrameSlots> stagingCoherent{};
        // the frame being rendered (the interval between two presents; presentCount names it)
        recomp::fgb::Camera camera;std::uint64_t cameraInterval=0;
        bool stencilSeen=false,stencilOk=false;std::uint64_t stencilInterval=0;VkImage stencilImage=VK_NULL_HANDLE;
        unsigned stencilBase=0,stencilFormat=0,stencilPitch=0;std::uint32_t stencilAtDraw=0,stencilAtResolve=0;
        std::uint64_t drawsBase=0,resolvesBase=0;    // counters.draws / .resolves at the previous present
        std::uint64_t tonemapBase=0;                 // postfxDraws[0] (the renderer's count of tonemap draws) at the previous present
        // the read-out
        std::uint16_t capture=0;unsigned held=0,drainNext=0,drainDone=0,serial=0;std::uint64_t triggerPresent=0;
        recomp::fgb::DirSink sink;std::vector<std::uint8_t> stencilBytes;
        std::uint64_t stored=0,unusable=0,gpuBytes=0; // since armed
    };
    // The renderer's own two members: a flag and a pointer (the state is made when diag-fg-burst is first seen and kept for the
    // run, about 5 KB), so the constructor and the destructor of Renderer keep their control flow too.
    bool fgbOn=false;          // the cached flag every hook tests first
    recomp::fgb::Launch fgbLaunch; // (one byte) a diag-fg-burst left by an earlier run does not arm
    FgbState* fgbP=nullptr;
    static constexpr const char* FgbTrace=RECOMP_APP_PATH("/private-trace");
    static constexpr const char* FgbBase=RECOMP_APP_PATH("/private-trace/fgburst");
    static constexpr VkDeviceSize FgbStagingBytes=recomp::fgb::ColorBytes+recomp::fgb::DepthBytes+recomp::fgb::WordBytes;
    static constexpr std::uint64_t FgbDiskBytes=128ull<<20; // free space asked for before a read-out (it writes 66.4 MB)

    // The two switch files.
    [[gnu::noinline]] void fgbFiles(bool& arm,bool& go) const noexcept {
        arm=access(RECOMP_APP_PATH("/private-trace/diag-fg-burst"),F_OK)==0;
        go=arm && access(RECOMP_APP_PATH("/private-trace/diag-fg-burst-go"),F_OK)==0;
    }
    // An image of the ring: 1280x720, one level, GENERAL layout once fgbAllocate() has transitioned it. Created and destroyed
    // with plain Vulkan calls: a failure does not reach the renderer's allocation-failure path (which evicts textures).
    bool fgbImage(VkFormat format,VkImageUsageFlags usage,Image& out) noexcept {
        out={};
        FgbState& fgb=*fgbP;
        VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.imageType=VK_IMAGE_TYPE_2D;i.format=format;
        i.extent={recomp::fgb::Width,recomp::fgb::Height,1};i.mipLevels=1;i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;
        i.usage=usage;i.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(d.device,&i,nullptr,&out.image)!=VK_SUCCESS){out={};return false;}
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(d.device,out.image,&r);
        const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=type?*type:0u;
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=out.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;
        v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; // (identity components: also the storage view)
        if(!type || vkAllocateMemory(d.device,&a,nullptr,&out.memory)!=VK_SUCCESS){out.memory=VK_NULL_HANDLE;fgbDestroy(d.device,out.image,VK_NULL_HANDLE,VK_NULL_HANDLE);out={};return false;}
        if(vkBindImageMemory(d.device,out.image,out.memory,0)!=VK_SUCCESS || vkCreateImageView(d.device,&v,nullptr,&out.view)!=VK_SUCCESS)
            {fgbDestroy(d.device,out.image,VK_NULL_HANDLE,out.memory);out={};return false;}
        out.bytes=r.size;out.format=format;out.width=recomp::fgb::Width;out.height=recomp::fgb::Height;out.layers=1;out.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
        fgb.gpuBytes+=r.size;
        return true;
    }
    static void fgbDestroy(VkDevice dev,VkImage image,VkImageView view,VkDeviceMemory memory) noexcept {
        if(view)vkDestroyImageView(dev,view,nullptr);
        if(image)vkDestroyImage(dev,image,nullptr);
        if(memory)vkFreeMemory(dev,memory,nullptr);
    }
    // Once the frame recording now has completed (its commands may name the image).
    void fgbDrop(Image& i) {
        if(i.image || i.view || i.memory)garbage.push_back([dev=d.device,im=i.image,v=i.view,m=i.memory]{fgbDestroy(dev,im,v,m);});
        i={};
    }
    // A read-out buffer (host visible, cached when the device has such a type: the CPU reads all of it), as makeReadback().
    bool fgbStaging(Buffer& out,bool& coherent) noexcept {
        out={};
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=FgbStagingBytes;b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(d.device,&b,nullptr,&out.buffer)!=VK_SUCCESS){out={};return false;}
        VkMemoryRequirements r{};vkGetBufferMemoryRequirements(d.device,out.buffer,&r);
        constexpr auto Visible=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,Cached=VK_MEMORY_PROPERTY_HOST_CACHED_BIT,Coherent=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto type=memoryType(r.memoryTypeBits,Visible|Cached|Coherent);coherent=true;
        if(!type){type=memoryType(r.memoryTypeBits,Visible|Cached);coherent=false;}
        if(!type){type=memoryType(r.memoryTypeBits,Visible|Coherent);coherent=true;}
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=type?*type:0u;
        if(!type || vkAllocateMemory(d.device,&a,nullptr,&out.memory)!=VK_SUCCESS){vkDestroyBuffer(d.device,out.buffer,nullptr);out={};return false;}
        if(vkBindBufferMemory(d.device,out.buffer,out.memory,0)!=VK_SUCCESS || vkMapMemory(d.device,out.memory,0,VK_WHOLE_SIZE,0,&out.mapped)!=VK_SUCCESS) {
            vkDestroyBuffer(d.device,out.buffer,nullptr);vkFreeMemory(d.device,out.memory,nullptr);out={};return false;
        }
        out.size=FgbStagingBytes;out.bytes=r.size;return true;
    }
    // Everything the capture holds goes back (after the frame recording now, or the next one, has completed).
    void fgbRelease() noexcept {
        FgbState& fgb=*fgbP;
        recomp::fgb::timesOn.store(false,std::memory_order_relaxed);
        try {
            for(auto& s:fgb.sets){fgbDrop(s.color);fgbDrop(s.depth);fgbDrop(s.word);s=FgbSet{};}
            if(fgb.pool){garbage.push_back([dev=d.device,p=fgb.pool]{vkDestroyDescriptorPool(dev,p,nullptr);});fgb.pool=VK_NULL_HANDLE;}
            for(auto& b:fgb.staging) {
                if(b.buffer)garbage.push_back([dev=d.device,buf=b.buffer,mem=b.memory,mapped=b.mapped]{
                    if(mapped)vkUnmapMemory(dev,mem);vkDestroyBuffer(dev,buf,nullptr);if(mem)vkFreeMemory(dev,mem,nullptr);});
                b={};
            }
        } catch(...) {} // (a garbage list that could not grow: the objects stay until the process ends)
        fgb.ring={};fgb.gpuBytes=0;fgb.held=fgb.drainNext=fgb.drainDone=0;++fgb.serial;
        fgb.stencilSeen=fgb.stencilOk=false;fgb.camera={};
        std::vector<std::uint8_t>().swap(fgb.stencilBytes);
    }
    // A refusal or a failure: one line, everything released, the capture latched until diag-fg-burst was absent at a poll.
    void fgbRefuse(const char* why,const char* stage) noexcept {
        FgbState& fgb=*fgbP;
        GLOG("RENDER_DIAG_FG_BURST refused=%s stage=%s capture=%04x frames_stored=%llu parts=%u write_errno=%d",why,stage,unsigned(fgb.capture),
             (unsigned long long)fgb.stored,fgb.sink.files,fgb.sink.lastErrno);
        fgbRelease();fgb.control.refused();fgbOn=false;
    }
    // Arming, at the first present after the poll saw diag-fg-burst (a command buffer is recording, no pass is open).
    // Returns the reason of a refusal, or null.
    const char* fgbAllocate(const Image& presentImage) {
        FgbState& fgb=*fgbP;
        namespace fb=recomp::fgb;
        if(rsK!=rscale::One)return "render-scale"; // (the depth target is not 1280 x 720 texels at another scale; the ring would not match the present image)
        if(presentImage.width!=fb::Width || presentImage.height!=fb::Height)return "present-size";
        if(!depthPipeline || !depthSetLayout || !depthLayout || !nearestSampler)return "depth-pipeline";
        if(!depthWordPipeline || !depthWordStorage || !depthWordSetLayout || !depthWordLayout)return "word-pipeline";
        // (R32F storage images: the renderer's own depth resolves write them without asking; RGBA8 ones: depthWordStorage above)
        fgb.gpuBytes=0;
        const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3*fb::Frames},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*fb::Frames}}};
        VkDescriptorPoolCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;p.maxSets=2*fb::Frames;p.poolSizeCount=2;p.pPoolSizes=sizes.data();
        if(vkCreateDescriptorPool(d.device,&p,nullptr,&fgb.pool)!=VK_SUCCESS){fgb.pool=VK_NULL_HANDLE;return "descriptor-pool";}
        constexpr VkImageUsageFlags Copy=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        constexpr VkImageUsageFlags Store=Copy|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT; // (as the renderer's own depth-resolve and word images)
        const char* why=nullptr;
        for(auto& s:fgb.sets) {
            if(!fgbImage(VK_FORMAT_R8G8B8A8_UNORM,Copy,s.color) || !fgbImage(VK_FORMAT_R32_SFLOAT,Store,s.depth) ||
               !fgbImage(VK_FORMAT_R8G8B8A8_UNORM,Store,s.word)){why="image-allocation";break;}
            toGeneral(s.color);toGeneral(s.depth);toGeneral(s.word);
            const VkDescriptorSetLayout layouts[2]{depthSetLayout,depthWordSetLayout};VkDescriptorSet out[2]{};
            VkDescriptorSetAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;a.descriptorPool=fgb.pool;a.descriptorSetCount=2;a.pSetLayouts=layouts;
            if(vkAllocateDescriptorSets(d.device,&a,out)!=VK_SUCCESS){why="descriptor-sets";break;}
            s.depthSet=out[0];s.wordSet=out[1];
        }
        return why;
    }
    // ---- hook: the 60-present switch poll
    [[gnu::noinline]] void fgbWindow() noexcept {
        fgGenWindow(); // framegen-1b: diag-fg-between and its files (fg_gen_renderer.inc; rides on this hook so that present() is not edited)
        namespace fb=recomp::fgb;
        bool arm=false,go=false;fgbFiles(arm,go);
        if(!fgbP) { // (never armed in this run)
            switch(fgbLaunch.poll(arm)) {
            case fb::Launch::Is::Absent: return; // the file is absent: no work, no log line
            case fb::Launch::Is::LeftOverFirst:
                GLOG("RENDER_DIAG_FG_BURST left_over=1 (diag-fg-burst was already there at this run's first poll: it does NOT arm; remove it, wait one window, create it again)");
                return;
            case fb::Launch::Is::LeftOverStill: return;
            case fb::Launch::Is::MayArm: break;
            }
            fgbP=new(std::nothrow) FgbState;
            if(!fgbP){GLOG("RENDER_DIAG_FG_BURST refused=host-memory stage=arming capture=0000 frames_stored=0 parts=0 write_errno=0");return;}
        }
        FgbState& fgb=*fgbP;
        if(!arm && fgb.control.state==fb::State::Idle)return; // (the file is absent and nothing is held: no work, no log line)
        const bool stale=fgb.control.goStale;
        switch(fgb.control.window(arm,go,fgb.ring.held())) {
        case fb::Act::Arm:
            fgbOn=true;fgb.stored=fgb.unusable=0;fgb.capture=0;fgb.sink=fb::DirSink{};fgbEveryFrame();
            GLOG("RENDER_DIAG_FG_BURST arming=1 go_present=%u%s",unsigned(fgb.control.goStale),
                 fgb.control.goStale?" (the diag-fg-burst-go that is already there cannot trigger: remove it, then create it again)":"");
            break;
        case fb::Act::Cancel: fgbOn=false;GLOG("RENDER_DIAG_FG_BURST disarmed=1 stage=arming");break;
        case fb::Act::Disarm:
            GLOG("RENDER_DIAG_FG_BURST disarmed=1 stage=armed frames_stored=%llu unusable=%llu files=0",(unsigned long long)fgb.stored,(unsigned long long)fgb.unusable);
            fgbRelease();fgbOn=false;
            break;
        case fb::Act::Trigger: fgbTrigger();break;
        case fb::Act::Unlatch: GLOG("RENDER_DIAG_FG_BURST ready=1 (diag-fg-burst was removed: the next one arms again)");break;
        case fb::Act::None:
            if(stale && !fgb.control.goStale)GLOG("RENDER_DIAG_FG_BURST go_present=0 (the trigger can fire now)");
            break;
        }
    }
    // The trigger: the ring is frozen; the read-out buffers and the capture's directory are made before any file is written.
    // The directory comes LAST: a trigger refused for disk space or memory leaves nothing on the disk (not even fgburst/).
    void fgbTrigger() noexcept {
        FgbState& fgb=*fgbP;
        namespace fb=recomp::fgb;
        fgb.held=fgb.ring.held();fgb.drainNext=fgb.drainDone=0;++fgb.serial;fgb.triggerPresent=counters.presents;fgb.sink=fb::DirSink{};
        if(!fb::roomFor(FgbTrace,FgbDiskBytes)){fgbRefuse("disk-space","trigger");return;} // (the directory the switch files are in: the same file system)
        try {fgb.stencilBytes.resize(fb::StencilBytes);} catch(...) {fgbRefuse("host-memory","trigger");return;}
        for(unsigned k=0;k<slotCount && k<FrameSlots;++k)
            if(!fgbStaging(fgb.staging[k],fgb.stagingCoherent[k])){fgbRefuse("readback-buffer","trigger");return;}
        const auto wanted=std::uint16_t(std::uint64_t(fb::monotonicNs())/1000003u+counters.presents*40503u);
        if(!fb::makeCaptureDir(FgbBase,wanted,&fgb.capture,fgb.sink.dir,sizeof fgb.sink.dir)){fgb.sink.lastErrno=errno;fgbRefuse("directory","trigger");return;}
        GLOG("RENDER_DIAG_FG_BURST triggered=1 capture=%04x frames=%u presents=%llu..%llu dir=fgburst/c%04x",unsigned(fgb.capture),fgb.held,
             (unsigned long long)fgb.sets[fgb.ring.slotOf(0)].meta.present,(unsigned long long)fgb.sets[fgb.ring.slotOf(fgb.held-1)].meta.present,unsigned(fgb.capture));
    }
    // ---- hook: draw(), where the renderer logs RENDER_POSTFX for a postfx shader's draw; the tonemap draw's camera constants.
    // Every frame while armed: fgbEveryFrame() makes the log's "once per 300 frames" test true for the tonemap's next draw.
    static_assert(post::PostfxShaders[0].hash==recomp::fgb::TonemapShader,"the tonemap is the first postfx shader");
    void fgbEveryFrame() noexcept {postfxLogged[0]=std::uint64_t{0}-300;} // (frame >= postfxLogged[0] + 300 holds for any frame)
    [[gnu::noinline]] void fgbTonemap(std::uint64_t psHash,const DrawInput& in) noexcept {
        fgGenTonemap(psHash,in); // framegen-1b: the frame's camera for the generator (a test of one flag unless diag-fg-between; rides on this hook so that draw() is not edited)
        if(!fgbOn)return;
        FgbState& fgb=*fgbP;
        if(psHash!=recomp::fgb::TonemapShader || !in.pixelConstants || fgb.control.state!=recomp::fgb::State::Armed)return;
        if(fgb.cameraInterval!=presentCount){fgb.camera={};fgb.cameraInterval=presentCount;}
        recomp::fgb::takeCamera(fgb.camera,*in.pixelConstants,frame,std::uint32_t(counters.draws-fgb.drawsBase));
    }
    // ---- hook: copyDepth() (a depth resolve reads `t`; no pass is open, its pending clears and residue are applied)
    [[gnu::noinline]] void fgbStencil(Target& t) noexcept {if(fgbOn)fgbStencilArmed(t);} // (thin: a load, a branch, a tail call)
    [[gnu::noinline]] void fgbStencilArmed(Target& t) noexcept {
        namespace fb=recomp::fgb;FgbState& fgb=*fgbP;
        if(fgb.control.state!=fb::State::Armed || fgb.stencilSeen)return;
        if(!t.depth || t.pitch!=fb::Width || !t.image.image)return; // (the shadow atlas, the reflection and minimap passes: another pitch)
        fgb.stencilSeen=true;fgb.stencilOk=false;fgb.stencilInterval=presentCount;fgb.stencilImage=t.image.image;
        fgb.stencilBase=t.base;fgb.stencilFormat=t.xenosFormat;fgb.stencilPitch=t.pitch;
        fgb.stencilAtDraw=std::uint32_t(counters.draws-fgb.drawsBase);fgb.stencilAtResolve=std::uint32_t(counters.resolves-fgb.resolvesBase);
        // (no stencil view yet: the renderer's own word pass makes it at this resolve; this frame has no stencil record)
        if(t.image.k!=rscale::One || t.image.width<fb::Width || t.image.height<fb::Height || !t.stencilView)return;
        try {
            auto& s=fgb.sets[fgb.ring.next()];
            const VkDescriptorImageInfo depthInfo{nearestSampler,t.image.view,VK_IMAGE_LAYOUT_GENERAL},outInfo{VK_NULL_HANDLE,s.word.view,VK_IMAGE_LAYOUT_GENERAL},
                stencilInfo{nearestSampler,t.stencilView,VK_IMAGE_LAYOUT_GENERAL};
            std::array<VkWriteDescriptorSet,3> w{};
            for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=s.wordSet;x.descriptorCount=1;}
            w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&depthInfo;
            w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;w[1].pImageInfo=&outInfo;
            w[2].dstBinding=2;w[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[2].pImageInfo=&stencilInfo;
            vkUpdateDescriptorSets(d.device,3,w.data(),0,nullptr);
            struct {std::int32_t region[6];std::uint32_t floatDepth,unused;} push{{0,0,0,0,std::int32_t(fb::Width),std::int32_t(fb::Height)},std::uint32_t(t.xenosFormat==17),0};
            static_assert(sizeof push==32);
            if(targetedBarriers)hazards({t.image.image},{s.word.image});else barrier();
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,depthWordPipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,depthWordLayout,0,1,&s.wordSet,0,nullptr);
            vkCmdPushConstants(cmd,depthWordLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,32,&push);
            vkCmdDispatch(cmd,(fb::Width+7)/8,(fb::Height+7)/8,1);
            afterWrite();
            fgb.stencilOk=true;
        } catch(...) {}
    }
    // The main depth target at the present, by the design's rule: the depth target of pitch 1280 with the newest write stamp.
    // Read as it is (pending alias clears and residue are NOT applied: the record says how many are pending).
    void fgbDepth(FgbSet& s,recomp::fgb::Meta& m) {
        FgbState& fgb=*fgbP;
        namespace fb=recomp::fgb;
        Target* best=nullptr;const Target* base0=nullptr;
        for(auto& [key,t]:targets) {
            if(!t.depth || t.pitch!=fb::Width || !t.image.image)continue;
            ++m.depthCandidates;
            if(!best || t.written>best->written)best=&t;
            if(t.base==0 && (!base0 || t.written>base0->written))base0=&t;
        }
        if(base0)m.base0Stamp=base0->written;
        if(!best)return;
        m.depthBase=best->base;m.depthFormat=best->xenosFormat;m.depthPitch=best->pitch;m.depthWidth=best->image.width;m.depthHeight=best->image.height;
        m.depthVkFormat=int(best->image.format);m.depthStamp=best->written;m.depthPending=unsigned(best->pendingClears.size());m.depthResidue=best->residuePending;
        m.depthIsStencilSource=m.stencil && fgb.stencilImage==best->image.image;
        if(best->image.k!=rscale::One || best->image.width<fb::Width || best->image.height<fb::Height)return;
        const VkDescriptorImageInfo depthInfo{nearestSampler,best->image.view,VK_IMAGE_LAYOUT_GENERAL},outInfo{VK_NULL_HANDLE,s.depth.view,VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet,2> w{};
        w[0].sType=w[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w[0].dstSet=w[1].dstSet=s.depthSet;w[0].descriptorCount=w[1].descriptorCount=1;
        w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&depthInfo;
        w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;w[1].pImageInfo=&outInfo;
        vkUpdateDescriptorSets(d.device,2,w.data(),0,nullptr);
        const std::array<std::int32_t,6> region{0,0,0,0,std::int32_t(fb::Width),std::int32_t(fb::Height)};
        if(targetedBarriers)hazards({best->image.image},{s.depth.image});else barrier();
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,depthPipeline); // (the plain resolve: the stored value, not rounded to 20e4)
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,depthLayout,0,1,&s.depthSet,0,nullptr);
        vkCmdPushConstants(cmd,depthLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,24,region.data());
        vkCmdDispatch(cmd,(fb::Width+7)/8,(fb::Height+7)/8,1);
        afterWrite();
        m.depth=true;
    }
    // One real frame into the ring (armed). Every present takes a slot, so a slot's descriptor sets are written again only 8
    // presents later; a frame whose present image is not the game's 1280x720 UNORM picture is stored without a colour picture.
    void fgbStore(const Image& presentImage) {
        FgbState& fgb=*fgbP;
        namespace fb=recomp::fgb;
        auto& s=fgb.sets[fgb.ring.next()];auto& m=s.meta;m=fb::Meta{};
        m.present=counters.presents;m.rendererFrame=frame;m.tag=presentTag;
        m.draws=std::uint32_t(counters.draws-fgb.drawsBase);m.resolves=std::uint32_t(counters.resolves-fgb.resolvesBase);
        const bool usable=presentImage.image && presentImage.width==fb::Width && presentImage.height==fb::Height &&
            presentImage.format==VK_FORMAT_R8G8B8A8_UNORM && !v369Mosaic && !census.active; // (not the sRGB image of diag-gamma-on, the mosaic, a census frame)
        if(usable) {
            VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={fb::Width,fb::Height,1};
            hazards({presentImage.image},{s.color.image});
            vkCmdCopyImage(cmd,presentImage.image,VK_IMAGE_LAYOUT_GENERAL,s.color.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
            afterWrite();
            m.color=true;
        } else ++fgb.unusable;
        // the frame's tonemap draws by the renderer's own counter (the hook itself sees only the first one of a frame)
        m.tonemapDraws=std::uint32_t(postfxDraws[0]-fgb.tonemapBase);
        // (what was taken while this frame was rendered: presentCount was advanced at the top of this present)
        if(fgb.camera.draws && fgb.cameraInterval+1==presentCount)m.camera=fgb.camera;
        if(fgb.stencilSeen && fgb.stencilInterval+1==presentCount) {
            m.stencil=fgb.stencilOk;m.stencilBase=fgb.stencilBase;m.stencilFormat=fgb.stencilFormat;m.stencilPitch=fgb.stencilPitch;
            m.stencilAtDraw=fgb.stencilAtDraw;m.stencilAtResolve=fgb.stencilAtResolve;
        }
        fgbDepth(s,m);
        if(m.tag) { // lane A's plan of this frame (its slot, the cap, the refresh estimate), when it was paced
            std::int64_t target=0;unsigned n=0;double period=0;
            if(recomp::pace::pacer().slot(m.tag,&target,&n,&period)){m.paced=true;m.slotNs=target;m.capRefreshes=n;m.periodNs=period;}
        }
        fgb.ring.stored();++fgb.stored;
    }
    // The read-out, one ring frame per present (oldest first): its three pictures into this frame slot's buffer; the files are
    // written when this frame's fence has signalled (fgbTake, from the slot's garbage list).
    void fgbReadBack() {
        FgbState& fgb=*fgbP;
        namespace fb=recomp::fgb;
        const unsigned j=fgb.drainNext;auto& s=fgb.sets[fgb.ring.slotOf(j)];auto& b=fgb.staging[slotIndex];
        if(!b.buffer){fgbRefuse("readback-buffer","read-out");return;}
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={fb::Width,fb::Height,1};
        hazards({s.color.image,s.depth.image,s.word.image},{});
        if(s.meta.color){c.bufferOffset=0;vkCmdCopyImageToBuffer(cmd,s.color.image,VK_IMAGE_LAYOUT_GENERAL,b.buffer,1,&c);}
        if(s.meta.depth){c.bufferOffset=fb::ColorBytes;vkCmdCopyImageToBuffer(cmd,s.depth.image,VK_IMAGE_LAYOUT_GENERAL,b.buffer,1,&c);}
        if(s.meta.stencil){c.bufferOffset=fb::ColorBytes+fb::DepthBytes;vkCmdCopyImageToBuffer(cmd,s.word.image,VK_IMAGE_LAYOUT_GENERAL,b.buffer,1,&c);}
        VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
        garbage.push_back([this,j,k=slotIndex,serial=fgb.serial]{fgbTake(j,k,serial);});
        ++fgb.drainNext;
    }
    // Frame j of the read-out is in host memory (the fence of the frame that copied it has signalled): its part files.
    [[gnu::noinline]] void fgbTake(unsigned j,unsigned bufferSlot,unsigned serial) noexcept {
        namespace fb=recomp::fgb;
        if(!fgbOn)return;
        FgbState& fgb=*fgbP;
        if(fgb.control.state!=fb::State::Draining || serial!=fgb.serial || j!=fgb.drainDone)return; // (a capture that was given up meanwhile)
        ++fgb.drainDone;
        auto& b=fgb.staging[bufferSlot];const auto& m=fgb.sets[fgb.ring.slotOf(j)].meta;
        if(fgb.sink.failures || !b.mapped || fgb.stencilBytes.size()!=fb::StencilBytes)return; // (after a failed write nothing more is written)
        if(!fgb.stagingCoherent[bufferSlot]) {
            VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=b.memory;range.size=VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(d.device,1,&range);
        }
        const auto* p=static_cast<const std::uint8_t*>(b.mapped);
        if(m.color && fb::writeObject(fgb.sink,fgb.capture,j,fb::KindColor,p,fb::ColorBytes)!=fb::partsFor(fb::ColorBytes))return;
        if(m.depth && fb::writeObject(fgb.sink,fgb.capture,j,fb::KindDepth,p+fb::ColorBytes,fb::DepthBytes)!=fb::partsFor(fb::DepthBytes))return;
        if(m.stencil) {
            fb::stencilOfWords(p+fb::ColorBytes+fb::DepthBytes,fgb.stencilBytes.data(),fb::Texels);
            fb::writeObject(fgb.sink,fgb.capture,j,fb::KindStencil,fgb.stencilBytes.data(),fb::StencilBytes);
        }
    }
    // The end of a read-out: the frames' records (with the present and display times heard meanwhile), the one line, release.
    void fgbClose() noexcept {
        FgbState& fgb=*fgbP;
        namespace fb=recomp::fgb;
        unsigned color=0,depth=0,stencil=0,camera=0,shown=0,paced=0,consecutive=1,tmLo=~0u,tmHi=0;std::uint64_t first=0,last=0;
        for(unsigned j=0;j<fgb.held;++j) {
            fb::Meta m=fgb.sets[fgb.ring.slotOf(j)].meta;m.capture=fgb.capture;m.index=j;m.frames=fgb.held;
            const auto t=fb::times().get(m.tag);m.callBeginNs=t.begin;m.callEndNs=t.end;m.displayNs=t.shown;
            color+=m.color;depth+=m.depth;stencil+=m.stencil;camera+=m.camera.draws!=0;shown+=m.displayNs!=0;paced+=m.paced;
            tmLo=std::min(tmLo,unsigned(m.tonemapDraws));tmHi=std::max(tmHi,unsigned(m.tonemapDraws));
            if(j && m.present!=last+1)consecutive=0;
            if(!j)first=m.present;
            last=m.present;
            char text[4096];const std::size_t n=fb::formatMeta(text,sizeof text,m);
            if(!fgb.sink.failures)fb::writeObject(fgb.sink,fgb.capture,j,fb::KindMeta,reinterpret_cast<const std::uint8_t*>(text),n);
        }
        GLOG("RENDER_DIAG_FG_BURST capture=%04x ok=%u frames=%u parts=%u bytes=%llu color=%u depth=%u stencil=%u camera=%u tonemap_draws=%u..%u displayed=%u paced=%u consecutive=%u "
             "presents=%llu..%llu frames_stored=%llu unusable=%llu write_failures=%u write_errno=%d dir=fgburst/c%04x",unsigned(fgb.capture),unsigned(!fgb.sink.failures),
             fgb.held,fgb.sink.files,(unsigned long long)fgb.sink.bytes,color,depth,stencil,camera,fgb.held?tmLo:0u,tmHi,shown,paced,consecutive,(unsigned long long)first,
             (unsigned long long)last,(unsigned long long)fgb.stored,(unsigned long long)fgb.unusable,fgb.sink.failures,fgb.sink.lastErrno,unsigned(fgb.capture));
        fgbRelease();fgb.control.closed();fgbOn=false;
    }
    // ---- hook: present(), after the blit into the present image (a command buffer is recording, no pass is open)
    [[gnu::noinline]] void fgbPresent(const Image& presentImage) noexcept {if(fgbOn|fgGenOn)fgbPresentEither(presentImage);} // (thin: two loads, a branch, a tail call; framegen-1b: + fgGenOn)
    [[gnu::noinline]] void fgbPresentEither(const Image& presentImage) noexcept { // framegen-1b: this frame's inputs for the generator, then the capture's own work
        if(fgGenOn)fgGenPresent(presentImage);
        if(fgbOn)fgbPresentArmed(presentImage);
    }
    [[gnu::noinline]] void fgbPresentArmed(const Image& presentImage) noexcept {
        namespace fb=recomp::fgb;FgbState& fgb=*fgbP;
        try {
            switch(fgb.control.state) {
            case fb::State::Arming:
                if(const char* why=fgbAllocate(presentImage)){fgb.control.armed(false);fgbRefuse(why,"arming");}
                else {
                    fgb.control.armed(true);fb::times().clear();fb::timesOn.store(true,std::memory_order_relaxed);
                    GLOG("RENDER_DIAG_FG_BURST armed=1 gpu_mb=%.1f images=%u (the last %u frames are kept from the next present on)",double(fgb.gpuBytes)/1048576.0,
                         3*fb::Frames,fb::Frames);
                }
                break;
            case fb::State::Armed: fgbStore(presentImage);break;
            case fb::State::Draining:
                if(fgb.drainNext<fgb.held)fgbReadBack();
                else if(fgb.drainDone>=fgb.held)fgbClose();
                else if(counters.presents>fgb.triggerPresent+600)fgbRefuse("read-out-timeout","read-out");
                break;
            default: break;
            }
        } catch(...) {fgbRefuse("exception","present");}
        fgb.camera={};fgb.stencilSeen=fgb.stencilOk=false;fgb.drawsBase=counters.draws;fgb.resolvesBase=counters.resolves; // the next frame
        fgb.tonemapBase=postfxDraws[0];
        if(fgbOn && fgb.control.state!=fb::State::Draining)fgbEveryFrame();
    }
