// Consume the completed GPU scalar/copy, not pixels from the live capture.
// expectedCopy1 retains the successful publication before the stale-write test.
{
    auto exposure=loadExposureScalarFixture(p+"/exposure",[](const std::string& path,unsigned bytes){return read(path,bytes);});
    auto& ed=exposure.draw;auto& eb=exposure.bound;auto& ep=exposure.program;
    ep.vertexCode=code(p+"/exposure/original.vert.spv");ep.pixelCode=code(p+"/exposure/original.frag.spv");
    assert(validateNativeExposureScalarProgram(ep));
    assert((ed.textures[0].fetch[1]&0xFFFFF000)==copy1->destination.baseAddress-0xA0000000u);
    ed.textures[0].source=expectedCopy1;
    const auto eplan=planNativeExposureScalarDraw(ed,eb,ep,ep.vertexIdentity,ep.pixelIdentity,false,true);assert(eplan);
    const auto constant=[&](unsigned c,unsigned lane){return colorFloat(ed.pixelConstants[c*4+lane]);};
    const float sourceValue=(*scalar1)[0];assert(sourceValue>0);
    // Independent scalar formula, in original float32 instruction order.
    const float logValue=std::log2(std::abs(sourceValue));
    const float power=std::exp2(mul(constant(193,1),logValue));
    const float raw=add(mul(power,constant(193,0)),add(constant(193,3),constant(193,2)));
    const float limited=std::min(std::max(raw,constant(194,0)),constant(194,1));
    const float sign=float(limited>-limited)-float(-limited>limited);
    const float adjusted=add(limited,mul(mul(constant(194,2),std::abs(limited)),sign));
    const float finalValue=std::min(std::max(adjusted,constant(194,0)),constant(194,1));
    const std::array<float,4> reference{std::exp2(finalValue),finalValue,finalValue,sourceValue};
    const char* ewhy=nullptr;
    auto actual=renderNativeExposureScalar(device,ep,ed,*eplan,&ewhy);
    if(!actual)std::printf("EXPOSURE_RENDER_REFUSED %s\n",ewhy);assert(actual && actual->size()==8);
    // Decode binary16 directly, not with a GPU conversion or guest VMX RTZ.
    const auto half=[](unsigned h){
        const unsigned exp=(h>>10)&31,mant=h&1023;assert(exp!=31);
        const float value=exp?std::ldexp(float(1024+mant),int(exp)-25):std::ldexp(float(mant),-24);
        return h&0x8000?-value:value;
    };
    double maxError=0;
    for(unsigned i=0;i<4;++i){
        const float value=half((*actual)[i*2]|(unsigned{(*actual)[i*2+1]}<<8));
        const double error=std::abs(double(value)-reference[i]);maxError=std::max(maxError,error);
        // One binary16 rounding unit plus float32 transcendental tolerance.
        assert(std::isfinite(value) && error<=.0006*std::max(1.,std::abs(double(reference[i]))));
    }
    auto eowner=owner1;auto eexpected=expected1;const auto eserial=eowner.nextSerial();
    for(unsigned w=0;w<2;++w){unsigned value=0;for(unsigned c=0;c<4;++c)value|=unsigned{(*actual)[w*4+c]}<<(c*8);eexpected[w]=value;}
    {NativeEdram32Mutation tx(eowner);
     assert(executeNativeExposureScalarDraw(device,ep,ed,eb,ep.vertexIdentity,ep.pixelIdentity,true,tx,&ewhy));}
    assert(eowner.nextSerial()==eserial+1 && eowner.readColorWords(720,1280,1280,720)==std::optional{eexpected});
    assert(eowner.readColorWords(1440,80,80,80)==expected4 && owner1.readColorWords(720,1280,1280,720)==std::optional{expected1});
    assert(ed.textures[0].source==expectedCopy1);
    NativeEdram32Bands empty;
    {NativeEdram32Mutation tx(empty);assert(executeNativeExposureScalarDraw(device,ep,ed,eb,ep.vertexIdentity,ep.pixelIdentity,true,tx));}
    assert(empty.readKnownColorRows(720,160,2,1) && !empty.readKnownColorRows(720,160,3,1) && !empty.knownTiles());
    NativeEdram32Bands pending;
    {NativeEdram32Mutation tx(pending);assert(!executeNativeExposureScalarDraw(device,ep,ed,eb,ep.vertexIdentity,ep.pixelIdentity,false,tx));}
    assert(!pending.readKnownColorRows(720,160,1,1));
    for(unsigned i=0;i<6;++i){auto bad=ep;auto opt=options1;auto desc=eplan->submitted;
        opt.externalTextures={{sampler.s,image1->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        if(i==0)bad.interpolators=1;if(i==1)bad.abiVersion=38;if(i==2)opt.samples=2;
        if(i==3)desc=eplan->storage.description;if(i==4)opt.externalTextures.clear();if(i==5)bad.pixelIdentity.resize(444);
        NativeTranslatedResources resources(device.device);assert(!resources.setup(device,VK_NULL_HANDLE,bad,{desc},NativeTranslatedBlend::Replace,opt));}
#ifdef RECOMP_PRIVATE_EXPOSURE_COPY
#include "exposure_copy_connection.inc"
#endif
    std::printf("EXPOSURE_CONNECTION_PASS variant=%u original_gpu_producer=1 actual_shader=1 numeric_r32=1 rgba16f=1 max_error=%.9g physical_two_words=1 scene_scratch_preserved=1 pending_query_refused=1 gameplay=0\n",variant,maxError);
}
