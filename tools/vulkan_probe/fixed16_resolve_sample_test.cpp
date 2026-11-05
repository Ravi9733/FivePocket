// AUTHORED full-domain input -> production resolve -> production image upload
// and view -> real Vulkan sampling. Does not claim guest shader equivalence.
#include "native_offset_sample_resolve.h"
#include "native_texture_image.h"
#include "image_sample_probe.h"
#include <bit>
#include <cmath>
#include <cstdio>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);std::setvbuf(stdout,nullptr,_IONBF,0);
    constexpr unsigned width=256,height=128;
    const auto description=XenosTextureDescription::decode({2u|(8u<<22)|0x80000000u,
        0x00a00000u|25u|(1u<<6),(width-1)|((height-1)<<13),0xD10u,0,0x200u});assert(description);
    const NativeOffscreenDrawPlan source{0x1000,width,height,width,0,32,4};
    NativeOffscreenResolveRequest request{};request.stack=0x10000;request.flags=0xec000000;
    NativeOffscreenResolveArguments args;args.stack92=std::vector<std::uint8_t>(4);args.stack100=std::vector<std::uint8_t>(4);
    const auto plan=planNativeOffsetSampleResolve(source,request,args,*description);assert(plan);
    std::vector<std::uint8_t> attachment(width*height*4);
    for(unsigned bits=0;bits<65536;++bits){attachment[2*bits]=bits;attachment[2*bits+1]=bits>>8;}
    const auto packed=packNativeOffsetSampleResolve(*plan,attachment,std::vector<std::uint8_t>(plan->storage.sourceBytes,0x5a));assert(packed);
    ProbeDevice device;ShaderRequirements requirements;assert(device.inspect() && device.create(requirements));
    auto image=NativeTextureImage::create(device,*description,*packed);assert(image);
    SampleReader reader(device,width,height,1,image->image(),VK_FORMAT_R16G16_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,
        argv[1],5,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image->view());
    const auto sampled=reader.readWords();assert(sampled.size()==width*height*5);
    for(unsigned pixel=0;pixel<width*height;++pixel) {
        assert(sampled[pixel*5+4]==0x53414d50u);
        for(unsigned channel=0;channel<2;++channel) {
            const unsigned bits=pixel*2+channel;
            const int value=bits<32768?int(bits):int(bits)-65536;
            const auto quantized=std::floor(std::clamp(value/32767.0,0.0,1.0)*65535.0+0.5);
            const auto expected=quantized/65535.0;
            const float actual=std::bit_cast<float>(sampled[pixel*5+channel]);
            assert(std::isfinite(actual) && std::abs(actual-expected)<0.00000012);
        }
    }
    // The importing consumer must not silently reinterpret signed/integer data.
    for(unsigned field=0;field<5;++field) {
        auto words=description->fetch;
        if(field<4)words[0]|=1u<<(2+field*2);else words[3]|=1u;
        const auto incompatible=XenosTextureDescription::decode(words);assert(incompatible);
        assert(!NativeTextureImage::create(device,*incompatible,*packed));
    }
    std::puts("FIXED16_RESOLVE_SAMPLE_GPU_PASS snorm_encodings=65536 production_packer=1 production_image_view=1 sampled_channels=2 numeric_select_rejections=5 authored=1 gameplay=0");
}
