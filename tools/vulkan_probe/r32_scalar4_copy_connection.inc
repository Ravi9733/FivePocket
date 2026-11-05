// Use the preceding original 4x4 GPU producer and this copy's actual operands.
{
    const auto call4=read(p+"/scalar4-copy/call.bin",48);Gta5TileResolveCapture c4;
    for(unsigned i=0;i<9;++i)c4.arguments[i]=be(call4,4*i);
    c4.stack=be(call4,36);c4.f1=(std::uint64_t{be(call4,40)}<<32)|be(call4,44);
    const auto payload4=[&](const char* key,unsigned size){return read(p+"/scalar4-copy/native-resolve-failure-"+key+".bin",size);};
    c4.state=payload4("state",13860);c4.destinationHeader=payload4("destination-header",52);c4.surfaces.emplace();
    for(unsigned i=0;i<5;++i)c4.surfaces->surfaces[i].object=be(*c4.state,12816+4*i);
    c4.surfaces->surfaces[0].header=payload4("surface-0",40);
    c4.pointed.rectangle=payload4("rectangle",16);c4.pointed.stack92=payload4("stack92",4);c4.pointed.stack100=payload4("stack100",4);
    const auto copy4=planNativeR32FloatResolve(c4);assert(copy4 && copy4->Width==4 && copy4->Height==4);
    assert(copy4->sourceObject==b4.surfaces[0].object && c4.surfaces->surfaces[0].header==b4.surfaces[0].header);
    const std::vector<std::uint8_t> prior4(copy4->storage.sourceBytes,0x93);
    auto destination4=prior4,expectedCopy4=prior4;std::vector<std::uint8_t> linear4(64);
    std::vector<bool> touched4(prior4.size());
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
        // Independent bank/pipe formula, captured 32-texel pitch, endian8in32.
        const unsigned outer=((y/32)+x/32)*64,inner=(y/2%8)*8+x%8,v=(outer+inner)*4;
        const unsigned bank=y/16%2,pipe=(x/8%4)^((y/8%2)*2);
        const unsigned at=(v&15)|((y%2)*16)|((v&16)*2)|(pipe*64)|((v&224u)*8)|((v&~255u)*16)|(bank*2048);
        const unsigned value=colorBits((*scalar4)[(y*4+x)*4]);
        for(unsigned b=0;b<4;++b) {
            const unsigned offset=(at+b)^3;assert(offset<expectedCopy4.size() && !touched4[offset]);touched4[offset]=true;
            expectedCopy4[offset]=value>>(b*8);linear4[(y*4+x)*4+b]=value>>(b*8);
        }
    }
    const auto priorSerial4=owner4.nextSerial();unsigned stores4=0;
    const auto write4=[&](const auto& bytes) {
        ++stores4;const NativeSerializedResourceUpdate update{copy4->destination.baseAddress,{destination4.data(),destination4.size()},&prior4,&bytes};
        return publishNativeSerializedResourceBatch(&update,1);
    };
    assert(publishNativeR32FloatResolve(c4,owner4,prior4,[&]{return sameNativeR32FloatInputs(c4,c4);},write4));
    assert(stores4==1 && destination4==expectedCopy4 && std::count(touched4.begin(),touched4.end(),true)==64);
    assert(prior4.size()==4096); // The other 4032 bytes remain unchanged.
    assert(owner4.nextSerial()==priorSerial4 && owner4.readColorWords(1440,80,80,80)==expected4);
    assert(owner4.readColorWords(720,1280,1280,720)==std::optional{parentExpected});
    auto image4=NativeTextureImage::create(device,copy4->destination,destination4);assert(image4 && image4->readback()==std::optional{linear4});
    SampleReader sample4(device,4,4,1,image4->image(),VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,
        (p+"/r32-copy/sample.comp.spv").c_str(),5,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image4->view());
    const auto words4=sample4.readWords();assert(words4.size()==16*5);
    for(unsigned i=0;i<16;++i) {
        const unsigned value=colorBits((*scalar4)[i*4]);
        for(unsigned channel=0;channel<4;++channel) {
            const unsigned select=copy4->destination.swizzle[channel];
            assert(words4[i*5+channel]==(select<4?value:select==4?0u:0x3F800000u));
        }
        assert(words4[i*5+4]==0x53414D50);
    }
#ifdef RECOMP_PRIVATE_R32_SCALAR1
#include "r32_scalar1_connection.inc"
#endif
    auto missing4=owner4;assert(missing4.invalidate(1440,1));
    assert(!publishNativeR32FloatResolve(c4,missing4,prior4,[]{return true;},write4) && stores4==1);
    assert(!publishNativeR32FloatResolve(c4,owner4,prior4,[]{return false;},write4) && stores4==1);
    destination4=prior4;destination4.back()^=1;const auto stale4=destination4;
    assert(!publishNativeR32FloatResolve(c4,owner4,prior4,[]{return true;},write4) && stores4==2 && destination4==stale4);
    std::printf("R32_SCALAR4_COPY_CONNECTION_PASS variant=%u original_gpu_producer=1 actual_copy=1 pixels=16 bitwise=1 independent_tiling=1 endian=2 float_gpu_import=1 sampled_swizzle=1 source_padding_preserved=1 destination_padding_preserved=1 unknown_stale_refused=1 guest_publication=0 live_copy=0\n",variant);
}
