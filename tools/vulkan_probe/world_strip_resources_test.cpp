// Exact saved descriptors and bytes -> existing storage/sampling planners.
// This inventories unsupported numeric views; it is NOT draw admission.
#include "native_geometry_sampler_plan.h"
#include "xenos_float32_texture.h"
#include "xenos_depth_stencil_state.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string folder=argv[1];std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto read=[&](const std::string& name,unsigned size) {
        std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);assert(f && f.tellg()==size);
        f.seekg(0);std::vector<std::uint8_t> b(size);assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    const auto be=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto meta=read("texture-meta.bin",4*36);
    for(unsigned i=0;i<4;++i) {
        const auto slot=be(meta,i*36),baseBytes=be(meta,i*36+4),mipBytes=be(meta,i*36+8);
        std::array<unsigned,6> fetch{};for(unsigned j=0;j<6;++j)fetch[j]=be(meta,i*36+12+4*j);
        const auto d=XenosTextureDescription::decode(fetch);assert(d);
        const auto chain=planXenosUnpackedMipChain(fetch);
        const auto sampler=planNativeGeometrySampler2D(fetch);
        const auto floating=planXenosFloatStorage(*d),single=planXenosTexture2D(*d);
        const auto base=read("texture-"+std::to_string(slot)+".bin",baseBytes);
        const auto mips=read("texture-"+std::to_string(slot)+"-mips.bin",mipBytes);
        std::printf("WORLD_STRIP_TEXTURE slot=%u format=%u width=%u height=%u pitch=%u endian=%u "
            "mips=%u..%u signs=%u,%u,%u,%u integer=%u exponent=%d "
            "chain=%u geometry_sampler=%u raw_float=%u single=%u base=%u mip=%u\n",
            slot,d->format,d->width,d->height,d->pitchTexels,d->endian,d->mipMin,d->mipMax,
            d->sign[0],d->sign[1],d->sign[2],d->sign[3],d->integerComponents,d->exponentBias,
            bool(chain),bool(sampler),bool(floating),bool(single),baseBytes,mipBytes);
        if(chain) {
            assert(chain->baseBytes==baseBytes && chain->mipBytes==mipBytes);
            const auto decoded=linearizeXenosUnpackedMipChain(fetch,base,mips);assert(decoded);
            assert(decoded->size()==d->mipMax+1);
        } else if(floating) {
            assert(floating->sourceBytes==baseBytes && !mipBytes);
            const auto decoded=linearizeXenosTexture2D(floating->description,base);assert(decoded);
        } else assert(false && "unimplemented texture storage");
    }
    const auto ds=decodeXenosDepthStencilState(0x00708767,0x00FFFF00,0);assert(ds);
    assert(ds->depthEnable && ds->depthWrite && ds->depthCompare==6 && ds->stencilEnable &&
        !ds->backfaceEnable && ds->front.compare==7 && ds->front.pass==2 && ds->front.reference==0 &&
        ds->front.readMask==255 && ds->front.writeMask==255 && !ds->front.fail && !ds->front.depthFail);
    std::puts("WORLD_STRIP_RESOURCE_INVENTORY_PASS slots=4 storage_decoded=1 native_admission=0");
}
