#include "native_logical_geometry_state.h"
#include "gta5_native_scene_inputs.h"
#include "gta5_logical_tile_clear_fixture.h"
#include <cstdio>
using namespace recomp::gpu;using namespace recomp::gpu::test;
int main() {
    unsigned cases=0,negative=0;
    for(unsigned abi:{14u,15u,16u})for(unsigned stride:{24u,28u}) {
        auto surfaces=authoredLogicalClear();surfaces.state->resize(13860);
        (*surfaces.state)[11071]=3;
        Gta5InlineDraw draw;draw.device=surfaces.call.device;draw.deviceState=*surfaces.state;
        draw.primitive=4;draw.stride=stride;draw.vertexCount=3;draw.vertices.resize(3*stride,0x47);
        const auto set=[&](unsigned r,unsigned v){tileWord(draw.deviceState,*gta5DrawRegisterOffset(r),v);};
        constexpr unsigned registers[][2]{
            {0x200E,0},{0x200F,0x20002000},{0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},{0x2104,0xFFFF},
            {0x2180,0x40210505},{0x2181,8},{0x2182,0x30007},{0x2183,0},{0x2184,0},
            {0x2200,0x700066},{0x210C,0xFFFF00},{0x210D,0xFFFF},
            {0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x33},{0x2204,0x80000},
            {0x2205,0x18002},{0x2206,0x43F},{0x2207,0},{0x2208,4},
            {0x2209,0x10001},{0x220A,0x10001},{0x220B,0x10001},
            {0x2300,0x400},{0x2301,0},{0x2302,4},{0x2303,0x40000000},{0x2304,0x3F800000},
            {0x2305,0x40000000},{0x2306,0x3F800000},{0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},
            {0x210F,0x44200000},{0x2110,0x44200000},{0x2111,0xC3B40000},{0x2112,0x43B40000},
            {0x2113,0x3F800000},{0x2114,0},{0x2293,0}};
        for(const auto& r:registers)set(r[0],r[1]);
        for(unsigned value:{0u,16u,17u,126u,127u}) {
            NativeTranslatedDrawProgram queryProgram;queryProgram.abiVersion=abi;queryProgram.stride=stride;
            queryProgram.indexed=true;queryProgram.geometryDepth20e4=true;queryProgram.geometryMrtFormats={0,0,0,0};
            queryProgram.geometryMrtOutputs=15;queryProgram.geometryPixelEffects=abi>=15;queryProgram.geometryDepthColorWitness=abi==16;
            set(0x2293,value);const auto queryState=planNativeLogicalGeometryState(draw,queryProgram,{0,0,1280,720});
            assert(queryState && queryState->visibilityControl==value);++cases;
            for(unsigned kill:{0x80u,0x100u}) {set(0x2293,value|kill);assert(!planNativeLogicalGeometryState(draw,queryProgram,{0,0,1280,720}));++negative;}
        }
        set(0x2293,0);
        surfaces.state=draw.deviceState;
        for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=0xA3000000+i;draw.pixelConstants[i]=0xB4000000+i;}
        NativeTranslatedDrawProgram p;p.abiVersion=abi;p.stride=stride;p.indexed=true;p.textureSlot=UINT32_MAX;
        p.geometryDepth20e4=true;p.geometryMrtFormats={0,0,0,0};p.geometryMrtOutputs=15;
        p.geometryPixelEffects=abi>=15;p.geometryDepthColorWitness=abi==16;
        const std::vector<unsigned> indices{0,1,2};const std::array<unsigned,4> full{0,0,1280,720};
        const auto input=prepareNativeLogicalGeometry(p,draw,indices,surfaces,full,{});assert(input);
        assert(input->state.scissor==full && input->descriptions.empty() && !input->anisotropy);
        assert(input->payload.size()==8208+draw.vertices.size() && tileWord(input->payload,0)==3 && tileWord(input->payload,4)==0);
        for(unsigned i=0;i<1024;++i)assert(tileWord(input->payload,16+4*i)==draw.vertexConstants[i] &&
            tileWord(input->payload,4112+4*i)==draw.pixelConstants[i]);
        assert(std::equal(draw.vertices.begin(),draw.vertices.end(),input->payload.begin()+8208));++cases;
        const auto clipped=planNativeLogicalGeometryState(draw,p,{64,392,1264,704});
        assert(clipped && (clipped->scissor==std::array<unsigned,4>{64,392,1200,312}));++cases;
        // Authored identities test selection only, never shader execution.
        p.vertexIdentity={1,2,3};p.pixelIdentity={4,5,6};p.vertexCode={7};p.pixelCode={8};
        std::vector<NativeTranslatedDrawProgram> programs{p};
        NativeClippedFanProgram queryProgram{p.vertexIdentity,p.pixelIdentity,{9},stride};
        std::vector<NativeClippedFanProgram> queryPrograms{queryProgram};
        assert(selectNativeGeometryQueryProgram(p,queryPrograms)==&queryPrograms[0]);++cases;
        queryPrograms[0].positionCode.clear();assert(!selectNativeGeometryQueryProgram(p,queryPrograms));++negative;
        queryPrograms={queryProgram};queryPrograms[0].pixelIdentity[0]^=1;
        assert(!selectNativeGeometryQueryProgram(p,queryPrograms));++negative;
        queryPrograms={queryProgram,queryProgram};assert(!selectNativeGeometryQueryProgram(p,queryPrograms));++negative;
        assert(selectNativeLogicalGeometryProgram(programs,p.vertexIdentity,p.pixelIdentity,draw,full)==&programs[0]);++cases;
        // The same input bridge called at live EndTiling. Authored CPU inputs,
        // not a game/GPU test: missing or changed producers must not be hidden.
        Gta5BoundIndexedStorage geometry;geometry.draw=draw;geometry.request={4,0,0,3};
        geometry.indexPhysicalAddress=0x1000;geometry.rawIndices=geometry.effectiveIndices=indices;
        Gta5BoundSurfaceCapture bound;
        for(unsigned slot=0;slot<5;++slot) {
            const auto& s=surfaces.surfaces[slot];bound.surfaces[slot]={s.object,s.tileCache,s.header};
        }
        std::vector<std::uint8_t> commandBytes;
        for(unsigned word:{0xC0032201u,0u,0x30004u,0x1000u,0x40000003u})
            for(unsigned lane=0;lane<4;++lane)commandBytes.push_back(word>>(24-8*lane));
        Pm4Snapshot commands;Pm4Error error;
        assert(decodePm4(commandBytes.data(),commandBytes.size(),{},commands,error));
        const std::vector<std::pair<unsigned,NativeMipTextureCapture>> textures;
        const std::array<unsigned,1> sequences{1};
        std::array<Gta5NativeSceneDrawSource,1> source{{{1,true,true,&geometry,&bound,
            &p.vertexIdentity,&p.pixelIdentity,&full,&textures}}};
        const auto bind=[&]{return prepareGta5NativeSceneInputs(draw.device,surfaces,sequences,source,programs,commands);};
        auto connected=bind();assert(connected && connected->draws.size()==1 && connected->queryCommands.matchedDraws==1);
        assert(connected->draws[0].input.payload==input->payload && connected->draws[0].programIndex==0);++cases;
        source[0].originalEmissionReturned=false;assert(!bind());source[0].originalEmissionReturned=true;++negative;
        source[0].orderedConstants=false;assert(!bind());source[0].orderedConstants=true;++negative;
        source[0].sequence=2;assert(!bind());source[0].sequence=1;++negative;
        source[0].geometry=nullptr;assert(!bind());source[0].geometry=&geometry;++negative;
        geometry.indexPhysicalAddress+=4;assert(!bind());geometry.indexPhysicalAddress-=4;++negative;
        geometry.request.indexCount=6;assert(!bind());geometry.request.indexCount=3;++negative;
        bound.surfaces[4].header.reset();assert(!bind());bound.surfaces[4].header=surfaces.surfaces[4].header;++negative;
        geometry.draw.vertexConstants[7]^=0x1234;connected=bind();assert(connected);
        assert(tileWord(connected->draws[0].input.payload,16+7*4)==geometry.draw.vertexConstants[7]);++cases;
        geometry.draw.vertexConstants[7]^=0x1234;
        NativeMipTextureCapture t0,t3;
        t0.fetch={0x81000002u,0x10000014,127|(127u<<13),0xD10u,2u<<6,0x20000A10};
        t3.fetch=t0.fetch;t3.fetch[1]=0x11000014;
        const auto chain=planXenosUnpackedMipChain(t0.fetch);assert(chain);
        t0.base.assign(chain->baseBytes,0x31);t0.mips.assign(chain->mipBytes,0x42);
        t3.base.assign(chain->baseBytes,0xD2);t3.mips.assign(chain->mipBytes,0xE3);
        auto textured=p;textured.textureSlots={3,0};textured.textureSlot=3;
        const std::vector<std::pair<unsigned,NativeMipTextureCapture>> numericOrder{{0,t0},{3,t3}};
        const auto ordered=prepareNativeLogicalGeometry(textured,draw,indices,surfaces,full,numericOrder);assert(ordered);
        assert((ordered->textureSourceIndices==std::vector<unsigned>{1,0}));
        assert(ordered->descriptions[0].fetch==t3.fetch && ordered->descriptions[1].fetch==t0.fetch);
        assert(numericOrder[ordered->textureSourceIndices[0]].second.base.front()==0xD2 &&
            numericOrder[ordered->textureSourceIndices[1]].second.base.front()==0x31);++cases;
        assert(!prepareNativeLogicalGeometry(textured,draw,indices,surfaces,full,{{3,t3},{3,t0}}));++negative;
        assert(!prepareNativeLogicalGeometry(textured,draw,indices,surfaces,full,{{0,t0},{2,t3}}));++negative;
        assert(!selectNativeLogicalGeometryProgram(programs,{1,2,9},p.pixelIdentity,draw,full));++negative;
        assert(!selectNativeLogicalGeometryProgram(programs,p.vertexIdentity,{4,5,9},draw,full));++negative;
        assert(!selectNativeLogicalGeometryProgram(programs,{},p.pixelIdentity,draw,full));++negative;
        programs[0].pixelCode.clear();
        assert(!selectNativeLogicalGeometryProgram(programs,p.vertexIdentity,p.pixelIdentity,draw,full));++negative;
        programs={p,p};assert(!selectNativeLogicalGeometryProgram(programs,p.vertexIdentity,p.pixelIdentity,draw,full));++negative;
        programs[0].stride=stride==24?28:24;
        assert(selectNativeLogicalGeometryProgram(programs,p.vertexIdentity,p.pixelIdentity,draw,full)==&programs[1]);++cases;
        for(const auto& r:registers) {
            set(r[0],r[1]^0x80000000u);
            // Depth/stencil and disabled write masks have independently decoded
            // modes, not a blanket equality table; test strict fields here.
            if(r[0]!=0x2200 && r[0]!=0x210C && r[0]!=0x210D)
                {assert(!planNativeLogicalGeometryState(draw,p,full));++negative;}
            set(r[0],r[1]);
        }
        for(const auto& rect:std::array<std::array<unsigned,4>,4>{{{9,0,8,8},{0,9,8,8},{0,0,1281,720},{0,0,1280,721}}})
            {assert(!planNativeLogicalGeometryState(draw,p,rect));++negative;}
        auto bad=p;bad.abiVersion=13;assert(!planNativeLogicalGeometryState(draw,bad,full));++negative;
        bad=p;bad.geometryDepthColorWitness=!bad.geometryDepthColorWitness;assert(!planNativeLogicalGeometryState(draw,bad,full));++negative;
        bad=p;bad.geometryPixelEffects=!bad.geometryPixelEffects;assert(!planNativeLogicalGeometryState(draw,bad,full));++negative;
        bad=p;bad.geometrySampleDepth=true;assert(!planNativeLogicalGeometryState(draw,bad,full));++negative;
        bad=p;bad.geometryMrtFormats[2]=1;assert(!planNativeLogicalGeometryState(draw,bad,full));++negative;
        bad=p;bad.geometryMrtBlendControls[0]=0x7010001;assert(!planNativeLogicalGeometryState(draw,bad,full));++negative;
        for(const auto& mutation:std::array<std::array<unsigned,2>,3>{{{11068,0x20},{11071,0x23},{11072,0xC0}}}) {
            const auto saved=draw.deviceState[mutation[0]];draw.deviceState[mutation[0]]=mutation[1];
            assert(!planNativeLogicalGeometryState(draw,p,full));draw.deviceState[mutation[0]]=saved;++negative;
        }
        auto wrong=surfaces;wrong.call.device+=4;assert(!prepareNativeLogicalGeometry(p,draw,indices,wrong,full,{}));++negative;
        wrong=surfaces;wrong.surfaces[4].header.reset();assert(!prepareNativeLogicalGeometry(p,draw,indices,wrong,full,{}));++negative;
        wrong=surfaces;(*wrong.state)[0]^=1;assert(!prepareNativeLogicalGeometry(p,draw,indices,wrong,full,{}));++negative;
        assert(!prepareNativeLogicalGeometry(p,draw,{0,1,3},surfaces,full,{}));++negative;
        assert(!prepareNativeLogicalGeometry(p,draw,{0,1},surfaces,full,{}));++negative;
        draw.vertices.pop_back();assert(!prepareNativeLogicalGeometry(p,draw,indices,surfaces,full,{}));++negative;
        draw.vertices.push_back(0x47);
        bad=p;bad.textureSlots={1,1};
        assert(!prepareNativeLogicalGeometry(bad,draw,indices,surfaces,full,{{1,{}},{1,{}}}));++negative;
    }
    std::printf("NATIVE_LOGICAL_GEOMETRY_STATE_PASS cases=%u negatives=%u submitted_constants_exact=1 scissor_extent_exact=1 gpu_execution=0\n",cases,negative);
    std::puts("NATIVE_SCENE_INPUT_BRIDGE_PASS original_dma_bound=1 missing_producer_rejected=1 current_constants_consumed=1 gpu_execution=0");
    std::puts("NATIVE_SCENE_TEXTURE_ORDER_PASS shader_slot_identity=1 payload_index_matches_descriptor=1 missing_duplicate_rejected=1 gpu_execution=0");
}
