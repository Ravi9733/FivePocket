// Actual copy arguments consume the GPU exposure result, never captured pixels.
{
    const auto call=read(p+"/exposure-copy/call.bin",48);Gta5TileResolveCapture ec;
    for(unsigned i=0;i<9;++i)ec.arguments[i]=be(call,4*i);
    ec.stack=be(call,36);ec.f1=(std::uint64_t{be(call,40)}<<32)|be(call,44);
    const auto payload=[&](const char* key,unsigned size){return read(p+"/exposure-copy/native-resolve-failure-"+key+".bin",size);};
    ec.state=payload("state",13860);ec.destinationHeader=payload("destination-header",52);ec.surfaces.emplace();
    for(unsigned i=0;i<5;++i)ec.surfaces->surfaces[i].object=be(*ec.state,12816+4*i);
    ec.surfaces->surfaces[0].header=payload("surface-0",40);
    ec.pointed.rectangle=payload("rectangle",16);ec.pointed.stack92=payload("stack92",4);ec.pointed.stack100=payload("stack100",4);
    const auto cp=planNativeExposureResolve(ec);assert(cp);
    assert(cp->sourceObject==eb.surfaces[0].object && ec.surfaces->surfaces[0].header==eb.surfaces[0].header);
    constexpr auto model=NativeEdram64Model::Xenia40x16;
    const std::vector<std::uint8_t> previous(cp->storage.sourceBytes,0xA7);assert(previous.size()==8192);
    auto destination=previous,expected=previous;
    for(unsigned i=0;i<8;++i)expected[i^1]=(*actual)[i]; // Independent pixel0 address and8in16 permutation.
    unsigned stores=0;const auto serial=eowner.nextSerial();
    const auto publish=[&](const auto& bytes){++stores;
        const NativeSerializedResourceUpdate u{cp->destination.baseAddress,{destination.data(),destination.size()},&previous,&bytes};
        return publishNativeSerializedResourceBatch(&u,1);};
    assert(publishNativeExposureResolve(ec,eowner,previous,model,[&]{return sameNativeExposureResolveInputs(ec,ec);},publish));
    assert(stores==1 && destination==expected && std::equal(destination.begin()+8,destination.end(),previous.begin()+8));
    assert(eowner.nextSerial()==serial && eowner.readColorWords(720,1280,1280,720)==std::optional{eexpected});
    assert(eowner.readColorWords(1440,80,80,80)==expected4);
    assert(linearizeXenosExpanded16FloatStorage(cp->destination,destination)==actual);
    // Existing explicit EXPAND storage view imports the original binary16
    // bytes as RGBA16F. This tests current Vulkan import+sampling, not the next
    // game's not-yet-captured shader or its selected sampler.
    auto copyImage=NativeTextureImage::create(device,cp->storage.description,destination);
    assert(copyImage && copyImage->readback()==actual);
    SampleReader reader(device,1,1,1,copyImage->image(),VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,
        (p+"/r32-copy/sample.comp.spv").c_str(),5,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,copyImage->view());
    const auto samples=reader.readWords();assert(samples.size()==5 && samples[4]==0x53414D50);
    for(unsigned ch=0;ch<4;++ch) {
        const unsigned sw=cp->destination.swizzle[ch];
        const float value=sw<4?half((*actual)[sw*2]|(unsigned{(*actual)[sw*2+1]}<<8)):sw==4?0.f:1.f;
        assert(samples[ch]==colorBits(value));
    }
#ifdef RECOMP_PRIVATE_BRIGHT_PASS
#include "bright_pass_connection.inc"
#endif
    // The fully populated scene is not a prerequisite: ONLY two known words
    // are sufficient, and copying them must not make unknown padding readable.
    assert(packNativeExposureResolve(ec,empty,previous,model)==std::optional{expected});
    assert(!empty.readKnownColorRows(720,160,3,1));
    auto missing=eowner;assert(missing.invalidate(720,1));
    assert(!publishNativeExposureResolve(ec,missing,previous,model,[]{return true;},publish) && stores==1);
    assert(!publishNativeExposureResolve(ec,eowner,previous,model,[]{return false;},publish) && stores==1);
    destination=previous;destination.back()^=1;const auto stale=destination;
    assert(!publishNativeExposureResolve(ec,eowner,previous,model,[]{return true;},publish));assert(stores==2 && destination==stale);
    std::printf("EXPOSURE_COPY_CONNECTION_PASS variant=%u original_gpu_producer=1 actual_copy=1 rgba16f_to_expand29=1 bitwise=1 two_words=1 endian=1 float_gpu_import=1 sampled_swizzle=1 padding_scene_scratch_preserved=1 unknown_stale_refused=1 live_copy=0 gameplay=0\n",variant);
}
