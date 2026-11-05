// Actual selector2 state; input comes from the preceding4x4 GPU copy, not a
// saved pixel capture. The1x1 target aliases scene word0 at720, not scratch1440.
{
    assert(read(p+"/scalar1/vertex-shader.bin",96)==sp.vertexIdentity);
    assert(read(p+"/scalar1/pixel-shader.bin",444)==sp.pixelIdentity);
    Gta5InlineDraw d1;d1.primitive=8;d1.vertexCount=3;d1.stride=24;
    d1.deviceState=read(p+"/scalar1/device-state.bin",13860);d1.vertices=read(p+"/scalar1/vertices.bin",72);
    d1.vertexShader=be(d1.deviceState,13072);d1.pixelShader=be(d1.deviceState,13068);
    const auto m1=read(p+"/scalar1/capture-meta.bin",36);
    d1.scissorRegisters=std::array<unsigned,3>{be(m1,24),be(m1,28),be(m1,32)};
    Gta5InlineDraw::Texture t1;t1.slot=0;for(unsigned i=0;i<6;++i)t1.fetch[i]=be(m1,i*4);
    t1.source=destination4;d1.textures={t1};
    assert((t1.fetch[1]&0xFFFFF000)==copy4->destination.baseAddress-0xA0000000u);
    Gta5BoundSurfaceCapture b1;for(unsigned i=0;i<5;++i)b1.surfaces[i].object=be(d1.deviceState,12816+i*4);
    b1.surfaces[0].header=read(p+"/scalar1/surface-0-resource.bin",40);
    const auto vc1=read(p+"/scalar1/vertex-constants.bin",4096),pc1=read(p+"/scalar1/pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){d1.vertexConstants[i]=be(vc1,i*4);d1.pixelConstants[i]=be(pc1,i*4);}
    if(variant)d1.pixelConstants[255*4]=colorBits(.125f); // Authored dynamic-scale control.
    const auto plan1=planNativeR32ScalarDraw(d1,b1,sp,sp.vertexIdentity,sp.pixelIdentity,false,true);
    assert(plan1 && plan1->target.width==1 && plan1->target.height==1 && plan1->target.pitch==80 &&
        plan1->target.baseTile==720 && plan1->target.tileCount==1 && plan1->storage.sourceBytes==4096);
    auto options1=options4;options1.externalTextures={{sampler.s,image4->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::vector<std::uint8_t> input1(8208+72);input1[3]=3;
    const auto put1=[&](unsigned at,unsigned v){for(unsigned i=0;i<4;++i)input1[at+i]=v>>(24-i*8);};
    for(unsigned i=0;i<1024;++i){put1(16+i*4,d1.vertexConstants[i]);put1(4112+i*4,d1.pixelConstants[i]);}
    std::copy(d1.vertices.begin(),d1.vertices.end(),input1.begin()+8208);
    auto target1=NativeLinearSampleTarget::create(device,1,1,1);assert(target1);
    const auto uv1=target1->draw(uvProgram,input1,6,plan1->viewport,{plan1->submitted},options1,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);assert(uv1);
    assert(*uv1==std::vector<float>({.5f,.5f,0.f,1.f}));
    const auto scalar1=target1->draw(sp,input1,6,plan1->viewport,{plan1->submitted},options1,
        NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);assert(scalar1 && scalar1->size()==4);
    unsigned tapMask1=0;
    const auto sample1=[&](unsigned tap) {
        const auto x=std::clamp(int(std::floor(mul(add(.5f,(positions[tap][0]+1.5f/1024)/4),4))),0,3);
        const auto y=std::clamp(int(std::floor(mul(add(.5f,(positions[tap][1]+1.5f/1024)/4),4))),0,3);
        assert(!(tapMask1&(1u<<(y*4+x))));tapMask1|=1u<<(y*4+x);
        return (*scalar4)[(y*4+x)*4];
    };
    float value1=add(sample1(14),sample1(15));for(int tap=13;tap>=0;--tap)value1=add(value1,sample1(unsigned(tap)));
    value1=mul(value1,colorFloat(d1.pixelConstants[255*4]));assert(tapMask1==0xFFFF);
    for(float v:*scalar1)assert(std::isfinite(v) && std::abs(double(v)-value1)<=2e-6*std::max(1.,std::abs(double(value1))));
    auto owner1=owner4;auto expected1=parentExpected;
    // With x=y=0, both the pitch80 target and pitch1280 scene address tile720,
    // word0. Only this word changes; compare every remaining scene pixel.
    expected1[0]=colorBits((*scalar1)[0]);const auto serial1=owner1.nextSerial();const char* why1=nullptr;
    {NativeEdram32Mutation tx(owner1);
     const bool done=executeNativeR32ScalarDraw(device,sp,d1,b1,sp.vertexIdentity,sp.pixelIdentity,true,tx,&why1);
     if(!done)std::printf("R32_SCALAR1_EXECUTOR_REFUSED %s\n",why1);assert(done);}
    assert(owner1.nextSerial()==serial1+1);
    assert(owner1.readColorWords(720,1280,1280,720)==std::optional{expected1});
    assert(owner1.readColorWords(1440,80,80,80)==expected4);
    assert(owner1.readKnownColorRows(720,80,1,1)==std::optional{std::vector<unsigned>{colorBits((*scalar1)[0])}});
    assert(owner4.readColorWords(720,1280,1280,720)==std::optional{parentExpected});
    assert(d1.textures[0].source==destination4);
    // A new owner learns ONE word, not an entire tile or the prior scene.
    NativeEdram32Bands partial1;
    {NativeEdram32Mutation tx(partial1);assert(executeNativeR32ScalarDraw(device,sp,d1,b1,sp.vertexIdentity,sp.pixelIdentity,true,tx));}
    assert(!partial1.knownTiles() && partial1.readKnownColorRows(720,80,1,1));
    assert(!partial1.readKnownColorRows(720,80,2,1) && !partial1.readKnownColorRows(1440,80,1,1));
    NativeEdram32Bands pending1;
    {NativeEdram32Mutation tx(pending1);assert(!executeNativeR32ScalarDraw(device,sp,d1,b1,sp.vertexIdentity,sp.pixelIdentity,false,tx));}
    assert(!pending1.readKnownColorRows(720,80,1,1));
#ifdef RECOMP_PRIVATE_R32_SCALAR1_COPY
#include "r32_scalar1_copy_connection.inc"
#endif
    std::printf("R32_SCALAR1_CONNECTION_PASS variant=%u actual_binding=1 same_original_shader=1 current4copy_source=1 numeric_r32=1 runtime_scale=1 pixels=1 original_sum_order=1 physical_base=720 scene_other_words_preserved=1 scratch1440_preserved=1 one_word_ownership=1 pending_query_refused=1 next_copy=0 live_completion=0\n",variant);
}
