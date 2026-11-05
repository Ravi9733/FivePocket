// thor-maptap (v661 / v663 / v665; v667 rework, Thor edition only: included by hud_screen.h inside the Android class Screen under
// RECOMP_THOR_EDITION, after wheel_screen.inc). Owner 2026-10-24 on v665: "the resume game button still looks like trash, and the screen
// colors still do not match in the map menu"; "Resume should be modeled after the ps5s button that says resume, transparent, and match
// the color of the screen with text in the same font as the sign in text".
// While the renderer says the game shows a pause screen (map_tap_renderer.inc: from the first frame with the pause blur and no tonemap
// to the first frame without), the lower screen is (resume_layout.h):
//  - the main screen's own picture: the renderer's copy of THIS frame's present image before the fps overlay (mirrorSource()), sampled
//    through the same display tables (KindReplica, bilinear), cover-fit: the main panel's centre at its own scale. The key changes every
//    frame, so the lower screen follows the main one frame by frame;
//  - the PS5-style pill: a fully rounded KindBox, ResumePillRgb at ResumePillAlpha over the mirror (pressed: opaque light);
//  - "Resume Game" (KindLabel, binding 0 = the label image: white over the translucent pill, dark over the pressed one).
// This pass's own set: binding 0 = the label image, 1 = the display tables, 2 = the mirrored source (a new set when the source view or
// the tables change; replaced ones retire as hud4's do).
public:
    // The renderer, per paused frame: the view and size of its copy of the present image (map_tap_renderer.inc mapTapMirror).
    void mirrorSource(VkImageView view,std::uint32_t w,std::uint32_t h) noexcept {
        mirrorSize.store(std::uint64_t(w)<<32|h,std::memory_order_relaxed);mirrorPublished.store(view,std::memory_order_release);
    }
    // The renderer thread, per real frame: whether a pause screen is up; whether the library is closing it (the pressed look).
    // (v811: pill = the Resume pill and its label; false = the blurred backdrop alone - a pause tab other than the map)
    void publishResume(bool on,bool pressed,bool pill=true) noexcept {resumePressed.store(pressed,std::memory_order_relaxed);resumePillOn.store(pill,std::memory_order_relaxed);
        resumeOn.store(on,std::memory_order_relaxed);}
    std::atomic<bool> resumePillOn{true};
    // thor-char (v685): the renderer thread, per real frame: whether a character switch runs (the Loading screen, loading_layout.h).
    void publishLoading(bool on,bool tint=false) noexcept {loadingTint.store(tint,std::memory_order_relaxed);loadingOn.store(on,std::memory_order_relaxed);} // (v693: tint)
    std::atomic<bool> loadingTint{false};
private:
    std::atomic<bool> resumeOn{false},resumePressed{false},loadingOn{false};bool loadingMode=false;std::uint64_t loadingPasses=0;
    std::atomic<VkImageView> mirrorPublished{VK_NULL_HANDLE};std::atomic<std::uint64_t> mirrorSize{0}; // (the renderer thread writes them)
    VkImageView mirrorView=VK_NULL_HANDLE;std::uint32_t mirrorW=0,mirrorH=0; // (prepare()'s snapshot)
    bool resumeMode=false,resumeFailed=false,labelUploaded=false,resumeIsPressed=false;std::uint64_t resumePasses=0,resumeSerial=0;
    VkImage labelImage=VK_NULL_HANDLE;VkDeviceMemory labelMemory=VK_NULL_HANDLE;VkImageView labelView=VK_NULL_HANDLE;
    VkBuffer labelStage=VK_NULL_HANDLE;VkDeviceMemory labelStageMemory=VK_NULL_HANDLE;
    VkDescriptorSet resumeSetHandle=VK_NULL_HANDLE;VkBuffer resumeSetLut=VK_NULL_HANDLE;VkImageView resumeSetMirror=VK_NULL_HANDLE;
    // (rows 0..LabelH: white text; LabelH..: the pressed look's dark text; thor-char v685: then "Loading" and the spinner's frames)
    // (v820: then the Quick Save label, white and dark: rows SaveTexRow..)
    static constexpr std::uint32_t SaveTexRow=maptap::LabelTexH;
    static constexpr std::uint32_t ResumeTexW=std::max<std::uint32_t>(maptap::LabelTexW,maptap::IconPx),ResumeTexH=SaveTexRow+2*maptap::IconPx; // (v832: the floppy)
    // prepare(), under hm: this frame shows the Resume screen while the renderer says so and the mirror's source is known.
    void resumeFrame(std::uint64_t& key) noexcept {
        resumeMode=!resumeFailed && !wheelMode && resumeOn.load(std::memory_order_relaxed) && mirrorView && mirrorW && mirrorH; // (mirror: snapshot in resumeSnapshot())
        loadingMode=!resumeFailed && !wheelMode && !resumeMode && loadingOn.load(std::memory_order_relaxed) && mirrorView && mirrorW && mirrorH; // (thor-char v685)
        if(loadingMode)resumeMode=true; // (the same pass; resumeRecord draws the Loading pill)
        resumeIsPressed=!loadingMode && resumePressed.load(std::memory_order_relaxed);
        if(resumeMode)key=0x524553554D453637ull^(++resumeSerial<<1)^(resumeIsPressed?1u:0u); // (a new picture every frame: the mirror moves)
    }
    // prepare(), under hm, before the hidden-HUD rule (v667, owner: "the second screen still lags when leaving the pause screen"): on the
    // first frames after a pause screen the HUD comes back at once - the last minimap replica counts as current (it is the game's own,
    // from before the pause) until the game's next replica arrives (the replay runs every DefaultRate frames), at full strength.
    bool resumeWas=false;unsigned resumeAfter=0;std::uint64_t resumeAfterSerial=0;
    void resumeLeave(bool& hidden) noexcept {
        const bool on=resumeMode;
        if(resumeWas && !on){resumeAfter=1;resumeAfterSerial=rep.serial;}
        else if(resumeAfter && resumeAfter<ResumeAfterFrames)++resumeAfter;else resumeAfter=0;
        resumeWas=on;
        if(!resumeAfter)return;
        if(rep.serial!=resumeAfterSerial){RECOMP_HUD2_LOG("RENDER_MAPTAP hud-back new_replica_after=%u frames",resumeAfter);resumeAfter=0;return;}
        if(replicaMode && replicaInfo.ever){replicaInfo.age=0;replicaInfo.stars=rep.stars;hidden=false;}
        if(resumeAfter==1)RECOMP_HUD2_LOG("RENDER_MAPTAP hud-back first_frame=1 replica=%u ever=%u",unsigned(replicaMode),unsigned(replicaInfo.ever));
    }
    static constexpr unsigned ResumeAfterFrames=30;
    void resumeSnapshot() noexcept {
        mirrorView=mirrorPublished.load(std::memory_order_acquire);const auto sz=mirrorSize.load(std::memory_order_relaxed);mirrorW=std::uint32_t(sz>>32);mirrorH=std::uint32_t(sz);
    }
    bool resumeObjects() noexcept {
        if(labelView)return true;
        VkImageCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent={ResumeTexW,ResumeTexH,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(!labelImage && vkCreateImage(dev,&ii,nullptr,&labelImage)!=VK_SUCCESS){labelImage=VK_NULL_HANDLE;return false;}
        if(!labelMemory) {
            VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev,labelImage,&r);
            const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(!type)return false;
            VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=r.size;ai.memoryTypeIndex=*type;
            if(vkAllocateMemory(dev,&ai,nullptr,&labelMemory)!=VK_SUCCESS){labelMemory=VK_NULL_HANDLE;return false;}
            if(vkBindImageMemory(dev,labelImage,labelMemory,0)!=VK_SUCCESS)return false;
        }
        if(!labelStage) {
            const VkDeviceSize bytes=VkDeviceSize(ResumeTexW)*ResumeTexH*4;
            VkBufferCreateInfo bi{};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=bytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            if(vkCreateBuffer(dev,&bi,nullptr,&labelStage)!=VK_SUCCESS){labelStage=VK_NULL_HANDLE;return false;}
            VkMemoryRequirements r{};vkGetBufferMemoryRequirements(dev,labelStage,&r);
            const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(!type)return false;
            VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=r.size;ai.memoryTypeIndex=*type;
            if(vkAllocateMemory(dev,&ai,nullptr,&labelStageMemory)!=VK_SUCCESS){labelStageMemory=VK_NULL_HANDLE;return false;}
            if(vkBindBufferMemory(dev,labelStage,labelStageMemory,0)!=VK_SUCCESS)return false;
            void* p=nullptr;if(vkMapMemory(dev,labelStageMemory,0,VK_WHOLE_SIZE,0,&p)!=VK_SUCCESS || !p)return false;
            auto* t=static_cast<std::uint8_t*>(p);
            std::memset(t,0,std::size_t(ResumeTexW)*ResumeTexH*4);
            const auto put=[&](std::uint32_t x,std::uint32_t y,std::uint32_t a,std::uint32_t text) {
                std::uint8_t* q=t+(std::size_t(y)*ResumeTexW+x)*4;const std::uint8_t c=std::uint8_t((text*a+127)/255);q[0]=q[1]=q[2]=c;q[3]=std::uint8_t(a); // (premultiplied)
            };
            for(std::uint32_t half=0;half<2;++half) {
                const std::uint32_t text=half?maptap::ResumePressedText:maptap::ResumeText;
                for(std::uint32_t y=0;y<maptap::LabelH;++y)for(std::uint32_t x=0;x<maptap::LabelW;++x)
                    put(x,half*maptap::LabelH+y,maptap::LabelCoverage[y*maptap::LabelW+x],text);
            }
            // thor-char (v685): "Loading" (white) and the spinner's frames (white), loading_layout.h
            for(std::uint32_t y=0;y<maptap::LoadingH;++y)for(std::uint32_t x=0;x<maptap::LoadingW;++x)
                put(x,maptap::LoadingTextRow+y,maptap::LoadingCoverage[y*maptap::LoadingW+x],maptap::ResumeText);
            for(std::uint32_t f=0;f<maptap::SpinFrames;++f)for(std::uint32_t y=0;y<maptap::SpinPx;++y)for(std::uint32_t x=0;x<maptap::SpinPx;++x)
                put((f%maptap::SpinCols)*maptap::SpinPx+x,maptap::LoadingSpinRow+(f/maptap::SpinCols)*maptap::SpinPx+y,maptap::spinCoverage(f,x,y),maptap::ResumeText);
            for(std::uint32_t half=0;half<2;++half) // v832: the floppy disk (white; the pressed look's dark one), quick_save_icons.h
                for(std::uint32_t y=0;y<maptap::IconPx;++y)for(std::uint32_t x=0;x<maptap::IconPx;++x)
                    put(x,SaveTexRow+half*maptap::IconPx+y,maptap::FloppyCoverage[y*maptap::IconPx+x],half?maptap::QsPressedText:maptap::QsText);
            vkUnmapMemory(dev,labelStageMemory);
        }
        VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=labelImage;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.format=VK_FORMAT_R8G8B8A8_UNORM;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(vkCreateImageView(dev,&vi,nullptr,&labelView)!=VK_SUCCESS){labelView=VK_NULL_HANDLE;return false;}
        return true;
    }
    // v820: the label image's one upload (outside a render pass: the Resume pass and the Quick Save button both call it before theirs).
    void labelUpload(VkCommandBuffer cb) noexcept {
        if(labelUploaded)return;
        VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=labelImage;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={ResumeTexW,ResumeTexH,1};
        vkCmdCopyBufferToImage(cb,labelStage,labelImage,VK_IMAGE_LAYOUT_GENERAL,1,&c);
        b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        labelUploaded=true;
    }
    // record(), under hm: true = this pass draws the Resume screen (list and set filled).
    bool resumeRecord(VkCommandBuffer cb,hud::DrawList& list,VkDescriptorSet& useSet) noexcept {
        if(!resumeMode)return false;
        if(!resumeObjects()){resumeFailed=true;RECOMP_HUD2_LOG("RENDER_MAPTAP resume off why=objects");return false;}
        if(!resumeSetHandle || resumeSetLut!=lutBuffer || resumeSetMirror!=mirrorView) {
            VkDescriptorSet s=VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
            if(vkAllocateDescriptorSets(dev,&ai,&s)!=VK_SUCCESS){resumeFailed=true;RECOMP_HUD2_LOG("RENDER_MAPTAP resume off why=set");return false;}
            const VkDescriptorImageInfo li{sampler,labelView,VK_IMAGE_LAYOUT_GENERAL},mi{sampler,mirrorView,VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo bi{lutBuffer,0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet w[3]{};
            for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=s;x.descriptorCount=1;}
            w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&li;
            w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
            w[2].dstBinding=2;w[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[2].pImageInfo=&mi;
            vkUpdateDescriptorSets(dev,3,w,0,nullptr);
            if(resumeSetHandle)replicaRetired.push_back(resumeSetHandle);
            resumeSetHandle=s;resumeSetLut=lutBuffer;resumeSetMirror=mirrorView;
        }
        labelUpload(cb);
        list=loadingMode?loadingDrawList(float(extent.width),float(extent.height),float(mirrorW),float(mirrorH),
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                loadingTint.load(std::memory_order_relaxed)) // (thor-char v685; v693: the pause menu's tint)
            :resumeDrawList(float(extent.width),float(extent.height),float(mirrorW),float(mirrorH),resumeIsPressed,resumePillOn.load(std::memory_order_relaxed));
        if(loadingMode && !(loadingPasses++%256))RECOMP_HUD2_LOG("RENDER_LOADING passes=%llu extent=%ux%u mirror=%ux%u",(unsigned long long)loadingPasses,extent.width,extent.height,mirrorW,mirrorH);
        useSet=resumeSetHandle;
        if(!(resumePasses++%256))RECOMP_HUD2_LOG("RENDER_MAPTAP resume passes=%llu extent=%ux%u mirror=%ux%u",(unsigned long long)resumePasses,
            extent.width,extent.height,mirrorW,mirrorH);
        return true;
    }
public:
    // The Resume screen's draws (resume_layout.h): the mirror, the pill, the label.
    static hud::DrawList resumeDrawList(float w,float h,float sw,float sh,bool pressed,bool pill=true) noexcept {
        hud::DrawList out;
        { // the main screen's picture, cover-fit, through the display tables (KindReplica: bilinear; flags 0: opaque)
            const auto c=maptap::mirrorCrop(sw,sh,w,h);
            auto& d=out.add();d.rect[0]=0;d.rect[1]=0;d.rect[2]=w;d.rect[3]=h;
            d.shape[0]=c.x0;d.shape[1]=c.y0;d.shape[2]=c.x1;d.shape[3]=c.y1;
            d.color2[0]=1.0f/sw;d.color2[1]=1.0f/sh;d.param[0]=hud::KindReplica;d.param[1]=0;d.param[2]=1.0f;d.param[3]=0;
        }
        const auto L=maptap::resumeLayout(w,h);
        if(!L.ok || !pill)return out; // (v811: a pause tab other than the map: the backdrop alone)
        { // v681: the pill's soft shadow (KindShadow: outside the pill only), the quad grown by the fade's width
            auto& d=out.add();const float g=maptap::ResumeShadowPx+maptap::ResumeShadowDy+1.0f;
            d.rect[0]=L.pill.x0-g;d.rect[1]=L.pill.y0-g;d.rect[2]=L.pill.x1+g;d.rect[3]=L.pill.y1+g;
            d.shape[0]=L.pill.x0;d.shape[1]=L.pill.y0+maptap::ResumeShadowDy;d.shape[2]=L.pill.x1;d.shape[3]=L.pill.y1+maptap::ResumeShadowDy;
            d.color2[0]=L.pill.x0;d.color2[1]=L.pill.y0;d.color2[2]=L.pill.x1;d.color2[3]=L.pill.y1;
            d.color[0]=d.color[1]=d.color[2]=0;d.color[3]=maptap::ResumeShadowAlpha;
            d.param[0]=hud::KindShadow;d.param[1]=L.radius;d.param[2]=maptap::ResumeShadowPx;d.param[3]=0;
        }
        { // the pill: white at ResumePillAlpha (pressed: opaque light), fully rounded, a 1-px antialiased edge
            auto& d=out.add();d.rect[0]=L.pill.x0;d.rect[1]=L.pill.y0;d.rect[2]=L.pill.x1;d.rect[3]=L.pill.y1;
            d.shape[0]=L.pill.x0;d.shape[1]=L.pill.y0;d.shape[2]=L.pill.x1;d.shape[3]=L.pill.y1;
            for(unsigned k=0;k<3;++k)d.color[k]=(pressed?maptap::ResumePressedFill:maptap::ResumePillRgb[k])/255.0f;
            d.color[3]=pressed?1.0f:maptap::ResumePillAlpha;
            d.param[0]=hud::KindBox;d.param[1]=L.radius;d.param[3]=1;
        }
        { // the label, texel for texel (the pressed look's dark text: the image's second half)
            auto& d=out.add();d.rect[0]=L.label.x0;d.rect[1]=L.label.y0;d.rect[2]=L.label.x0+float(maptap::LabelW);d.rect[3]=L.label.y0+float(maptap::LabelH);
            d.shape[0]=0;d.shape[1]=pressed?float(maptap::LabelH):0.0f;d.shape[2]=float(maptap::LabelW);d.shape[3]=d.shape[1]+float(maptap::LabelH);
            d.param[0]=hud::KindLabel;d.param[3]=0;
        }
        return out;
    }
    // thor-char (v685): the Loading screen's draws (loading_layout.h): the mirror, the pill's shadow, the pill, the spinner's frame for
    // `ms`, "Loading" - the Resume screen's look; nothing pressed (not tappable).
    static hud::DrawList loadingDrawList(float w,float h,float sw,float sh,std::int64_t ms,bool tint=false) noexcept {
        hud::DrawList out=resumeDrawList(w,h,sw,sh,false);
        out.n=out.n>0?1u:0u; // (keep the mirror only)
        if(out.n && tint) { // v693: the pause menu's look over the mirror (hud_screen.frag kind 4 flag 16)
            auto& m=out.d[0];m.param[1]=16;for(unsigned k=0;k<3;++k)m.color[k]=maptap::PauseTintOffset[k];m.color[3]=maptap::PauseTintGain;m.color2[2]=maptap::PauseTintSat;
        }
        const auto L=maptap::spinnerLayout(w,h); // (v687: the spinner alone, centred; v689, owner: "the spinning wheel doesn't need the shadow": no halo)
        if(!L.ok)return out;
        { // the spinner's frame (texel for texel)
            const unsigned f=maptap::spinFrame(ms);
            auto& d=out.add();d.rect[0]=L.spin.x0;d.rect[1]=L.spin.y0;d.rect[2]=L.spin.x1;d.rect[3]=L.spin.y1;
            d.shape[0]=float((f%maptap::SpinCols)*maptap::SpinPx);d.shape[1]=float(maptap::LoadingSpinRow+(f/maptap::SpinCols)*maptap::SpinPx);
            d.shape[2]=d.shape[0]+float(maptap::SpinPx);d.shape[3]=d.shape[1]+float(maptap::SpinPx);d.param[0]=hud::KindLabel;d.param[3]=0;
        }
        return out;
    }
private:
