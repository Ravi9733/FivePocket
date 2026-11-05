// AUTHORED GPU driver fixture. Real scene clears -> disjoint fixed16 clear ->
// MRT and pyramid attachment draws -> physical reference model -> raw10 copy.
// No saved pixels or synthetic fixture is used by the live renderer.
#include "native_logical_tile_target.h"
#include "native_fixed16_clear_target.h"
#include "native_fixed16_edram.h"
#include "native_rectangle_mrt_target.h"
#include "native_offscreen_draw_target.h"
#include "native_raw10_color_resolve.h"
#include "native_serialized_resource_batch.h"
#include "../../android/tests/gta5_logical_tile_clear_fixture.h"
#ifndef RECOMP_PRIVATE_RAW10_CALL
#error The exact stopped resolve call must be receipt-bound
#endif
#include RECOMP_PRIVATE_RAW10_CALL
#include <bit>
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
namespace {
std::vector<std::uint8_t> read(const std::string& path,unsigned size=0) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f && f.tellg()>0);
    const auto n=static_cast<unsigned>(f.tellg());assert(!size || n==size);
    std::vector<std::uint8_t> b(n);f.seekg(0);assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
}
std::vector<std::uint32_t> module(const std::string& path) {
    const auto b=read(path);assert(b.size()%4==0);std::vector<std::uint32_t> w(b.size()/4);
    std::memcpy(w.data(),b.data(),b.size());assert(w.front()==0x07230203);return w;
}
unsigned be(const auto& b,unsigned at) {assert(at+4<=b.size());
    return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];}
void put(auto& b,unsigned at,unsigned v) {assert(at+4<=b.size());for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-i*8);}
unsigned le(const auto& b,unsigned at) {assert(at+4<=b.size());
    return b[at]|(unsigned(b[at+1])<<8)|(unsigned(b[at+2])<<16)|(unsigned(b[at+3])<<24);}
Gta5TileResolveCapture resolveCall(const std::string& folder) {
    const auto load=[&](const std::string& key,unsigned size){return read(folder+"/native-resolve-failure-"+key+".bin",size);};
    Gta5TileResolveCapture c;c.stack=SavedStack;c.arguments=SavedArgs;c.f1=SavedF1;
    c.state=load("state",13860);c.destinationHeader=load("destination-header",52);c.surfaces.emplace();
    for(unsigned i=0;i<5;++i) {auto& s=c.surfaces->surfaces[i];s.object=be(*c.state,12816+4*i);
        if(s.object)s.header=load("surface-"+std::to_string(i),40);}
    c.pointed.rectangle=load("rectangle",16);c.pointed.stack92=load("stack92",4);c.pointed.stack100=load("stack100",4);
    return c;
}
// Independent general texture tile addressing, not the production specializer.
unsigned textureOffset(unsigned x,unsigned y) {
    const unsigned v=(((y/32)*40+x/32)*64+(y/2%8)*8+x%8)*4;
    return (v&15)|((y%2)*16)|((v&16)*2)|(((x/8%4)^((y/8%2)*2))*64)|
        ((v&224)*8)|((v&~255u)*16)|((y/16%2)*2048);
}
// Expected raw colour view for the full copy. Filled only from checked GPU
// outputs, with independently calculated physical addressing.
void oracleWrite(std::vector<unsigned>& expected,const NativeOffscreenDrawPlan& p,const auto& bytes) {
    const unsigned words=p.format==7?2:1,perTile=80/words;
    assert(bytes.size()==std::size_t(p.width)*p.height*words*4);
    for(unsigned y=0;y<p.height;++y)for(unsigned x=0;x<p.width;++x)for(unsigned w=0;w<words;++w) {
        const unsigned tile=p.baseTile+(y/16)*(p.pitch/perTile)+x/perTile;
        if(tile<720 || tile>=1440)continue;
        const unsigned cx=(tile-720)%16*80+(x%perTile)*words+w,cy=(tile-720)/16*16+y%16;
        assert(cx<1280 && cy<720);expected[cy*1280+cx]=le(bytes,((y*p.width+x)*words+w)*4);
    }
}
struct Draw {Gta5InlineDraw input;Gta5BoundSurfaceCapture bound;};
Draw authoredDraw(unsigned width,unsigned height,unsigned pitch,bool mrt,unsigned format) {
    Draw f;auto& d=f.input;d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices.resize(72);d.deviceState.resize(13860);
    const auto reg=[&](unsigned r,unsigned v){put(d.deviceState,*gta5DrawRegisterOffset(r),v);};
    reg(0x2000,pitch);reg(0x2104,mrt?255:15);reg(0x2200,0x00700770);reg(0x2302,5);
    reg(0x210F,std::bit_cast<unsigned>(float(width)/2));reg(0x2110,std::bit_cast<unsigned>(float(width)/2));
    reg(0x2111,std::bit_cast<unsigned>(-float(height)/2));reg(0x2112,std::bit_cast<unsigned>(float(height)/2));
    put(d.deviceState,10368,pitch);d.scissorRegisters=std::array<unsigned,3>{0,0,(height<<16)|width};
    for(unsigned slot=0;slot<(mrt?2u:1u);++slot) {
        auto& s=f.bound.surfaces[slot];s.object=0x10001000+slot*64;put(d.deviceState,12816+slot*4,s.object);
        s.header=std::vector<std::uint8_t>(40);put(*s.header,24,pitch);
        put(*s.header,28,(slot?720:904)|((slot?14:format)<<16));put(*s.header,36,((width-1)<<18)|((height-1)<<3));
    }
    for(unsigned i=0;i<(mrt?2u:1u);++i) {
        Gta5InlineDraw::Texture t;t.slot=mrt?(i?12:9):5;
        t.fetch={2u|(1u<<22)|0x80000000u,0x01000000u|(mrt?6u:32u),31u|(31u<<13),0xD10,0,0x200};
        const auto desc=XenosTextureDescription::decode(t.fetch);assert(desc);
        const auto plan=planXenosTexture2D(*desc);assert(plan);t.source.resize(plan->sourceBytes);
        d.textures.push_back(std::move(t));
    }
    return f;
}
constexpr std::array<std::array<float,4>,3> Positions{{{-1,1,0,1},{1,1,0,1},{-1,-1,0,1}}};
unsigned halfEighth(unsigned v) {constexpr unsigned bits[]{0,0x3000,0x3400,0x3600,0x3800,0x3900,0x3A00,0x3B00};return bits[v%8];}
void checkOutput(const auto& bytes,unsigned width,unsigned height,unsigned format,bool mrtSecond=false) {
    assert(bytes.size()==std::size_t(width)*height*(format==7?8:4));
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const unsigned at=(y*width+x)*(format==7?8:4);
        if(format==7) {assert(le(bytes,at)==(halfEighth(x)|(halfEighth(y)<<16)));assert(le(bytes,at+4)==0x3C003800);}
        else assert(le(bytes,at)==std::bit_cast<unsigned>(mrtSecond?float(x+y*640):float(x%8)/8));
    }
}
}
int main(int argc,char** argv) {
    assert(argc==6);std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto state=read(argv[1],13860);const auto call=resolveCall(argv[2]);assert(planNativeRaw10ColorResolve(call));
    const auto vs=module(argv[3]),mrtPs=module(argv[4]),offsetPs=module(argv[5]);
    ProbeDevice device;ShaderRequirements requirements;assert(device.inspect() && device.create(requirements));
    auto scene=NativeLogicalTileTarget::create(device,state);assert(scene);
    const auto capture=test::authoredLogicalClear(state);NativeEdram32Bands owner;std::vector<unsigned> expected(1280*720);
    for(unsigned bin=0;bin<2;++bin) {
        NativeEdram32Mutation mutation(owner);const auto plans=prepareNativeSceneColorAliases(mutation,capture,bin);assert(plans);
        if(bin)assert(scene->carryCapturedTile(capture,0,1));
        std::array<std::array<std::uint8_t,4>,4> colors{};
        for(unsigned s=0;s<4;++s)colors[s]={std::uint8_t(11+bin*83+s*7),31,63,127};
        assert(scene->clear({1u<<bin,0x3F,colors,0.5f,19,{}}));
        assert(stageCompletedNativeSceneColors(mutation,*plans,[&](unsigned slot) {
            assert(!owner.knownTiles());auto bytes=scene->readCapturedColorTile(capture,bin,slot);assert(bytes);
            oracleWrite(expected,(*plans)[slot].physical,*bytes);return bytes;
        }));mutation.commit();
    }
    assert(owner.readColorWords(720,1280,1280,720)==std::optional{expected});
    Gta5Fixed16ClearPlan shadow{0x2000,256,256,320,64,64,1,4,0,{}};
    shadow.depth=Gta5Depth24ClearPlan{0x2100,256,256,320,0,64,1,1,*planExactXenosFloat24Clear(0.5,23)};
    auto shadowTarget=NativeFixed16ClearTarget::create(device,shadow);assert(shadowTarget);
    {NativeEdram32Mutation mutation(owner);assert(executeNativeEdram32Fixed16Write(mutation,shadow,[&] {
        return shadowTarget->clear(shadow.color,shadow.depth->value);
    }));}
    assert(owner.readColorWords(720,1280,1280,720)==std::optional{expected});
    NativeTranslatedDrawProgram program;program.stride=24;program.interpolators=2;
    program.vertexIdentity={1};program.pixelIdentity={2};program.vertexCode=vs;program.pixelCode=mrtPs;
    program.abiVersion=22;program.rectangleMrt=true;program.textureSlot=9;program.textureSlots={9,12};
    auto mrt=authoredDraw(640,360,640,true,7);const char* why=nullptr;
    const auto mrtPlan=planNativeRectangleMrtDraw(mrt.input,program,mrt.bound,&why);assert(mrtPlan);
    auto mrtTarget=NativeRectangleMrtTarget::create(device,*mrtPlan);assert(mrtTarget && !mrtTarget->readRgba(0));
    {NativeEdram32Mutation mutation(owner);
     const bool drawn=mrtTarget->draw(program,mrt.input,mrt.bound,Positions,&why);
     std::printf("RAW10_MRT_GPU drawn=%u stage=%s\n",unsigned(drawn),why);assert(drawn);
     for(unsigned slot=0;slot<2;++slot) {
        const auto bytes=mrtTarget->readRgba(slot);assert(bytes);const auto& p=mrtPlan->targets[slot];
        checkOutput(*bytes,p.width,p.height,p.format,slot==1);oracleWrite(expected,p,*bytes);
        assert(recordCompletedNativeColorDraw(mutation,p,*bytes,NativeEdram64Model::Xenia40x16));
     }mutation.commit();}
    assert(owner.readColorWords(720,1280,1280,720)==std::optional{expected});
    program.rectangleMrt=false;program.abiVersion=23;program.offsetSample=true;
    program.textureSlot=5;program.textureSlots={5};program.pixelCode=offsetPs;
    for(const auto size:{std::array<unsigned,3>{320,180,320},{160,90,160},{80,45,80},{40,11,80}}) {
        const unsigned format=size[0]==40?14:7;auto draw=authoredDraw(size[0],size[1],size[2],false,format);
        const auto p=planNativeOffsetSampleDraw(draw.input,program,draw.bound,&why);assert(p);
        auto target=NativeOffscreenDrawTarget::create(device,*p);assert(target && !target->readRgba());
        {NativeEdram32Mutation mutation(owner);
         const bool drawn=target->drawOffsetSample(program,draw.input,draw.bound,Positions,&why);
         std::printf("RAW10_OFFSET_GPU width=%u drawn=%u stage=%s\n",p->width,unsigned(drawn),why);assert(drawn);
         const auto bytes=target->readRgba();assert(bytes);checkOutput(*bytes,p->width,p->height,p->format);
         oracleWrite(expected,*p,*bytes);assert(recordCompletedNativeColorDraw(mutation,*p,*bytes,NativeEdram64Model::Xenia40x16));
         mutation.commit();}
        assert(owner.readColorWords(720,1280,1280,720)==std::optional{expected});
    }
    const auto plan=planNativeRaw10ColorResolve(call);assert(plan);
    const std::vector<std::uint8_t> previous(plan->Bytes,0xA5);auto destination=previous,packed=previous;
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        const auto raw=expected[y*1280+x];const unsigned swapped=(raw&0xC00FFC00)|((raw&1023)<<20)|((raw>>20)&1023);
        put(packed,textureOffset(x,y),swapped);
    }
    unsigned stores=0;const auto publish=[&](const auto& result) {
        ++stores;const NativeSerializedResourceUpdate u{0xA1000000,{destination.data(),destination.size()},&previous,&result};
        return publishNativeSerializedResourceBatch(&u,1);
    };
    assert(publishNativeRaw10ColorResolve(call,owner,previous,[&]{return sameNativeRaw10ColorInputs(call,call);},publish));
    assert(stores==1 && destination==packed);
    auto missing=owner;assert(missing.invalidate(904,1));
    assert(!publishNativeRaw10ColorResolve(call,missing,previous,[]{return true;},publish));assert(stores==1);
    destination=previous;destination.back()^=1;const auto stale=destination;
    assert(!publishNativeRaw10ColorResolve(call,owner,previous,[]{return true;},publish));assert(stores==2 && destination==stale);
    assert(!publishNativeRaw10ColorResolve(call,owner,previous,[]{return false;},publish));assert(stores==2);
    std::puts("RAW10_GPU_CONNECTION_PASS scene_bins=2 disjoint_fixed16_clear=1 mrt_attachments=2 pyramid_draws=4 "
        "all_output_pixels_checked=1 independent_physical_and_texture_addressing=1 "
        "production_publication=1 padding_preserved=1 stale_or_unknown_no_write=1 "
        "authored_shaders=1 exact_captured_copy=1 reference_model=xenia40x16 xbox_layout_verified=0 live_dispatch=0 gameplay=0");
}
