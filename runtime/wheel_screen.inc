// thor-wheel (v601, Thor edition only: included by hud_screen.h inside the Android class Screen under RECOMP_THOR_EDITION).
// While the renderer's wheel replica (wheel_replay_renderer.inc) is current, the second screen shows it instead of the HUD picture:
// the ring's square texel for texel, centred, and the stats panel's rectangle in the top right corner (wheelrep::layout); the
// minimap, bars and stars are not drawn. The replica is current for wheelrep::StaleFrames real frames after the last one: the HUD
// picture comes back the next frame after the wheel closes (hud4 keeps replaying the minimap meanwhile, so it is current). Setting: files/settings/second-screen.txt "wheel=main" = the game's own wheel on
// the main screen (the renderer skips and replays nothing; this never shows a wheel).
// The calls: prepare() -> wheelFrame() after its frame key; record() -> wheelRecord() before the replica / HUD choice; the swapchain's
// life sets wheelOn (beside v576's hideOn).
public:
    // The renderer, per operation: whether the lower screen takes the wheel (a HUD swapchain exists, the setting is Lower).
    bool wheelWanted() const noexcept {return wanted() && wheelOn.load(std::memory_order_relaxed);}
    // thor-radio (v651): the same for the radio wheel (the setting "Radio wheel: Lower screen", files/settings/second-screen.txt radio=main = Main)
    bool radioWanted() const noexcept {return wanted() && radioOn.load(std::memory_order_relaxed);}
    // thor-char: the character wheel (the setting "Character wheel: Lower screen", second-screen.txt character=main = Main)
    bool charWanted() const noexcept {return wanted() && charOn.load(std::memory_order_relaxed);}
    // v815: the game's phone (second-screen.txt phone=main = Main)
    bool phoneWanted() const noexcept {return wanted() && phoneOn.load(std::memory_order_relaxed);}
    // The renderer thread, after a wheel replay recorded into its command buffer (as publishReplica).
    void publishWheel(const Wheel& w) noexcept {std::lock_guard l(wm);wheel=w;}
private:
    // (v619: the HUD returns the frame after the wheel's last replay; 1 more for a late replay. v628: the rule is wheelrep::ScreenChoice,
    // the same code the host harness runs per present for its LB-held sequence)
    std::mutex wm;Wheel wheel{},wheelRep{};std::uint64_t wheelPasses=0;wheelrep::ScreenChoice wheelChoice{};bool wheelMode=false;
    std::atomic<bool> wheelOn{false},radioOn{false},charOn{false},phoneOn{false}; // (v651: radioOn; thor-char: charOn; v815: phoneOn)
    VkImageView wheelSetView=VK_NULL_HANDLE;VkBuffer wheelSetLut=VK_NULL_HANDLE;VkDescriptorSet wheelSetHandle=VK_NULL_HANDLE;
    // prepare(), under hm: this frame shows the wheel when its replica is current; the frame key then follows the replica's serial.
    void wheelFrame(std::uint64_t& key) noexcept {
        {std::lock_guard l(wm);wheelRep=wheel;}
        wheelMode=wheelChoice.frame((wheelRep.phone?phoneOn:wheelRep.character?charOn:wheelRep.radio?radioOn:wheelOn).load(std::memory_order_relaxed),wheelRep.serial,wheelRep.view && wheelRep.width && wheelRep.height); // (v651: the radio's own setting)
        if(wheelMode)key=wheelRep.serial*0x100000001B3ull^0x5748454C00000000ull;
    }
    // The set that names the wheel's view with the current tables (a replaced one is freed at the next swapchain, as hud4's).
    VkDescriptorSet wheelSet(VkImageView view) noexcept {
        if(!view)return VK_NULL_HANDLE;
        if(view==wheelSetView && lutBuffer==wheelSetLut && wheelSetHandle)return wheelSetHandle;
        VkDescriptorSet s=VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
        if(vkAllocateDescriptorSets(dev,&ai,&s)!=VK_SUCCESS)return VK_NULL_HANDLE;
        writeSet(s,view);
        if(wheelSetHandle)replicaRetired.push_back(wheelSetHandle);
        wheelSetHandle=s;wheelSetView=view;wheelSetLut=lutBuffer;
        RECOMP_HUD2_LOG("RENDER_WHEEL screen_set=1 view=%p sets_retired=%zu",static_cast<void*>(view),replicaRetired.size());
        return s;
    }
    // record(), under hm: true = this pass draws the wheel (list and set filled).
    bool wheelRecord(VkCommandBuffer cb,hud::DrawList& list,VkDescriptorSet& useSet) noexcept {
        if(!wheelMode)return false;
        const VkDescriptorSet s=wheelSet(wheelRep.view);
        if(!s)return false;
        VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; // (the renderer's replay, an earlier submission on this queue, wrote it as a colour attachment)
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&mb,0,nullptr,0,nullptr);
        list=wheelRep.phone?hud::phoneDrawList(extent.width,extent.height,wheelRep.ring,wheelRep.width,wheelRep.height,wheelRep.phoneTurn,wheelRep.stats) // (v815)
            :wheelRep.character?hud::charDrawList(extent.width,extent.height,wheelRep.ring,wheelRep.stats,wheelRep.width,wheelRep.height) // (thor-char)
                               :hud::wheelDrawList(extent.width,extent.height,wheelRep.ring,wheelRep.stats,wheelRep.width,wheelRep.height,wheelRep.k);
        useSet=s;++wheelPasses;
        return true;
    }
