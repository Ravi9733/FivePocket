// HOST ONLY: original captured state + authored/derived physical owners.
// No saved resource is a LIVE physical ownership receipt or GPU proof.
#include "native_stencil_rejected_rectangle.h"
#include "xenos_texture_2d.h"
#include <cassert>
#include <bit>
#include <cstdio>
#include <fstream>
#include <string_view>
#ifndef RECOMP_PRIVATE_STENCIL_REJECT_IDENTITY
#error An explicit analysed private shader identity is required
#endif
#include RECOMP_PRIVATE_STENCIL_REJECT_IDENTITY
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string path=argv[1];
    const auto read=[&](const std::string& name,unsigned size){
        std::ifstream f(path+"/"+name,std::ios::binary|std::ios::ate);assert(f && f.tellg()==size);
        f.seekg(0);std::vector<std::uint8_t> b(size);assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
        (std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    Gta5InlineDraw d;d.primitive=8;d.vertexCount=3;d.stride=36;d.vertices=read("vertices.bin",108);
    d.deviceState=read("device-state.bin",13860);d.vertexShader=word(d.deviceState,13072);d.pixelShader=word(d.deviceState,13068);
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,47187200};
    #ifdef RECOMP_PRIVATE_CONDITIONAL_STENCIL_REJECT
    const auto& identity=nativeConditionalStencilRejectIdentity;
    #else
    const auto& identity=nativeStencilRejectIdentity;
    #endif
    const auto vs=read("vertex-shader.bin",168),ps=read("pixel-shader.bin",identity.pixel.size());
    assert(vs==identity.vertex && ps==identity.pixel);
    Gta5BoundSurfaceCapture b;
    for(unsigned i=0;i<5;++i){b.surfaces[i].object=word(d.deviceState,12816+4*i);b.surfaces[i].tileCacheObject=word(d.deviceState,13104+4*i);}
    for(unsigned i:{0u,4u})b.surfaces[i].header=read("surface-"+std::to_string(i)+"-argument.bin",40);
    NativeEdram32Bands owner;NativeStencilRejectedRectangleProof proof;const char* why=nullptr;
    const auto check=[&](const auto& draw,const auto& bindings,const auto& vertex,const auto& pixel,bool indexed=false,bool idle=true){
        return proveNativeStencilRejectedRectangle(draw,bindings,identity,vertex,pixel,indexed,idle,owner,proof,&why);
    };
    assert(!check(d,b,vs,ps) && std::string_view(why)=="physical-stencil-unknown");
    std::vector<std::uint32_t> raw(1280*720);
    for(unsigned n=0;n<raw.size();++n){const unsigned s=n&255;raw[n]=(n<<8)|((s&7)==1?s^1:s);}
    assert(owner.recordCompletedDepthWords(0,1280,1280,720,raw,owner.nextSerial()));
    assert(check(d,b,vs,ps) && proof.examined==raw.size() && proof.passing==0);
    assert(proof.ownerNextSerial==owner.nextSerial());
    // The colour destination [720,1440) is deliberately unknown throughout.
    assert(!owner.readDepthWords(720,1280,1280,720));
    for(unsigned at:std::array<unsigned,3>{0u,unsigned(raw.size()/2),unsigned(raw.size()-1)}) {
        auto one=raw;one[at]=(one[at]&0xFFFFFF00u)|1;
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,one,owner.nextSerial()));
        assert(!check(d,b,vs,ps) && proof.passing==1 && proof.examined==raw.size());
    }
    assert(owner.recordCompletedDepthWords(0,1280,1280,720,raw,owner.nextSerial()));
    assert(!check(d,b,vs,ps,true) && !check(d,b,vs,ps,false,false));
    {auto bad=vs;bad[0]^=1;assert(!check(d,b,bad,ps));}
    {auto bad=ps;bad[0]^=1;assert(!check(d,b,vs,bad));}
    for(unsigned r:{0x2000u,0x2001u,0x2002u,0x2104u,0x210Du,0x2180u,0x2181u,0x2182u,0x2183u,0x2184u,
            0x2200u,0x2201u,0x2202u,0x2203u,0x2204u,0x2205u,0x2206u,0x2293u,0x231Cu,0x2324u,0x2325u}) {
        auto bad=d;put(bad.deviceState,*gta5DrawRegisterOffset(r),*gta5DrawRegister(d,r)^1u);assert(!check(bad,b,vs,ps));
    }
    for(unsigned at:{12408u,12816u,12820u,12832u,13068u,13072u}) {
        auto bad=d;put(bad.deviceState,at,word(bad.deviceState,at)^4u);assert(!check(bad,b,vs,ps));
    }
    {auto bad=b;bad.surfaces[4].header.reset();assert(!check(d,bad,vs,ps));}
    {auto bad=b;put(*bad.surfaces[0].header,28,0xC02D0);assert(!check(d,bad,vs,ps));}
    {auto bad=d;bad.scissorRegisters.reset();assert(!check(bad,b,vs,ps));}
    {auto bad=d;bad.deviceState[11068]|=0x10;assert(!check(bad,b,vs,ps));}
    {auto bad=d;bad.textures.emplace_back();assert(!check(bad,b,vs,ps));}
    {auto bad=identity;bad.profile=static_cast<NativeStencilRejectedRectangleProfile>(99);
        assert(!proveNativeStencilRejectedRectangle(d,b,bad,vs,ps,false,true,owner,proof,&why));}
    {auto bad=identity;bad.profile=identity.profile==NativeStencilRejectedRectangleProfile::DirectionalExpanded?
        NativeStencilRejectedRectangleProfile::ConditionalUnexpanded:NativeStencilRejectedRectangleProfile::DirectionalExpanded;
        assert(!proveNativeStencilRejectedRectangle(d,b,bad,vs,ps,false,true,owner,proof,&why));}
    // Successful production transaction preserves only already-known owners.
    bool display=false;
    {NativeEdram32Mutation mutation(owner);assert(owner.knownTiles()==0);
        assert(finishNativeReadOnlyDraw(proveNativeStencilRejectedRectangle(d,b,identity,vs,ps,false,true,
            mutation.candidate(),proof,&why),mutation));}
    assert(!display && owner.readDepthWords(0,1280,1280,720)==raw && !owner.readDepthWords(720,1280,1280,720));
    {NativeEdram32Mutation mutation(owner);assert(!finishNativeReadOnlyDraw(false,mutation,&display));}
    assert(!display && owner.knownTiles()==0);
    std::puts("STENCIL_REJECT_HOST_PASS captured_state=1 authored_owner=1 single_passing_sample_rejects=1 unknown_colour_preserved=1 identity_query_guards=1 gpu=0 gameplay=0");
    #ifdef RECOMP_PRIVATE_CONDITIONAL_STENCIL_REJECT
    std::puts("CONDITIONAL_STENCIL_REJECT_HOST_PASS exact_new_capture=1 all_sample_test=1 native_shader_execution=0 gameplay=0");
    return 0;
    #endif
    // Separate DERIVED connection: saved slot2 -> independently evaluated ABI26
    // physical RGBA -> production depth half-tile view -> current stencil test.
    // An actual run MUST re-read the completed physical owner, not this fixture.
    std::vector<std::uint8_t> storage;
    for(unsigned part=0;storage.size()<3768320;++part) {
        auto bytes=read("texture-2-part-"+std::to_string(part)+".bin",std::min(std::size_t{65536},3768320-storage.size()));
        storage.insert(storage.end(),bytes.begin(),bytes.end());
    }
    const auto desc=XenosTextureDescription::decode({2315274242,123109510,5891327,2624532,3,528});assert(desc);
    auto source=linearizeXenosTexture2D(*desc,storage);assert(source);
    std::array<unsigned,256> red{};
    for(unsigned n=0;n<256;++n) {
        const float product=(float(n)/255.0f)*std::bit_cast<float>(0x41FEFFFFu);
        const float value=(product-std::floor(product))*std::bit_cast<float>(0x3D008081u);
        red[n]=std::lround(value*255.0f);
    }
    std::vector<std::uint8_t> rgba(source->size());
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        const auto to=4*(y*1280+x),from=4*(y*1280+x/80*80+(x%80+40)%80);
        for(unsigned c=0;c<4;++c)rgba[to+c]=c?(*source)[from+c]:red[(*source)[from+c]];
    }
    NativeOffscreenDrawPlan plan{b.surfaces[4].object,1280,720,1280,0,720,0};
    assert(owner.recordCompletedColorBand(plan,rgba,owner.nextSerial()));
    assert(check(d,b,vs,ps) && proof.examined==1280*720 && !proof.passing);
    std::puts("STENCIL_REJECT_DERIVED_CONNECTION_PASS saved_texture=1 derived_screen_output=1 examined=921600 passing=0 live_owner=0 gpu=0 gameplay=0");
}
