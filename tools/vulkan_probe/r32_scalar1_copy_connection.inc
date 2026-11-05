// Actual1copy consumes the completed1scalar owner at720, not old scratch1440.
{
    const auto call1=read(p+"/scalar1-copy/call.bin",48);Gta5TileResolveCapture c1;
    for(unsigned i=0;i<9;++i)c1.arguments[i]=be(call1,4*i);
    c1.stack=be(call1,36);c1.f1=(std::uint64_t{be(call1,40)}<<32)|be(call1,44);
    const auto payload1=[&](const char* key,unsigned size){return read(p+"/scalar1-copy/native-resolve-failure-"+key+".bin",size);};
    c1.state=payload1("state",13860);c1.destinationHeader=payload1("destination-header",52);c1.surfaces.emplace();
    for(unsigned i=0;i<5;++i)c1.surfaces->surfaces[i].object=be(*c1.state,12816+4*i);
    c1.surfaces->surfaces[0].header=payload1("surface-0",40);
    c1.pointed.rectangle=payload1("rectangle",16);c1.pointed.stack92=payload1("stack92",4);c1.pointed.stack100=payload1("stack100",4);
    const auto copy1=planNativeR32FloatResolve(c1);
    assert(copy1 && copy1->Width==1 && copy1->Height==1 && copy1->Base==720);
    assert(copy1->sourceObject==b1.surfaces[0].object && c1.surfaces->surfaces[0].header==b1.surfaces[0].header);
    const std::vector<std::uint8_t> prior1(copy1->storage.sourceBytes,0xA7);
    assert(prior1.size()==4096);
    auto destination1=prior1,expectedCopy1=prior1;std::vector<std::uint8_t> linear1(4);
    const unsigned copied1=colorBits((*scalar1)[0]);
    // Independent tiled address: pixel(0,0) is byte0;8in32 reverses four bytes.
    for(unsigned b=0;b<4;++b){expectedCopy1[b^3]=copied1>>(b*8);linear1[b]=copied1>>(b*8);}
    const auto copySerial1=owner1.nextSerial();unsigned stores1=0;
    const auto write1=[&](const auto& bytes) {
        ++stores1;const NativeSerializedResourceUpdate update{copy1->destination.baseAddress,{destination1.data(),destination1.size()},&prior1,&bytes};
        return publishNativeSerializedResourceBatch(&update,1);
    };
    assert(publishNativeR32FloatResolve(c1,owner1,prior1,[&]{return sameNativeR32FloatInputs(c1,c1);},write1));
    assert(stores1==1 && destination1==expectedCopy1);
    assert(std::equal(destination1.begin()+4,destination1.end(),prior1.begin()+4));
    assert(owner1.nextSerial()==copySerial1 && owner1.readColorWords(720,1280,1280,720)==std::optional{expected1});
    assert(owner1.readColorWords(1440,80,80,80)==expected4);
    auto image1=NativeTextureImage::create(device,copy1->destination,destination1);assert(image1 && image1->readback()==std::optional{linear1});
    SampleReader sampleCopy1(device,1,1,1,image1->image(),VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,
        (p+"/r32-copy/sample.comp.spv").c_str(),5,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image1->view());
    const auto words1=sampleCopy1.readWords();assert(words1.size()==5);
    for(unsigned channel=0;channel<4;++channel) {
        const unsigned select=copy1->destination.swizzle[channel];
        assert(words1[channel]==(select<4?copied1:select==4?0u:0x3F800000u));
    }
    assert(words1[4]==0x53414D50);
    auto missing1=owner1;assert(missing1.invalidate(720,1));
    assert(missing1.readColorWords(1440,80,80,80)==expected4); // Wrong scratch is present but insufficient.
    assert(!publishNativeR32FloatResolve(c1,missing1,prior1,[]{return true;},write1) && stores1==1);
    assert(!publishNativeR32FloatResolve(c1,owner1,prior1,[]{return false;},write1) && stores1==1);
    destination1=prior1;destination1.back()^=1;const auto stale1=destination1;
    assert(!publishNativeR32FloatResolve(c1,owner1,prior1,[]{return true;},write1) && stores1==2 && destination1==stale1);
#ifdef RECOMP_PRIVATE_EXPOSURE_SCALAR
#include "exposure_scalar_connection.inc"
#endif
    std::printf("R32_SCALAR1_COPY_CONNECTION_PASS variant=%u original_gpu_producer=1 actual_copy=1 pixels=1 source_base=720 bitwise=1 independent_tiling=1 endian=2 float_gpu_import=1 sampled_swizzle=1 scene_scratch_preserved=1 destination_padding_preserved=1 wrong_base_unknown_stale_refused=1 guest_publication=0 live_copy=0\n",variant);
}
