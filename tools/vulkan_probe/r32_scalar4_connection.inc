// Same original bytecode, actual next binding; input is the preceding GPU copy.
{
    assert(read(p+"/scalar4/vertex-shader.bin",96)==sp.vertexIdentity);
    assert(read(p+"/scalar4/pixel-shader.bin",444)==sp.pixelIdentity);
    Gta5InlineDraw d4;d4.primitive=8;d4.vertexCount=3;d4.stride=24;
    d4.deviceState=read(p+"/scalar4/device-state.bin",13860);d4.vertices=read(p+"/scalar4/vertices.bin",72);
    d4.vertexShader=be(d4.deviceState,13072);d4.pixelShader=be(d4.deviceState,13068);
    const auto m4=read(p+"/scalar4/capture-meta.bin",36);
    d4.scissorRegisters=std::array<unsigned,3>{be(m4,24),be(m4,28),be(m4,32)};
    Gta5InlineDraw::Texture t4;t4.slot=0;for(unsigned i=0;i<6;++i)t4.fetch[i]=be(m4,i*4);
    t4.source=destination16;d4.textures={t4};
    assert((t4.fetch[1]&0xFFFFF000)==cp16->destination.baseAddress-0xA0000000u);
    Gta5BoundSurfaceCapture b4;for(unsigned i=0;i<5;++i)b4.surfaces[i].object=be(d4.deviceState,12816+i*4);
    b4.surfaces[0].header=read(p+"/scalar4/surface-0-resource.bin",40);
    const auto vc4=read(p+"/scalar4/vertex-constants.bin",4096),pc4=read(p+"/scalar4/pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){d4.vertexConstants[i]=be(vc4,i*4);d4.pixelConstants[i]=be(pc4,i*4);}
    if(variant)d4.pixelConstants[255*4]=colorBits(.125f); // Explicit counterfactual scale.
    const auto plan4=planNativeR32ScalarDraw(d4,b4,sp,sp.vertexIdentity,sp.pixelIdentity,false,true);
    assert(plan4 && plan4->target.width==4 && plan4->target.height==4 && plan4->storage.sourceBytes==4096);
    auto options4=opt;options4.externalTextures={{sampler.s,image16->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::vector<std::uint8_t> input4(8208+72);input4[3]=3;
    const auto put4=[&](unsigned at,unsigned v){for(unsigned i=0;i<4;++i)input4[at+i]=v>>(24-i*8);};
    for(unsigned i=0;i<1024;++i){put4(16+i*4,d4.vertexConstants[i]);put4(4112+i*4,d4.pixelConstants[i]);}
    std::copy(d4.vertices.begin(),d4.vertices.end(),input4.begin()+8208);
    auto target4=NativeLinearSampleTarget::create(device,4,4,1);assert(target4);
    const auto uv4=target4->draw(uvProgram,input4,6,plan4->viewport,{plan4->submitted},options4,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);assert(uv4);
    const auto scalar4=target4->draw(sp,input4,6,plan4->viewport,{plan4->submitted},options4,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);assert(scalar4);
    auto owner4=scalarOwner;auto expected4=expectedParent;assert(expected4);
    unsigned errors4=0;double maxError4=0;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
        const unsigned at=(y*4+x)*4;const float u=(*uv4)[at],v=(*uv4)[at+1];
        assert(u>=0 && u<1 && v>=0 && v<1 && (*uv4)[at+2]==0 && (*uv4)[at+3]==1);
        const auto sample4=[&](unsigned tap) {
            const auto sx=std::clamp(int(std::floor(mul(add(u,(positions[tap][0]+1.5f/1024)/16),16))),0,15);
            const auto sy=std::clamp(int(std::floor(mul(add(v,(positions[tap][1]+1.5f/1024)/16),16))),0,15);
            return (*scalar)[(sy*16+sx)*4];
        };
        float value=add(sample4(14),sample4(15));for(int tap=13;tap>=0;--tap)value=add(value,sample4(unsigned(tap)));
        value=mul(value,colorFloat(d4.pixelConstants[255*4]));
        for(unsigned c=0;c<4;++c){const auto error=std::abs(double((*scalar4)[at+c])-value);maxError4=std::max(maxError4,error);
            if(!std::isfinite((*scalar4)[at+c]) || error>2e-6*std::max(1.,std::abs(double(value))))++errors4;}
        (*expected4)[y*80+x]=colorBits((*scalar4)[at]);
    }
    std::printf("R32_SCALAR4_COMPARE variant=%u mismatches=%u max_absolute=%.9g\n",variant,errors4,maxError4);assert(!errors4);
    const auto serial4=owner4.nextSerial();const char* why4=nullptr;
    {NativeEdram32Mutation tx(owner4);
     const bool done=executeNativeR32ScalarDraw(device,sp,d4,b4,sp.vertexIdentity,sp.pixelIdentity,true,tx,&why4);
     if(!done)std::printf("R32_SCALAR4_EXECUTOR_REFUSED %s\n",why4);assert(done);}
    assert(owner4.nextSerial()==serial4+1 && owner4.readColorWords(1440,80,80,80)==expected4);
    assert(owner4.readColorWords(720,1280,1280,720)==std::optional{parentExpected});
    assert(scalarOwner.readColorWords(1440,80,80,80)==expectedParent && d4.textures[0].source==destination16);
    {auto wrong=d4;wrong.textures[0]=st;assert(!planNativeR32ScalarDraw(wrong,b4,sp,sp.vertexIdentity,sp.pixelIdentity,false,true));}
    {auto wrong=d4;wrong.scissorRegisters=sd.scissorRegisters;assert(!planNativeR32ScalarDraw(wrong,b4,sp,sp.vertexIdentity,sp.pixelIdentity,false,true));}
    NativeEdram32Bands pending4;
    {NativeEdram32Mutation tx(pending4);assert(!executeNativeR32ScalarDraw(device,sp,d4,b4,sp.vertexIdentity,sp.pixelIdentity,false,tx));}
    assert(!pending4.knownTiles());
#ifdef RECOMP_PRIVATE_R32_SCALAR4_COPY
#include "r32_scalar4_copy_connection.inc"
#endif
    std::printf("R32_SCALAR4_CONNECTION_PASS variant=%u actual_binding=1 same_original_shader=1 current16copy_source=1 numeric_r32=1 runtime_scale=1 pixels=16 original_sum_order=1 physical_r32=1 parent_padding_preserved=1 source_preserved=1 mixed_profiles_refused=1 pending_query_refused=1 next_copy=0 live_completion=0\n",variant);
}
