// AUTHORED inputs: production pipeline -> tile draw -> dispatch reducer ->
// readback. This is not admission of the live title's surface/owner transition.
#include "device.h"
#include <cassert>
static unsigned inspectedPipelines=0;
static VkResult inspectStencilPipeline(VkDevice device,VkPipelineCache cache,
        uint32_t count,const VkGraphicsPipelineCreateInfo* infos,
        const VkAllocationCallbacks* allocator,VkPipeline* pipelines) {
    assert(count==1 && infos[0].stageCount==1);
    assert(infos[0].pStages[0].stage==VK_SHADER_STAGE_VERTEX_BIT);
    const auto& color=*infos[0].pColorBlendState;
    assert(color.attachmentCount==4);
    for(unsigned i=0;i<4;++i)assert(color.pAttachments[i].colorWriteMask==0);
    ++inspectedPipelines;
    return vkCreateGraphicsPipelines(device,cache,count,infos,allocator,pipelines);
}
// Inspect what production actually hands Vulkan, not a duplicate planner.
#define vkCreateGraphicsPipelines inspectStencilPipeline
#include "native_logical_tile_target.h"
#undef vkCreateGraphicsPipelines
#include "native_stencil_tile_batch.h"
#include "native_stencil_pipeline_options.h"
#include "gta5_logical_tile_clear_fixture.h"
#include <cstdio>
#include <fstream>
#include <string_view>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main(int argc,char** argv) {
    assert(argc==2);std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream shader(argv[1],std::ios::binary|std::ios::ate);assert(shader);
    const auto size=shader.tellg();assert(size>0 && size<65536 && size%4==0);shader.seekg(0);
    NativeTranslatedDrawProgram program;program.abiVersion=25;program.directTriangles=true;
    program.stride=36;program.textureSlot=UINT32_MAX;
    program.vertexIdentity={'A','U','T','H'}; // AUTHORED, never used for game matching.
    program.vertexCode.resize(size/4);
    assert(shader.read(reinterpret_cast<char*>(program.vertexCode.data()),size));
    assert(validateNativeStencilOnlyProgram(program));
    ProbeDevice device;ShaderRequirements requirements;
    assert(device.inspect() && device.create(requirements,false,false,true));
    const auto capture=authoredLogicalClear();
    NativeLogicalTileTarget::Clear clear;clear.tiles=3;clear.aspects=63;clear.depth=.5f;clear.stencil=37;
    for(unsigned i=0;i<4;++i)clear.colors[i]={std::uint8_t(17+i*19),61,137,241};
    std::vector<std::uint8_t> input(8208+3*36);tileWord(input,0,3);
    const std::array<std::array<float,2>,3> xy{{{-1,-1},{3,-1},{-1,3}}};
    for(unsigned i=0;i<3;++i) {
        tileFloat(input,8208+i*36,xy[i][0]);tileFloat(input,8212+i*36,xy[i][1]);
        tileFloat(input,8216+i*36,.75f);
    }
    NativeTranslatedPipelineOptions options;options.colorAttachmentCount=4;
    options.colorWriteMasks={0,0,0,0};options.minDepth=0;options.maxDepth=1;
    options.depthStencil=decodeXenosDepthStencilState(0x00720213u,0x00010001u,0x00010001u);
    assert(options.depthStencil);
    for(unsigned invalid=0;invalid<2;++invalid) {
        auto bad=options;
        if(invalid==0)bad.colorAttachmentCount=1;else bad.colorWriteMasks={15,15,15,15};
        NativeTranslatedResources pipeline(device.device);
        assert(!pipeline.setup(device,VK_NULL_HANDLE,program,{},NativeTranslatedBlend::Replace,bad));
    }
    auto target=NativeLogicalTileTarget::create(device,*capture.state);assert(target && target->clear(clear));
    const auto before=target->readback();assert(before);
    const auto draw=[&](NativeLogicalTileTarget& owner,const std::array<unsigned,4>& clip) {
        return executeNativeStencilTileBatch(owner.tileCount(),[&](unsigned tile,const char** why) {
            return owner.drawCapturedStencilOnly(capture,tile,program,input,3,{1,1,0,0},clip,options,why);
        });
    };
    const auto result=draw(*target,{32,352,64,64});
    std::printf("STENCIL_CONNECTED_RESULT completed=%u accepted=%u pipelines=%u stage=%s\n",
        unsigned(result.completed),result.accepted,inspectedPipelines,result.stage);
    assert(result.completed && result.accepted==2 && inspectedPipelines==2 && target->completedDrawSerial()==2);
    const auto after=target->readback();assert(after);
    assert(after->colors==before->colors && after->depth==before->depth);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        const bool inside=x>=32 && x<96 && y>=352 && y<416;
        assert(after->stencil[y*1280+x]==(inside?36:37));
    }
    std::puts("STENCIL_CONNECTED_GPU_PASS production_pipeline_masks=0 colors_preserved=4 depth_preserved=1 stencil_exact=1 tiles=2 authored=1 gameplay=0");
    const auto empty=draw(*target,{0,0,0,0});assert(empty.completed && target->completedDrawSerial()==2);
    const auto unchanged=target->readback();assert(unchanged);
    assert(unchanged->colors==after->colors && unchanged->depth==after->depth && unchanged->stencil==after->stencil);
    for(unsigned initializedDepthTiles:{0u,1u}) {
        auto partial=NativeLogicalTileTarget::create(device,*capture.state);assert(partial);
        auto onlyColorStencil=clear;onlyColorStencil.aspects=0x2f;assert(partial->clear(onlyColorStencil));
        if(initializedDepthTiles) {
            auto onlyDepth=clear;onlyDepth.tiles=initializedDepthTiles;onlyDepth.aspects=0x10;
            assert(partial->clear(onlyDepth));
        }
        const auto refused=draw(*partial,{32,352,64,64});
        assert(!refused.completed && refused.accepted==initializedDepthTiles);
        assert(partial->completedDrawSerial()==initializedDepthTiles);
        assert(std::string_view(refused.stage)=="depth-uninitialized");
        const auto stencil=partial->readStencilRegion({0,0,1280,720});assert(stencil);
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const bool changed=initializedDepthTiles && x>=32 && x<96 && y>=352 && y<384;
            assert((*stencil)[y*1280+x]==(changed?36:37));
        }
    }
    std::puts("STENCIL_REFUSAL_CONNECTION_PASS actual_depth_uninitialized=1 first_and_later_tile=1 false_acknowledgement=0 empty_preserves=1 gameplay=0");
    NativeStencilOnlyPlan observed;
    observed.state=*decodeXenosDepthStencilState(0x00720213u,0x00FFFF20u,0x00FFFF00u);
    observed.minDepth=1;observed.maxDepth=0;observed.cull=0;
    options=nativeStencilPipelineOptions(observed);
    assert(options.minDepth==1 && options.maxDepth==0 && options.cullMode==VK_CULL_MODE_NONE);
    // Real captured register semantics, AUTHORED pixels/geometry. Both face
    // windings must draw, mismatching stencil must not change, and reverse Z
    // must invert which incoming depth fails. This is not live owner proof.
    for(unsigned stencilValue:{32u,37u})for(float z:{.25f,.75f})for(bool reverse:{false,true}) {
        clear.stencil=stencilValue;
        auto original=NativeLogicalTileTarget::create(device,*capture.state);assert(original && original->clear(clear));
        for(unsigned i=0;i<3;++i) {
            const auto j=reverse?2-i:i;
            tileFloat(input,8208+i*36,xy[j][0]);tileFloat(input,8212+i*36,xy[j][1]);
            tileFloat(input,8216+i*36,z);
        }
        const auto seed=original->readback();assert(seed);
        const auto submitted=draw(*original,{32,352,64,64});assert(submitted.completed && submitted.accepted==2);
        const auto pixels=original->readback();assert(pixels);
        assert(pixels->colors==seed->colors && pixels->depth==seed->depth);
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const bool changed=stencilValue==32 && z==.25f && x>=32 && x<96 && y>=352 && y<416;
            assert(pixels->stencil[y*1280+x]==(changed?0:stencilValue));
        }
    }
    std::puts("STENCIL_CAPTURED_STATE_GPU_PASS stencil_reference=32 masks=255 reverse_depth=1 both_windings=1 nonmatching_preserved=1 authored_geometry=1 gameplay=0");
}
