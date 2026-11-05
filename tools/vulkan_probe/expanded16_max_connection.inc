// Included inside the already-fenced producer/copy test. Authored initial
// owner, captured shaders and copy operands. This is not a live game draw.
{
    NativeTranslatedDrawProgram maxProgram;maxProgram.abiVersion=36;
    maxProgram.stride=24;maxProgram.interpolators=1;maxProgram.textureSlot=5;maxProgram.textureSlots={5};
    maxProgram.vertexIdentity=read(p+"/max/vertex-shader.bin",96);
    maxProgram.pixelIdentity=read(p+"/max/pixel-shader.bin",120);
    maxProgram.vertexCode=code(p+"/max/original.vert.spv");maxProgram.pixelCode=code(p+"/max/original.frag.spv");
    assert(validateNativeExpanded16MaxProgram(maxProgram) && texture->supportsLinearFiltering());
    Sampler linear{device.device};auto info=si;info.magFilter=info.minFilter=VK_FILTER_LINEAR;
    assert(vkCreateSampler(device.device,&info,nullptr,&linear.s)==VK_SUCCESS);
    auto maxOptions=options;maxOptions.externalTextures={{linear.s,texture->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    for(unsigned i=0;i<9;++i) {
        auto bad=maxProgram;auto opt=maxOptions;auto desc=resolvePlan->storage.description;
        if(i==0)bad.abiVersion=35;if(i==1)bad.stride=36;if(i==2)opt.samples=2;
        if(i==3)opt.depthStencil.reset();if(i==4)opt.colorAttachmentCount=2;
        if(i==5)desc.format=29;if(i==6)opt.externalTextures.clear();if(i==7)bad.interpolators=2;
        if(i==8)bad.vertexIdentity.resize(132);
        NativeTranslatedResources resources(device.device);
        assert(!resources.setup(device,VK_NULL_HANDLE,bad,{desc},NativeTranslatedBlend::Replace,opt));
    }
    Gta5InlineDraw next;next.deviceState=read(p+"/max/device-state.bin",13860);
    const auto viewport=nativeTranslatedViewport(next);assert(viewport);
    auto maxVertices=read(p+"/max/vertices.bin",72);
    std::vector<std::uint8_t> maxInput(8208+72);maxInput[3]=3;
    std::copy(maxVertices.begin(),maxVertices.end(),maxInput.begin()+8208);
    auto maxTarget=NativeLinearSampleTarget::create(device,320,180,1);assert(maxTarget);
    auto uvProgram=maxProgram;uvProgram.pixelCode=code(p+"/max/coordinates.frag.spv");
    const auto uv=maxTarget->draw(uvProgram,maxInput,6,*viewport,{resolvePlan->storage.description},maxOptions,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);
    if(!uv)std::printf("MAX_REDUCE_UV_REFUSED stage=%s\n",maxTarget->lastDrawFailure());assert(uv);
    const auto reduced=maxTarget->draw(maxProgram,maxInput,6,*viewport,{resolvePlan->storage.description},maxOptions,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);
    if(!reduced)std::printf("MAX_REDUCE_RASTER_REFUSED stage=%s\n",maxTarget->lastDrawFailure());assert(reduced);
    const auto channel=[&](int x,int y,unsigned c) {
        x=std::clamp(x,0,639);y=std::clamp(y,0,359);
        const auto at=(y*640+x)*8+c*2;
        const std::uint16_t h=expectedLinear[at]|(unsigned{expectedLinear[at+1]}<<8);
        _Float16 f;std::memcpy(&f,&h,2);return float(f);
    };
    // Independent8194-query precision probe measured floor8 weights. The batch
    // must rerun that probe BEFORE this connected check; a reported limit is
    // not sufficient evidence of a rounding policy on another driver.
    VkPhysicalDeviceProperties precisionProps{};vkGetPhysicalDeviceProperties(device.physical,&precisionProps);
    assert(precisionProps.limits.subTexelPrecisionBits==8);
    const auto mix=[](float a,float b,float t){volatile float x=a*(1-t),y=b*t;return float(x+y);};
    const auto sample=[&](float u,float v,float ox,float oy,unsigned c) {
        volatile float dx=(ox+1.5f/1024)/640,dy=(oy+1.5f/1024)/360;
        volatile float su=u+dx,sv=v+dy,px=su*640,py=sv*360;
        const float tx=px-.5f,ty=py-.5f;const int x=std::floor(tx),y=std::floor(ty);
        const float wx=std::floor((tx-x)*256)/256,wy=std::floor((ty-y)*256)/256;
        return mix(mix(channel(x,y,c),channel(x+1,y,c),wx),
                   mix(channel(x,y+1,c),channel(x+1,y+1,c),wx),wy);
    };
    unsigned mismatches=0;double errorMax=0;
    for(unsigned y=0;y<180;++y)for(unsigned x=0;x<320;++x) {
        const auto at=(y*320+x)*4;const auto u=(*uv)[at],v=(*uv)[at+1];
        assert(u>=0 && u<1 && v>=0 && v<1 && (*uv)[at+2]==0 && (*uv)[at+3]==1);
        for(unsigned c=0;c<4;++c) {
            const float expected=c==3?1:std::max({sample(u,v,.5f,.5f,c),sample(u,v,.5f,-.5f,c),
                sample(u,v,-.5f,-.5f,c),sample(u,v,-.5f,.5f,c)});
            const float actual=(*reduced)[at+c];const double error=std::abs(double(actual)-expected);
            errorMax=std::max(errorMax,error);
            if(error>2e-5*std::max(1.,std::abs(double(expected)))) {
                if(mismatches<8)std::printf("MAX_REDUCE_MISMATCH variant=%u x=%u y=%u c=%u actual=%.9g expected=%.9g error=%.9g\n",variant,x,y,c,actual,expected,error);
                ++mismatches;
            }
        }
    }
    std::printf("MAX_REDUCE_COMPARE variant=%u mismatches=%u max_absolute=%.9g measured_original_uv=1 weight_model=floor8 tolerance=2e-5\n",variant,mismatches,errorMax);
    assert(!mismatches && texture->readback()==std::optional{expectedLinear} && connected.nextSerial()==serial);
    const auto maxMeta=read(p+"/max/capture-meta.bin",36);
    next.primitive=8;next.vertexCount=3;next.stride=24;next.vertices=maxVertices;
    next.vertexShader=be(next.deviceState,13072);next.pixelShader=be(next.deviceState,13068);
    next.scissorRegisters=std::array<unsigned,3>{be(maxMeta,24),be(maxMeta,28),be(maxMeta,32)};
    Gta5InlineDraw::Texture maxTexture;maxTexture.slot=5;maxTexture.source=*completed;
    for(unsigned i=0;i<6;++i)maxTexture.fetch[i]=be(maxMeta,i*4);
    next.textures={maxTexture};Gta5BoundSurfaceCapture maxBound;
    for(unsigned i=0;i<5;++i)maxBound.surfaces[i].object=be(next.deviceState,12816+4*i);
    maxBound.surfaces[0].header=read(p+"/max/surface-0-resource.bin",40);
    const char* maxWhy=nullptr;
    const auto maxPlan=planNativeExpanded16MaxDraw(next,maxBound,maxProgram,
        maxProgram.vertexIdentity,maxProgram.pixelIdentity,false,true,&maxWhy);
    if(!maxPlan)std::printf("MAX_REDUCE_PLAN_REFUSED %s\n",maxWhy);assert(maxPlan);
    auto maxOwner=connected;
    auto parentBefore=connected.readColorWords(720,1280,1280,720);assert(parentBefore);
    auto parentExpected=*parentBefore;
    for(unsigned y=0;y<180;++y)for(unsigned x=0;x<320;++x) {
        std::array<unsigned,4> bits{};
        for(unsigned c=0;c<4;++c)bits[c]=colorBits((*reduced)[(y*320+x)*4+c]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound));
        const auto tile=(y/16)*4+x/80;
        parentExpected[((tile/16)*16+y%16)*1280+(tile%16)*80+x%80]=packed;
    }
    {NativeEdram32Mutation transaction(maxOwner);
     const bool complete=executeNativeExpanded16MaxDraw(device,maxProgram,next,maxBound,
        maxProgram.vertexIdentity,maxProgram.pixelIdentity,true,transaction,&maxWhy);
     if(!complete)std::printf("MAX_REDUCE_EXECUTOR_REFUSED %s\n",maxWhy);assert(complete);}
    assert(maxOwner.readColorWords(720,1280,1280,720)==std::optional{parentExpected});
    assert(maxOwner.nextSerial()==connected.nextSerial()+1 && next.textures[0].source==*completed);
    NativeEdram32Bands refused;
    {NativeEdram32Mutation transaction(refused);
     assert(!executeNativeExpanded16MaxDraw(device,maxProgram,next,maxBound,
        maxProgram.vertexIdentity,maxProgram.pixelIdentity,false,transaction));}
    assert(refused.nextSerial()==1 && !refused.knownTiles());
#ifdef RECOMP_PRIVATE_MAX_COPY
    // Actual stopped320x180 copy operands, connected to the REAL preceding
    // original shader/executor above. Initial destination padding is authored;
    // GPU import is verified, but the next game shader remains unproven.
    const auto smallMeta=read(p+"/max-copy/call.bin",48);
    Gta5TileResolveCapture small;
    for(unsigned i=0;i<9;++i)small.arguments[i]=be(smallMeta,i*4);
    small.stack=be(smallMeta,36);small.f1=(std::uint64_t{be(smallMeta,40)}<<32)|be(smallMeta,44);
    const auto field=[&](const char* name,unsigned bytes){return read(p+"/max-copy/native-resolve-failure-"+name+".bin",bytes);};
    small.state=field("state",13860);small.destinationHeader=field("destination-header",52);small.surfaces.emplace();
    for(unsigned i=0;i<5;++i)small.surfaces->surfaces[i].object=be(*small.state,12816+i*4);
    small.surfaces->surfaces[0].header=field("surface-0",40);
    small.pointed.rectangle=field("rectangle",16);small.pointed.stack92=field("stack92",4);small.pointed.stack100=field("stack100",4);
    const auto smallPlan=planNativeRgb7e3FloatResolve(small,&maxWhy);
    if(!smallPlan)std::printf("MAX_COPY_PLAN_REFUSED %s\n",maxWhy);assert(smallPlan);
    assert(smallPlan->width==320 && smallPlan->height==180 && smallPlan->pitch==320);
    assert(smallPlan->sourceObject==maxPlan->target.object);
    assert(*small.surfaces->surfaces[0].header==*maxBound.surfaces[0].header);
    const auto copySerial=maxOwner.nextSerial();
    const std::vector<std::uint8_t> previousSmall(smallPlan->storage.sourceBytes,0xA7);
    auto expectedSmall=previousSmall;std::vector<std::uint8_t> expectedSmallLinear(320*180*8);
    for(unsigned y=0;y<180;++y)for(unsigned x=0;x<320;++x) {
        const auto tile=(y/16)*4+x/80;
        const auto encoded=parentExpected[((tile/16)*16+y%16)*1280+(tile%16)*80+x%80];
        const auto floats=unpackColor7e3(encoded);
        for(unsigned c=0;c<4;++c) {
            const _Float16 reference=colorFloat(floats[c]);std::uint16_t h=0;std::memcpy(&h,&reference,2);
            for(unsigned byte=0;byte<2;++byte) {
                expectedSmall[(rgb7e3_test::offset(x,y,320)+c*2+byte)^1]=h>>(byte*8);
                expectedSmallLinear[(y*320+x)*8+c*2+byte]=h>>(byte*8);
            }
        }
    }
    auto destinationSmall=previousSmall;unsigned publications=0;
    const auto publishSmall=[&](const auto& bytes) {
        ++publications;if(destinationSmall!=previousSmall)return false;
        destinationSmall=bytes;return true;
    };
    assert(publishNativeRgb7e3FloatResolve(small,maxOwner,previousSmall,
        [&]{return sameNativeRgb7e3FloatInputs(small,small);},publishSmall,&maxWhy));
    assert(destinationSmall==expectedSmall && publications==1 && maxOwner.nextSerial()==copySerial);
    assert(maxOwner.readColorWords(720,1280,1280,720)==std::optional{parentExpected});
    auto smallTexture=NativeTextureImage::create(device,smallPlan->storage.description,destinationSmall);
    assert(smallTexture && smallTexture->readback()==std::optional{expectedSmallLinear});
    const auto preserved=destinationSmall;
    assert(!publishNativeRgb7e3FloatResolve(small,maxOwner,previousSmall,[]{return false;},publishSmall));
    assert(destinationSmall==preserved && publications==1 && next.textures[0].source==*completed);
    std::printf("MAX_TO_FLOAT_COPY_CONNECTION_PASS variant=%u pixels=57600 original_call=1 original_max_producer=1 current_physical=1 binary16=1 independent_tiling=1 padding_source_preserved=1 stale_refused=1 gpu_import=1 next_game_shader=0 live_publication=0\n",variant);
#ifdef RECOMP_PRIVATE_EXPANDED16_LUMA
#include "expanded16_luma_connection.inc"
#endif
#endif
    std::printf("MAX_REDUCE_EXECUTOR_PASS variant=%u exact_current_state=1 parent_words=921600 source_preserved=1 pending_query_refuses=1 live_completion=0\n",variant);
    std::printf("FILTER_COPY_MAX_REDUCE_PASS variant=%u pixels=57600 current_producer=1 original_copy=1 original_next_shaders=1 native_linear=1 source_preserved=1 live_completion=0 xbox_filter_conformance=0\n",variant);
}
