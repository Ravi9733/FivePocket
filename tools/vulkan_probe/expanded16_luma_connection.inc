// Included after the actual max->320copy connection. No guest publication.
{
    NativeTranslatedDrawProgram lumaProgram;lumaProgram.abiVersion=37;
    lumaProgram.stride=24;lumaProgram.interpolators=1;lumaProgram.textureSlot=0;lumaProgram.textureSlots={0};
    lumaProgram.vertexIdentity=read(p+"/luma/vertex-shader.bin",96);
    lumaProgram.pixelIdentity=read(p+"/luma/pixel-shader.bin",324);
    lumaProgram.vertexCode=code(p+"/luma/original.vert.spv");lumaProgram.pixelCode=code(p+"/luma/original.frag.spv");
    assert(validateNativeExpanded16LumaProgram(lumaProgram));
    Gta5InlineDraw luma;luma.primitive=8;luma.vertexCount=3;luma.stride=24;
    luma.deviceState=read(p+"/luma/device-state.bin",13860);luma.vertices=read(p+"/luma/vertices.bin",72);
    luma.vertexShader=be(luma.deviceState,13072);luma.pixelShader=be(luma.deviceState,13068);
    const auto lm=read(p+"/luma/capture-meta.bin",36);
    luma.scissorRegisters=std::array<unsigned,3>{be(lm,24),be(lm,28),be(lm,32)};
    Gta5InlineDraw::Texture lt;lt.slot=0;for(unsigned i=0;i<6;++i)lt.fetch[i]=be(lm,i*4);
    lt.source=destinationSmall;luma.textures={lt};
    assert((lt.fetch[1]&0xFFFFF000)==(smallPlan->destination.baseAddress-0xA0000000u));
    Gta5BoundSurfaceCapture lb;for(unsigned i=0;i<5;++i)lb.surfaces[i].object=be(luma.deviceState,12816+4*i);
    lb.surfaces[0].header=read(p+"/luma/surface-0-resource.bin",40);
    const auto lvc=read(p+"/luma/vertex-constants.bin",4096),lpc=read(p+"/luma/pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){luma.vertexConstants[i]=be(lvc,i*4);luma.pixelConstants[i]=be(lpc,i*4);}
    // Counterfactual constants, not a second live capture: prove runtime inputs
    // reach both the conditionals and the scalar accumulator without baking.
    if(variant){luma.pixelConstants[18*4+1]=colorBits(.75f);luma.pixelConstants[254*4]=colorBits(1.f);
        luma.pixelConstants[254*4+1]=0;luma.pixelConstants[254*4+2]=colorBits(.25f);}
    const auto lp=planNativeExpanded16LumaDraw(luma,lb,lumaProgram,lumaProgram.vertexIdentity,lumaProgram.pixelIdentity,false,true);
    assert(lp && lp->storage.sourceBytes==destinationSmall.size());
    auto lopts=maxOptions;lopts.externalTextures={{sampler.s,smallTexture->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    for(unsigned i=0;i<7;++i) {
        auto bad=lumaProgram;auto opt=lopts;auto desc=lp->storage.description;
        if(i==0)bad.abiVersion=36;if(i==1)bad.textureSlot=5;if(i==2)opt.samples=2;
        if(i==3)desc.format=29;if(i==4)opt.externalTextures.clear();if(i==5)bad.interpolators=2;
        if(i==6)bad.pixelIdentity.resize(120);
        NativeTranslatedResources resources(device.device);
        assert(!resources.setup(device,VK_NULL_HANDLE,bad,{desc},NativeTranslatedBlend::Replace,opt));
    }
    std::vector<std::uint8_t> li(8208+72);li[3]=3;
    const auto set=[&](unsigned at,unsigned v){for(unsigned c=0;c<4;++c)li[at+c]=v>>(24-c*8);};
    for(unsigned i=0;i<1024;++i){set(16+i*4,luma.vertexConstants[i]);set(4112+i*4,luma.pixelConstants[i]);}
    std::copy(luma.vertices.begin(),luma.vertices.end(),li.begin()+8208);
    auto target=NativeLinearSampleTarget::create(device,64,64,1);assert(target);
    auto uvp=lumaProgram;uvp.pixelCode=code(p+"/luma/coordinates.frag.spv");
    const auto uv=target->draw(uvp,li,6,lp->viewport,{lp->storage.description},lopts,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);assert(uv);
    const auto result=target->draw(lumaProgram,li,6,lp->viewport,{lp->storage.description},lopts,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);assert(result);
    const auto pc=[&](unsigned r,unsigned c){return colorFloat(luma.pixelConstants[r*4+c]);};
    const auto mul=[](float a,float b){volatile float r=a*b;return float(r);};
    const auto add=[](float a,float b){volatile float r=a+b;return float(r);};
    const float scale=pc(18,1),a=pc(254,0)==0?scale:pc(255,3),b=pc(254,1)==0?scale:pc(255,3);
    constexpr float offsets[8][2]{{1.5,.5},{.5,-.5},{1.5,-.5},{-.5,-.5},{.5,.5},{-1.5,-.5},{-.5,.5},{-1.5,.5}};
    unsigned errors=0;double maxError=0;
    NativeEdram32Bands lumaOwner=maxOwner;
    const std::vector<std::uint8_t> initial(80*80*4,0xA6);
    assert(lumaOwner.recordCompletedRawColorRows(1440,80,80,80,initial,lumaOwner.nextSerial()));
    auto parent=lumaOwner.readColorWords(1440,80,80,80);assert(parent);
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        const unsigned at=(y*64+x)*4;const float u=(*uv)[at],v=(*uv)[at+1];
        assert(u>=0 && u<1 && v>=0 && v<1 && (*uv)[at+2]==0 && (*uv)[at+3]==1);
        const auto sum=[&](unsigned tap,float factor) {
            const auto sx=std::clamp(int(std::floor(mul(add(u,(offsets[tap][0]+1.5f/1024)/320),320))),0,319);
            const auto sy=std::clamp(int(std::floor(mul(add(v,(offsets[tap][1]+1.5f/1024)/180),180))),0,179);
            const auto channel=[&](unsigned c){const auto offset=(sy*320+sx)*8+c*2;
                const std::uint16_t h=expectedSmallLinear[offset]|(unsigned{expectedSmallLinear[offset+1]}<<8);
                _Float16 f;std::memcpy(&f,&h,2);return float(f);};
            return add(add(mul(mul(channel(2),pc(255,2)),factor),mul(mul(channel(0),pc(255,0)),factor)),mul(mul(channel(1),pc(255,1)),factor));
        };
        float expected=sum(0,scale);for(unsigned tap:{2u,1u,4u,3u,6u,5u,7u})expected=add(sum(tap,a),mul(expected,b));
        expected=mul(expected,pc(254,2));
        for(unsigned c=0;c<4;++c){const double error=std::abs(double((*result)[at+c])-expected);maxError=std::max(error,maxError);
            if(!std::isfinite((*result)[at+c]) || error>2e-5*std::max(1.,std::abs(double(expected))))++errors;}
        (*parent)[y*80+x]=colorBits((*result)[at]);
    }
    std::printf("LUMA_COMPARE variant=%u mismatches=%u max_absolute=%.9g\n",variant,errors,maxError);assert(!errors);
    const auto serial=lumaOwner.nextSerial();const char* why=nullptr;
    {NativeEdram32Mutation transaction(lumaOwner);
     const bool done=executeNativeExpanded16LumaDraw(device,lumaProgram,luma,lb,lumaProgram.vertexIdentity,lumaProgram.pixelIdentity,true,transaction,&why);
     if(!done)std::printf("LUMA_EXECUTOR_REFUSED %s\n",why);assert(done);}
    assert(lumaOwner.nextSerial()==serial+1 && lumaOwner.readColorWords(1440,80,80,80)==parent);
    assert(lumaOwner.readColorWords(720,1280,1280,720)==std::optional{parentExpected});
    assert(luma.textures[0].source==destinationSmall && destinationSmall==expectedSmall);
    NativeEdram32Bands pending;
    {NativeEdram32Mutation transaction(pending);assert(!executeNativeExpanded16LumaDraw(device,lumaProgram,luma,lb,lumaProgram.vertexIdentity,lumaProgram.pixelIdentity,false,transaction));}
    assert(!pending.knownTiles());
    std::printf("MAX_COPY_LUMA_CONNECTION_PASS variant=%u actual_source_binding=1 original_shader=1 runtime_constants=1 pixels=4096 r32_physical=1 parent_padding_preserved=1 source_preserved=1 pending_query_refused=1 next_copy=0 live_completion=0\n",variant);
#ifdef RECOMP_PRIVATE_LUMA_R32_COPY
#include "luma_r32_copy_connection.inc"
#endif
}
