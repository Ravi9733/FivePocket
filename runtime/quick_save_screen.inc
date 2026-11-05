// v820 (thor-radio, Thor edition only: included by hud_screen.h inside the Android class Screen under RECOMP_THOR_EDITION, after
// resume_screen.inc whose label image it shares). The lower screen's Quick Save button (quick_save.h): drawn over the map while the
// lower screen shows the HUD map (not a wheel, the phone, a pause screen, Loading or a hidden HUD); the pad machine (map_tap.h) is told
// per picture whether it is shown, and the pressed look follows its macro. Setting word save=off = no button.
private:
    std::atomic<bool> saveOn{false};bool saveShow=false,saveShowNow=false,savePressed=false,saveFailed=false;std::uint64_t savePasses=0;
    VkDescriptorSet saveSetHandle=VK_NULL_HANDLE;VkBuffer saveSetLut=VK_NULL_HANDLE;
    void saveTell(bool shown) noexcept {
#if defined(__ANDROID__)
        maptap::machine().setSaveShown(shown);
#else
        (void)shown;
#endif
    }
    bool saveBusy() noexcept {
#if defined(__ANDROID__)
        return maptap::machine().quickSaveBusy();
#else
        return false;
#endif
    }
    void saveOff() noexcept {saveShow=saveShowNow=false;saveTell(false);}
    // prepare(), under hm, after the wheel / Resume decisions: shown or not, pressed or not (both change the picture's key).
    void saveFrame(std::uint64_t& key) noexcept {
        const auto L=maptap::quickSaveLayout(float(extent.width),float(extent.height));
        saveShow=!saveFailed && saveOn.load(std::memory_order_relaxed) && replicaMode && !hudHidden && !wheelMode && !resumeMode && L.ok;
        saveTell(saveShow);
        savePressed=saveShow && saveBusy();
        if(saveShow)key^=savePressed?0x5153415645505245ull:0x5153415645534857ull;
    }
    // record(), before the pass: the label image (shared with the Resume screen) made and uploaded once. False = no button this picture.
    bool saveUpload(VkCommandBuffer cb) noexcept {
        if(!resumeObjects()){saveFailed=true;saveTell(false);RECOMP_HUD2_LOG("RENDER_QSAVE off why=objects");return false;}
        labelUpload(cb);
        return true;
    }
    VkDescriptorSet saveSet() noexcept {
        if(saveSetHandle && saveSetLut==lutBuffer)return saveSetHandle;
        VkDescriptorSet s=VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&setLayout;
        if(vkAllocateDescriptorSets(dev,&ai,&s)!=VK_SUCCESS)return VK_NULL_HANDLE;
        const VkDescriptorImageInfo li{sampler,labelView,VK_IMAGE_LAYOUT_GENERAL};const VkDescriptorBufferInfo bi{lutBuffer,0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        for(auto& x:w){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=s;x.descriptorCount=1;}
        w[0].dstBinding=0;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&li;
        w[1].dstBinding=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&bi;
        w[2].dstBinding=2;w[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[2].pImageInfo=&li; // (binding 2 unused here)
        vkUpdateDescriptorSets(dev,3,w,0,nullptr);
        if(saveSetHandle)replicaRetired.push_back(saveSetHandle);
        saveSetHandle=s;saveSetLut=lutBuffer;
        RECOMP_HUD2_LOG("RENDER_QSAVE screen_set=1");
        return s;
    }
public:
    // The button's draws (also the host test's): the box, then the label texel for texel (rows of the shared label image).
    static hud::DrawList saveDrawList(float w,float h,bool pressed,float labelRow) noexcept {
        hud::DrawList out;
        const auto L=maptap::quickSaveLayout(w,h);
        if(!L.ok)return out;
        {
            auto& d=out.add();d.rect[0]=L.box.x0;d.rect[1]=L.box.y0;d.rect[2]=L.box.x1;d.rect[3]=L.box.y1;
            d.shape[0]=L.box.x0;d.shape[1]=L.box.y0;d.shape[2]=L.box.x1;d.shape[3]=L.box.y1;
            for(unsigned k=0;k<3;++k)d.color[k]=pressed?float(maptap::QsPressedFill)/255.0f:0.0f;
            d.color[3]=pressed?1.0f:maptap::QsFillAlpha;
            d.param[0]=hud::KindBox;d.param[1]=L.radius;d.param[3]=1;
        }
        {
            auto& d=out.add();d.rect[0]=L.label.x0;d.rect[1]=L.label.y0;d.rect[2]=L.label.x1;d.rect[3]=L.label.y1;
            d.shape[0]=0;d.shape[1]=labelRow+(pressed?float(maptap::IconPx):0.0f);d.shape[2]=float(maptap::IconPx);d.shape[3]=d.shape[1]+float(maptap::IconPx); // (v832: the floppy)
            d.param[0]=hud::KindLabel;d.param[3]=0;
        }
        return out;
    }
private:
    // record(), inside the HUD pass after the map's draws (and the help box): the button (its own set).
    void saveRecord(VkCommandBuffer cb,float w,float h) noexcept {
        const auto list=saveDrawList(w,h,savePressed,float(SaveTexRow));
        if(!list.n)return;
        const VkDescriptorSet s=saveSet();
        if(!s)return;
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&s,0,nullptr);
        for(unsigned k=0;k<list.n;++k) {
            float push[24];std::memcpy(push,&list.d[k],sizeof(hud::Draw));
            push[20]=w;push[21]=h;push[22]=push[23]=1.0f/float(hud::RadarCapacity);
            vkCmdPushConstants(cb,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,push);
            vkCmdDraw(cb,4,1,0,0);
        }
        if(!(savePasses++%256))RECOMP_HUD2_LOG("RENDER_QSAVE passes=%llu pressed=%u extent=%.0fx%.0f",(unsigned long long)savePasses,unsigned(savePressed),double(w),double(h));
    }
