// Actual copy operands, directly after the fenced original luma producer.
// Authored initial destination and owner padding, not captured live pixels.
{
    const auto call=read(p+"/r32-copy/call.bin",48);Gta5TileResolveCapture c;
    for(unsigned i=0;i<9;++i)c.arguments[i]=be(call,4*i);
    c.stack=be(call,36);c.f1=(std::uint64_t{be(call,40)}<<32)|be(call,44);
    const auto payload=[&](const char* key,unsigned size){return read(p+"/r32-copy/native-resolve-failure-"+key+".bin",size);};
    c.state=payload("state",13860);c.destinationHeader=payload("destination-header",52);c.surfaces.emplace();
    for(unsigned i=0;i<5;++i)c.surfaces->surfaces[i].object=be(*c.state,12816+4*i);
    c.surfaces->surfaces[0].header=payload("surface-0",40);
    c.pointed.rectangle=payload("rectangle",16);c.pointed.stack92=payload("stack92",4);c.pointed.stack100=payload("stack100",4);
    const auto cp=planNativeR32FloatResolve(c);assert(cp);
    assert(cp->sourceObject==lb.surfaces[0].object && c.surfaces->surfaces[0].header==lb.surfaces[0].header);
    const std::vector<std::uint8_t> prior(cp->storage.sourceBytes,0xA7);
    std::vector<std::uint8_t> destination=prior,expected=prior,linear(64*64*4);
    std::vector<bool> touched(prior.size());
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        const unsigned outer=((y/32)*2+x/32)*64,inner=(y/2%8)*8+x%8,v=(outer+inner)*4;
        const unsigned bank=y/16%2,pipe=(x/8%4)^((y/8%2)*2);
        const unsigned at=(v&15)|((y%2)*16)|((v&16)*2)|(pipe*64)|((v&224u)*8)|((v&~255u)*16)|(bank*2048);
        const unsigned value=colorBits((*result)[(y*64+x)*4]);
        for(unsigned b=0;b<4;++b) {
            const unsigned offset=(at+b)^3;assert(offset<expected.size() && !touched[offset]);touched[offset]=true;
            expected[offset]=value>>(b*8);linear[(y*64+x)*4+b]=value>>(b*8);
        }
    }
    const auto priorSerial=lumaOwner.nextSerial();unsigned stores=0;
    const auto write=[&](const auto& bytes) {
        ++stores;const NativeSerializedResourceUpdate update{cp->destination.baseAddress,{destination.data(),destination.size()},&prior,&bytes};
        return publishNativeSerializedResourceBatch(&update,1);
    };
    assert(publishNativeR32FloatResolve(c,lumaOwner,prior,[&]{return sameNativeR32FloatInputs(c,c);},write));
    assert(stores==1 && destination==expected && std::count(touched.begin(),touched.end(),true)==16384);
    assert(lumaOwner.nextSerial()==priorSerial && lumaOwner.readColorWords(1440,80,80,80)==parent);
    auto image=NativeTextureImage::create(device,cp->destination,destination);assert(image && image->readback()==std::optional{linear});
    SampleReader sampled(device,64,64,1,image->image(),VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,
        (p+"/r32-copy/sample.comp.spv").c_str(),5,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image->view());
    const auto words=sampled.readWords();assert(words.size()==64*64*5);
    for(unsigned i=0;i<64*64;++i) {
        const unsigned value=colorBits((*result)[i*4]);
        for(unsigned channel=0;channel<4;++channel) {
            const unsigned select=cp->destination.swizzle[channel];
            assert(words[i*5+channel]==(select<4?value:select==4?0u:0x3F800000u));
        }
        assert(words[i*5+4]==0x53414D50);
    }
#ifdef RECOMP_PRIVATE_R32_SCALAR
#include "r32_scalar_connection.inc"
#endif
    auto bad=lumaOwner;assert(bad.invalidate(1443,1));
    assert(!publishNativeR32FloatResolve(c,bad,prior,[]{return true;},write) && stores==1);
    assert(!publishNativeR32FloatResolve(c,lumaOwner,prior,[]{return false;},write) && stores==1);
    destination=prior;destination.back()^=1;const auto stale=destination;
    assert(!publishNativeR32FloatResolve(c,lumaOwner,prior,[]{return true;},write) && stores==2 && destination==stale);
    std::printf("LUMA_R32_COPY_CONNECTION_PASS variant=%u original_gpu_producer=1 actual_copy=1 pixels=4096 bitwise=1 independent_tiling=1 endian=2 float_gpu_import=1 sampled_swizzle=1 source_padding_preserved=1 unknown_stale_refused=1 guest_publication=0 live_copy=0\n",variant);
}
