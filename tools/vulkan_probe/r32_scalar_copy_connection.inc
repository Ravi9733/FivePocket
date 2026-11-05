// Connected to the just-executed original scalar shader, not captured output.
{
    const auto call=read(p+"/scalar-copy/call.bin",48);Gta5TileResolveCapture c16;
    for(unsigned i=0;i<9;++i)c16.arguments[i]=be(call,4*i);
    c16.stack=be(call,36);c16.f1=(std::uint64_t{be(call,40)}<<32)|be(call,44);
    const auto payload=[&](const char* key,unsigned size){return read(p+"/scalar-copy/native-resolve-failure-"+key+".bin",size);};
    c16.state=payload("state",13860);c16.destinationHeader=payload("destination-header",52);c16.surfaces.emplace();
    for(unsigned i=0;i<5;++i)c16.surfaces->surfaces[i].object=be(*c16.state,12816+4*i);
    c16.surfaces->surfaces[0].header=payload("surface-0",40);
    c16.pointed.rectangle=payload("rectangle",16);c16.pointed.stack92=payload("stack92",4);c16.pointed.stack100=payload("stack100",4);
    const auto cp16=planNativeR32FloatResolve(c16);assert(cp16 && cp16->Width==16 && cp16->Height==16);
    assert(cp16->sourceObject==sb.surfaces[0].object && c16.surfaces->surfaces[0].header==sb.surfaces[0].header);
    const std::vector<std::uint8_t> prior16(cp16->storage.sourceBytes,0x97);
    auto destination16=prior16,expected16=prior16;std::vector<std::uint8_t> linear16(16*16*4);
    std::vector<bool> touched16(prior16.size());
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        // Independent bank/pipe formula with the captured32-texel pitch.
        const unsigned outer=((y/32)+x/32)*64,inner=(y/2%8)*8+x%8,v=(outer+inner)*4;
        const unsigned bank=y/16%2,pipe=(x/8%4)^((y/8%2)*2);
        const unsigned at=(v&15)|((y%2)*16)|((v&16)*2)|(pipe*64)|((v&224u)*8)|((v&~255u)*16)|(bank*2048);
        const unsigned value=colorBits((*scalar)[(y*16+x)*4]);
        for(unsigned b=0;b<4;++b) {
            const unsigned offset=(at+b)^3;assert(offset<expected16.size() && !touched16[offset]);touched16[offset]=true;
            expected16[offset]=value>>(b*8);linear16[(y*16+x)*4+b]=value>>(b*8);
        }
    }
    const auto priorSerial16=scalarOwner.nextSerial();unsigned stores16=0;
    const auto write16=[&](const auto& bytes) {
        ++stores16;const NativeSerializedResourceUpdate update{cp16->destination.baseAddress,{destination16.data(),destination16.size()},&prior16,&bytes};
        return publishNativeSerializedResourceBatch(&update,1);
    };
    assert(publishNativeR32FloatResolve(c16,scalarOwner,prior16,[&]{return sameNativeR32FloatInputs(c16,c16);},write16));
    assert(stores16==1 && destination16==expected16 && std::count(touched16.begin(),touched16.end(),true)==1024);
    assert(prior16.size()>1024); // Full tiled allocation includes preserved non-texel bytes.
    assert(scalarOwner.nextSerial()==priorSerial16 && scalarOwner.readColorWords(1440,80,80,80)==expectedParent);
    auto image16=NativeTextureImage::create(device,cp16->destination,destination16);assert(image16 && image16->readback()==std::optional{linear16});
    SampleReader sample16(device,16,16,1,image16->image(),VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,
        (p+"/r32-copy/sample.comp.spv").c_str(),5,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image16->view());
    const auto words16=sample16.readWords();assert(words16.size()==16*16*5);
    for(unsigned i=0;i<256;++i) {
        const unsigned value=colorBits((*scalar)[i*4]);
        for(unsigned channel=0;channel<4;++channel) {
            const unsigned select=cp16->destination.swizzle[channel];
            assert(words16[i*5+channel]==(select<4?value:select==4?0u:0x3F800000u));
        }
        assert(words16[i*5+4]==0x53414D50);
    }
#ifdef RECOMP_PRIVATE_R32_SCALAR4
#include "r32_scalar4_connection.inc"
#endif
    auto missing16=scalarOwner;assert(missing16.invalidate(1440,1));
    assert(!publishNativeR32FloatResolve(c16,missing16,prior16,[]{return true;},write16) && stores16==1);
    assert(!publishNativeR32FloatResolve(c16,scalarOwner,prior16,[]{return false;},write16) && stores16==1);
    destination16=prior16;destination16.back()^=1;const auto stale16=destination16;
    assert(!publishNativeR32FloatResolve(c16,scalarOwner,prior16,[]{return true;},write16) && stores16==2 && destination16==stale16);
    std::printf("R32_SCALAR_COPY_CONNECTION_PASS variant=%u original_gpu_producer=1 actual_copy=1 pixels=256 bitwise=1 independent_tiling=1 endian=2 float_gpu_import=1 sampled_swizzle=1 source_padding_preserved=1 destination_padding_preserved=1 unknown_stale_refused=1 guest_publication=0 live_copy=0\n",variant);
}
