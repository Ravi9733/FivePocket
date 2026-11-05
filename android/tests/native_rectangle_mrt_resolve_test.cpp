// ABI22 resolve admission. The source plan and the destination header are both
// the ones the device recorded for the stopped resolve in
// local-analysis/app-game-cache-replay-qohsqof6:
//
//   RENDER_API_RECTANGLE_MRT target0=10271bd0 target1=10270dc0 width=640
//     height=360 base0=904 base1=720 tiles=184 completed=1 stage=drawn
//   RENDER_NATIVE_RESOLVE_FAILURE flags=00000001 destination=a83933fc
//
// flags=1 selects colour1, the k_32_FLOAT target, and the destination is a
// k_32_FLOAT texture, so the copy is byte-for-byte after tiling.
#include "native_rectangle_mrt_resolve.h"
#include <cassert>
#include <cstdio>
#include <string_view>

using namespace recomp::gpu;

namespace {
// Captured destination fetch words, byte 28 of its 52-byte header.
constexpr std::array<std::uint32_t,6> Destination{0x85000156u,0xa67fd0a4u,0x2ce27fu,0x16d1u,0x0u,0x200u};
// The colour0 destination the device recorded for the SECOND resolve of the
// same completed target, in app-game-cache-replay-_yw_doyk: format 32
// (k_16_16_16_16_FLOAT), 640x360, tiled, pitch 640, endian 1, eight bytes/texel.
constexpr std::array<std::uint32_t,6> Destination0{0x85000156u,0xa65d2060u,0x2ce27fu,0xd11u,0x0u,0x200u};
constexpr std::uint32_t Stack=0x640AF930u;
unsigned passed=0;
void expect(bool condition,const char* what) {
    if(!condition){std::fprintf(stderr,"MRT_RESOLVE_FAIL %s\n",what);std::abort();}
    ++passed;
}
XenosTextureDescription destination(std::array<std::uint32_t,6> words=Destination) {
    const auto d=XenosTextureDescription::decode(words);
    expect(d.has_value(),"destination decodes");
    return *d;
}
NativeRectangleMrtPlan source() {
    NativeRectangleMrtPlan p{};
    p.targets[0]=NativeOffscreenDrawPlan{0x10271bd0u,640,360,640,904,184};
    p.targets[1]=NativeOffscreenDrawPlan{0x10270dc0u,640,360,640,720,184};
    p.formats={7,14}; // k_16_16_16_16_FLOAT and k_32_FLOAT
    return p;
}
NativeOffscreenResolveRequest request(std::uint32_t flags=1) {
    NativeOffscreenResolveRequest r{};r.stack=Stack;r.flags=flags;return r;
}
NativeOffscreenResolveArguments arguments() {
    NativeOffscreenResolveArguments a;
    a.stack92=std::vector<std::uint8_t>(4,0);a.stack100=std::vector<std::uint8_t>(4,0);
    return a;
}
void refuses(const NativeOffscreenResolveRequest& r,const NativeOffscreenResolveArguments& a,
             const XenosTextureDescription& d,const char* reason) {
    const char* why=nullptr;
    expect(!planNativeRectangleMrtResolve(source(),r,a,d,&why),reason);
    expect(why && std::string_view(why)==reason,reason);
}
}

int main() {
    const auto d=destination();const auto a=arguments();
    {
        const char* why=nullptr;
        const auto plan=planNativeRectangleMrtResolve(source(),request(),a,d,&why);
        expect(plan.has_value(),"captured resolve admitted");
        expect(plan->sourceSlot==1,"flags bits 0-2 select colour1");
        expect(plan->width==640 && plan->height==360,"extent from the source target");
        expect(plan->storage.bytesPerTexel==4,"four bytes per texel");
        expect(plan->storage.linearBytes==640u*360u*4u,"linear extent");
        expect(plan->storage.description.format==6,"raw addressing view");

        // The whole point: the fenced attachment readback lands in the guest's
        // tiled, endian-permuted storage byte for byte, padding preserved.
        std::vector<std::uint8_t> attachment(plan->storage.linearBytes);
        for(std::size_t i=0;i<attachment.size();++i)attachment[i]=static_cast<std::uint8_t>(i*13u+5u);
        const std::vector<std::uint8_t> previous(plan->storage.sourceBytes,0x5A);
        const auto packed=packNativeRectangleMrtResolve(*plan,attachment,previous);
        expect(packed.has_value(),"packing accepts the fenced readback");
        expect(packed->size()==plan->storage.sourceBytes,"packed size is the storage extent");
        const auto back=linearizeXenosTexture2D(plan->storage.description,*packed);
        expect(back.has_value() && *back==attachment,"round-trips byte for byte");
        expect(*packed!=previous,"real texels were written");

        // A wrong-sized readback is refused rather than padded or truncated.
        expect(!packNativeRectangleMrtResolve(*plan,std::vector<std::uint8_t>(attachment.size()-4),previous),
            "short readback refused");
        expect(!packNativeRectangleMrtResolve(*plan,attachment,std::vector<std::uint8_t>(previous.size()-1)),
            "wrong previous allocation refused");
    }

    // Source selection: both colours this pass writes are admitted, each into
    // its own byte-identical destination format.
    {
        const auto d0=destination(Destination0);
        const char* why=nullptr;
        const auto plan=planNativeRectangleMrtResolve(source(),request(0),a,d0,&why);
        expect(plan.has_value(),"colour0 resolve admitted");
        expect(plan->sourceSlot==0,"flags 0 selects colour0");
        expect(plan->storage.bytesPerTexel==8,"k_16_16_16_16_FLOAT is eight bytes per texel");
        expect(plan->storage.linearBytes==640u*360u*8u,"colour0 linear extent");
        std::vector<std::uint8_t> attachment(plan->storage.linearBytes);
        for(std::size_t i=0;i<attachment.size();++i)attachment[i]=static_cast<std::uint8_t>(i*7u+3u);
        const std::vector<std::uint8_t> previous(plan->storage.sourceBytes,0x5A);
        const auto packed=packNativeRectangleMrtResolve(*plan,attachment,previous);
        expect(packed.has_value(),"colour0 packing accepted");
        const auto back=linearizeXenosTexture2D(plan->storage.description,*packed);
        expect(back.has_value() && *back==attachment,"colour0 round-trips byte for byte");
    }
    // Each source format demands its own destination format; crossing them is
    // a silent conversion and must be refused.
    refuses(request(0),a,d,"destination-format");                      // colour0 into a k_32_FLOAT destination
    refuses(request(1),a,destination(Destination0),"destination-format"); // colour1 into a k_16_16_16_16 destination
    refuses(request(2),a,d,"resolve-source-slot");
    refuses(request(4),a,d,"resolve-source-slot"); // depth; this pass binds none
    refuses(request(0x100|1),a,d,"resolve-flags"); // region clear not implemented
    refuses(request(0x10|1),a,d,"resolve-flags");

    // Destination must be the matching format and extent.
    {auto w=Destination;w[1]=(w[1]&~63u)|30u;refuses(request(),a,destination(w),"destination-format");}
    {auto w=Destination;w[2]=(320u-1u)|((360u-1u)<<13);refuses(request(),a,destination(w),"destination-extent");}

    // Subresource and argument shape.
    {auto r=request();r.mip=1;refuses(r,a,d,"resolve-subresource");}
    {auto r=request();r.slice=1;refuses(r,a,d,"resolve-subresource");}
    {auto r=request();r.stack=0;refuses(r,a,d,"resolve-stack");}
    {auto r=request();r.stack=Stack|1;refuses(r,a,d,"resolve-stack");}
    {auto b=a;b.stack92=std::vector<std::uint8_t>{1,0,0,0};refuses(request(),b,d,"resolve-stack");}
    {auto r=request();r.destinationPoint=Stack;auto b=a;b.point=std::vector<std::uint8_t>(8,1);
     refuses(r,b,d,"resolve-destination-point");}

    // An explicit rectangle must cover the whole surface at the origin.
    {
        auto r=request();r.rectangle=Stack;auto b=a;
        const auto rect=[&](std::uint32_t x,std::uint32_t y,std::uint32_t w,std::uint32_t h) {
            std::vector<std::uint8_t> v(16);
            const std::uint32_t f[4]{x,y,w,h};
            for(unsigned i=0;i<4;++i)for(unsigned c=0;c<4;++c)v[i*4+c]=static_cast<std::uint8_t>(f[i]>>(24-8*c));
            return v;
        };
        b.rectangle=rect(0,0,640,360);
        const char* why=nullptr;
        expect(planNativeRectangleMrtResolve(source(),r,b,d,&why).has_value(),"full rectangle admitted");
        b.rectangle=rect(0,0,320,360);refuses(r,b,d,"resolve-rectangle");
        b.rectangle=rect(8,0,640,360);refuses(r,b,d,"resolve-rectangle");
    }

    std::printf("NATIVE_RECTANGLE_MRT_RESOLVE_PASS captured_source=1 captured_destination=1 "
        "source_index_from_flags=1 byte_identical=1 cases=%u gpu=0 gameplay=0\n",passed);
    return 0;
}
