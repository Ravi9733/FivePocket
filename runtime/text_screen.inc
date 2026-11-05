// v818 (thor-radio, Thor edition only: included by hud_screen.h inside the Android class Screen under RECOMP_THOR_EDITION). Owner
// 2026-10-25: "Mission text can go on the bottom screen if feasible" (subtitles stay on the main screen). While the renderer replays the
// game's help box (wheel_replay_renderer.inc text lane, every frame the game shows it), the lower screen draws that replay over its map:
// the replay's area texel for texel at the top left inside the frame (hud::textLayout), premultiplied (only the box's own pixels). The
// replay is current for TextStaleFrames lower-screen frames after the last one (the help box gone = no more replays).
public:
    bool textWanted() const noexcept {return wanted() && textOn.load(std::memory_order_relaxed);}
    void publishText(const TextImg& t) noexcept {std::lock_guard l(tm);text=t;}
private:
    static constexpr unsigned TextStaleFrames=6;
    std::mutex tm;TextImg text{},textRep{};std::uint64_t textSeenSerial=0,textPasses=0;unsigned textAge=1000;std::atomic<bool> textOn{false};
    VkImageView textSetView=VK_NULL_HANDLE;VkBuffer textSetLut=VK_NULL_HANDLE;VkDescriptorSet textSetHandle=VK_NULL_HANDLE;
    bool textShow() const noexcept {return textAge<TextStaleFrames && textRep.view && textRep.width && textRep.height && textOn.load(std::memory_order_relaxed);}
    void textFrame(std::uint64_t& key) noexcept {
        {std::lock_guard l(tm);textRep=text;}
        if(textRep.serial!=textSeenSerial){textSeenSerial=textRep.serial;textAge=0;}else if(textAge<1000)++textAge;
        if(textShow() || textAge==TextStaleFrames)key^=textRep.serial*0x9E3779B97F4A7C15ull^textAge; // (each replay, and its disappearance, redraws)
    }
    VkDescriptorSet textSet(VkImageView view) noexcept {
        if(!view)return VK_NULL_HANDLE;
        if(view==textSetView && lutBuffer==textSetLut && textSetHandle)return textSetHandle;
        VkDescriptorSet s=VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
        if(vkAllocateDescriptorSets(dev,&ai,&s)!=VK_SUCCESS)return VK_NULL_HANDLE;
        writeSet(s,view);
        if(textSetHandle)replicaRetired.push_back(textSetHandle);
        textSetHandle=s;textSetView=view;textSetLut=lutBuffer;
        RECOMP_HUD2_LOG("RENDER_TEXT screen_set=1 view=%p",static_cast<void*>(view));
        return s;
    }
    // record(), inside the HUD pass after the map's draws: the help box (its own set).
    void textRecord(VkCommandBuffer cb,float w,float h) noexcept {
        if(!textShow())return;
        const auto list=hud::textDrawList(std::uint32_t(w),std::uint32_t(h),textRep.box,textRep.width,textRep.height);
        if(!list.n)return;
        const VkDescriptorSet s=textSet(textRep.view);
        if(!s)return;
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&s,0,nullptr);
        for(unsigned k=0;k<list.n;++k) {
            float push[24];std::memcpy(push,&list.d[k],sizeof(hud::Draw));
            push[20]=w;push[21]=h;push[22]=push[23]=1.0f/float(hud::RadarCapacity);
            vkCmdPushConstants(cb,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,push);
            vkCmdDraw(cb,4,1,0,0);
        }
        if(!(textPasses++%256))RECOMP_HUD2_LOG("RENDER_TEXT passes=%llu box=%.0f,%.0f..%.0f,%.0f",(unsigned long long)textPasses,
            double(textRep.box[0]),double(textRep.box[1]),double(textRep.box[2]),double(textRep.box[3]));
    }
