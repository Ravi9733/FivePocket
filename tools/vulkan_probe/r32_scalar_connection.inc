// Inside the R32 copy connection: consume its derived pixels, not a live replay.
{
    NativeTranslatedDrawProgram sp;sp.abiVersion=38;sp.stride=24;sp.interpolators=1;
    sp.textureSlot=0;sp.textureSlots={0};sp.vertexIdentity=read(p+"/scalar/vertex-shader.bin",96);
    sp.pixelIdentity=read(p+"/scalar/pixel-shader.bin",444);
    sp.vertexCode=code(p+"/scalar/original.vert.spv");sp.pixelCode=code(p+"/scalar/original.frag.spv");
    assert(validateNativeR32ScalarProgram(sp));
    Gta5InlineDraw sd;sd.primitive=8;sd.vertexCount=3;sd.stride=24;
    sd.deviceState=read(p+"/scalar/device-state.bin",13860);sd.vertices=read(p+"/scalar/vertices.bin",72);
    sd.vertexShader=be(sd.deviceState,13072);sd.pixelShader=be(sd.deviceState,13068);
    const auto sm=read(p+"/scalar/capture-meta.bin",36);
    sd.scissorRegisters=std::array<unsigned,3>{be(sm,24),be(sm,28),be(sm,32)};
    Gta5InlineDraw::Texture st;st.slot=0;for(unsigned i=0;i<6;++i)st.fetch[i]=be(sm,i*4);
    st.source=destination;sd.textures={st};
    assert((st.fetch[1]&0xFFFFF000)==(cp->destination.baseAddress-0xA0000000u));
    Gta5BoundSurfaceCapture sb;for(unsigned i=0;i<5;++i)sb.surfaces[i].object=be(sd.deviceState,12816+4*i);
    sb.surfaces[0].header=read(p+"/scalar/surface-0-resource.bin",40);
    const auto svc=read(p+"/scalar/vertex-constants.bin",4096),spc=read(p+"/scalar/pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){sd.vertexConstants[i]=be(svc,i*4);sd.pixelConstants[i]=be(spc,i*4);}
    // Variant1 is explicitly counterfactual, proving the runtime scale is read.
    if(variant)sd.pixelConstants[255*4]=colorBits(.125f);
    const auto plan=planNativeR32ScalarDraw(sd,sb,sp,sp.vertexIdentity,sp.pixelIdentity,false,true);
    assert(plan && plan->storage.sourceBytes==destination.size() && plan->submitted.format==36);
    assert(plan->storage.description.format==6); // Address view must NOT reach the sampler.
    auto opt=maxOptions;opt.externalTextures={{sampler.s,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    for(unsigned i=0;i<7;++i) {
        auto bad=sp;auto options=opt;auto desc=plan->submitted;
        if(i==0)bad.abiVersion=37;if(i==1)bad.textureSlot=5;if(i==2)options.samples=2;
        if(i==3)desc=plan->storage.description;if(i==4)options.externalTextures.clear();
        if(i==5)bad.interpolators=2;if(i==6)bad.pixelIdentity.resize(324);
        NativeTranslatedResources resources(device.device);
        assert(!resources.setup(device,VK_NULL_HANDLE,bad,{desc},NativeTranslatedBlend::Replace,options));
    }
    std::vector<std::uint8_t> si(8208+72);si[3]=3;
    const auto setWord=[&](unsigned at,unsigned v){for(unsigned c=0;c<4;++c)si[at+c]=v>>(24-c*8);};
    for(unsigned i=0;i<1024;++i){setWord(16+i*4,sd.vertexConstants[i]);setWord(4112+i*4,sd.pixelConstants[i]);}
    std::copy(sd.vertices.begin(),sd.vertices.end(),si.begin()+8208);
    auto target=NativeLinearSampleTarget::create(device,16,16,1);assert(target);
    auto uvProgram=sp;uvProgram.pixelCode=code(p+"/scalar/coordinates.frag.spv");
    const auto uv=target->draw(uvProgram,si,6,plan->viewport,{plan->submitted},opt,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);assert(uv);
    const auto scalar=target->draw(sp,si,6,plan->viewport,{plan->submitted},opt,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);assert(scalar);
    constexpr float positions[16][2]{{-1.5,1.5},{-1.5,.5},{-1.5,-.5},{-1.5,-1.5},
        {-.5,1.5},{-.5,.5},{-.5,-.5},{-.5,-1.5},{.5,1.5},{.5,.5},{.5,-.5},{.5,-1.5},
        {1.5,1.5},{1.5,.5},{1.5,-1.5},{1.5,-.5}};
    auto scalarOwner=lumaOwner;auto expectedParent=parent;assert(expectedParent);
    unsigned errors=0;double maxError=0;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned at=(y*16+x)*4;const float u=(*uv)[at],v=(*uv)[at+1];
        assert(u>=0 && u<1 && v>=0 && v<1 && (*uv)[at+2]==0 && (*uv)[at+3]==1);
        const auto sample=[&](unsigned tap){
            const auto sx=std::clamp(int(std::floor(mul(add(u,(positions[tap][0]+1.5f/1024)/64),64))),0,63);
            const auto sy=std::clamp(int(std::floor(mul(add(v,(positions[tap][1]+1.5f/1024)/64),64))),0,63);
            return (*result)[(sy*64+sx)*4];
        };
        // Actual original dataflow: (tap14+tap15), then13 down to0, then scale.
        float expected=add(sample(14),sample(15));
        for(int tap=13;tap>=0;--tap)expected=add(expected,sample(unsigned(tap)));
        expected=mul(expected,colorFloat(sd.pixelConstants[255*4]));
        for(unsigned c=0;c<4;++c){const double error=std::abs(double((*scalar)[at+c])-expected);
            maxError=std::max(maxError,error);
            if(!std::isfinite((*scalar)[at+c]) || error>2e-6*std::max(1.,std::abs(double(expected))))++errors;}
        (*expectedParent)[y*80+x]=colorBits((*scalar)[at]);
    }
    std::printf("R32_SCALAR_COMPARE variant=%u mismatches=%u max_absolute=%.9g\n",variant,errors,maxError);assert(!errors);
    const auto serial=scalarOwner.nextSerial();const char* why=nullptr;
    {NativeEdram32Mutation tx(scalarOwner);
     const bool done=executeNativeR32ScalarDraw(device,sp,sd,sb,sp.vertexIdentity,sp.pixelIdentity,true,tx,&why);
     if(!done)std::printf("R32_SCALAR_EXECUTOR_REFUSED %s\n",why);assert(done);}
    assert(scalarOwner.nextSerial()==serial+1 && scalarOwner.readColorWords(1440,80,80,80)==expectedParent);
    assert(scalarOwner.readColorWords(720,1280,1280,720)==std::optional{parentExpected});
    assert(lumaOwner.readColorWords(1440,80,80,80)==parent && sd.textures[0].source==destination);
    NativeEdram32Bands pending;
    {NativeEdram32Mutation tx(pending);assert(!executeNativeR32ScalarDraw(device,sp,sd,sb,sp.vertexIdentity,sp.pixelIdentity,false,tx));}
    assert(!pending.knownTiles());
#ifdef RECOMP_PRIVATE_R32_SCALAR_COPY
#include "r32_scalar_copy_connection.inc"
#endif
    std::printf("R32_SCALAR_CONNECTION_PASS variant=%u actual_binding=1 original_shader=1 numeric_r32=1 runtime_scale=1 pixels=256 original_sum_order=1 physical_r32=1 parent_padding_preserved=1 source_preserved=1 pending_query_refused=1 next_copy=0 live_completion=0\n",variant);
}
