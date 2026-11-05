// ABI23 resolve admission. The source plan is the one the device recorded for
// the completed draw in local-analysis/app-game-cache-replay-tuz2eky1:
//
//   RENDER_API_OFFSET_SAMPLE object=10271c10 width=320 height=180 base=904
//     tiles=48 target_format=7 state=1 completed=1
//   RENDER_NATIVE_RESOLVE_FAILURE stage=display-owner-invalid flags=00000000
//
// Source colour format 7 (k_16_16_16_16_FLOAT) maps to texture format 32 at
// eight bytes per texel, so the copy is byte-for-byte after tiling.
#include "native_offset_sample_resolve.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string_view>
using namespace recomp::gpu;

namespace {
constexpr std::uint32_t Stack=0x640AF930u;
unsigned passed=0;
void expect(bool c,const char* what){if(!c){std::fprintf(stderr,"OS_RESOLVE_FAIL %s\n",what);std::abort();}++passed;}
// A 320x180 format-32 destination, the shape this band writes.
XenosTextureDescription destination(unsigned format=32,unsigned w=320,unsigned h=180) {
    std::array<std::uint32_t,6> f{2u|((320u/32u)<<22)|0x80000000u,0x00a00000u|format|(1u<<6),
        (w-1)|((h-1)<<13),0u,0u,0x200u};
    const auto d=XenosTextureDescription::decode(f);
    expect(d.has_value(),"destination decodes");
    return *d;
}
NativeOffscreenDrawPlan source(unsigned format=7) {
    return NativeOffscreenDrawPlan{0x10271c10u,320,180,320,904,48,format};
}
NativeOffscreenResolveRequest request(std::uint32_t flags=0) {
    NativeOffscreenResolveRequest r{};r.stack=Stack;r.flags=flags;return r;
}
NativeOffscreenResolveArguments arguments() {
    NativeOffscreenResolveArguments a;
    a.stack92=std::vector<std::uint8_t>(4,0);a.stack100=std::vector<std::uint8_t>(4,0);
    return a;
}
void refuses(const NativeOffscreenResolveRequest& r,const NativeOffscreenResolveArguments& a,
             const XenosTextureDescription& d,const char* reason,const NativeOffscreenDrawPlan& s=source()) {
    const char* why=nullptr;
    expect(!planNativeOffsetSampleResolve(s,r,a,d,&why),reason);
    expect(why && std::string_view(why)==reason,reason);
}
}

int main() {
    const auto d=destination();const auto a=arguments();
    {
        const char* why=nullptr;
        const auto plan=planNativeOffsetSampleResolve(source(),request(),a,d,&why);
        expect(plan.has_value(),"captured resolve admitted");
        expect(plan->width==320 && plan->height==180,"extent from the source target");
        expect(plan->storage.bytesPerTexel==8,"k_16_16_16_16_FLOAT is eight bytes per texel");
        expect(plan->storage.linearBytes==320u*180u*8u,"linear extent");

        std::vector<std::uint8_t> attachment(plan->storage.linearBytes);
        for(std::size_t i=0;i<attachment.size();++i)attachment[i]=static_cast<std::uint8_t>(i*11u+3u);
        const std::vector<std::uint8_t> previous(plan->storage.sourceBytes,0x5A);
        const auto packed=packNativeOffsetSampleResolve(*plan,attachment,previous);
        expect(packed.has_value(),"packing accepts the fenced readback");
        const auto back=linearizeXenosTexture2D(plan->storage.description,*packed);
        expect(back.has_value() && *back==attachment,"round-trips byte for byte");
        expect(*packed!=previous,"real texels were written");
        expect(!packNativeOffsetSampleResolve(*plan,std::vector<std::uint8_t>(attachment.size()-8),previous),
            "short readback refused");
    }

    // This band binds one colour target, so index 0 is the only source.
    // The flags word's fields are the ones sub_823C2738 actually reads; each
    // refuses under its own name, and a bit the guest never reads refuses too
    // rather than being ignored.
    refuses(request(1),a,d,"resolve-flags");          // bit 0, never read there
    refuses(request(0x80),a,d,"resolve-flags");       // bit 7
    refuses(request(0x00010000),a,d,"resolve-flags"); // bit 16
    refuses(request(0x10),a,d,"resolve-sample-select");
    refuses(request(0x100),a,d,"resolve-clear");      // colour clear
    refuses(request(0x200),a,d,"resolve-clear");      // depth clear
    refuses(request(0x2000),a,d,"resolve-controls");  // bit 13
    refuses(request(0x4000),a,d,"resolve-controls");  // bit 14
    refuses(request(0x8000),a,d,"resolve-controls");  // bit 15
    // copy_dest_exp_bias, bits 26-31 signed. The float targets take none.
    refuses(request(0xec000000),a,d,"resolve-exponent-bias");  // -5
    refuses(request(0x04000000),a,d,"resolve-exponent-bias");  // +1

    // Source and destination formats must be the matched pair.
    refuses(request(),a,destination(36),"destination-format");
    refuses(request(),a,destination(6),"destination-format");
    // An unmapped colour format has no destination format at all. Format 4 is
    // mapped now, so it refuses on its bias instead -- a different reason.
    refuses(request(),a,d,"source-colour-format",source(0));
    refuses(request(),a,destination(25),"resolve-exponent-bias",source(4));
    {   // k_32_FLOAT sources map to texture format 36 at four bytes per texel.
        const char* why=nullptr;
        const auto plan=planNativeOffsetSampleResolve(source(14),request(),a,destination(36),&why);
        expect(plan.has_value(),"k_32_FLOAT source admitted");
        expect(plan->storage.bytesPerTexel==4,"k_32_FLOAT is four bytes per texel");
    }
    refuses(request(),a,destination(32,160,180),"destination-extent");

    // Subresource and argument shape.
    {auto r=request();r.mip=1;refuses(r,a,d,"resolve-subresource");}
    {auto r=request();r.slice=1;refuses(r,a,d,"resolve-subresource");}
    {auto r=request();r.stack=0;refuses(r,a,d,"resolve-stack");}
    {auto b=a;b.stack100=std::vector<std::uint8_t>{0,0,0,1};refuses(request(),b,d,"resolve-stack");}

    // ABI24: a k_16_16 source into a k_16_16 destination. The guest asks for
    // exponent bias -5; the pixel stage already divided by 32, so the net bias
    // is zero. Decode stored SNORM, clamp negatives, then quantize UNORM;
    // the unrelated clear-path XOR would incorrectly map zero to 0.5.
    {
        const auto fixed=source(4);
        const auto dst=destination(25);
        const char* why=nullptr;
        const auto plan=planNativeOffsetSampleResolve(fixed,request(0xec000000),a,dst,&why);
        expect(plan.has_value(),"k_16_16 resolve admitted with the guest's -5 bias");
        expect(plan->copy==NativeOffsetSampleResolveCopy::SnormToUnorm16,
            "fixed point takes numeric SNORM-to-UNORM conversion");
        expect(plan->storage.bytesPerTexel==4,"two 16-bit channels");
        expect(nativeOffsetSampleResolveConvertsFixed16(4) && !nativeOffsetSampleResolveConvertsFixed16(7) &&
               !nativeOffsetSampleResolveConvertsFixed16(14),"only the fixed-point source is converted");
        // Values from an independent saturating UNORM oracle, not from XOR.
        std::vector<std::uint8_t> attachment(plan->storage.linearBytes,0);
        const auto put=[&](std::size_t texel,std::int16_t r,std::int16_t g) {
            const auto w=[&](std::size_t at,std::int16_t v) {
                const auto u=static_cast<std::uint16_t>(v);
                attachment[at]=std::uint8_t(u&0xFF);attachment[at+1]=std::uint8_t(u>>8);
            };
            w(texel*4,r);w(texel*4+2,g);
        };
        put(0,0,0);put(1,32767,-32767);put(2,-1,1);put(3,16384,-32768);
        const std::vector<std::uint8_t> previous(plan->storage.sourceBytes,0x5A);
        const auto packed=packNativeOffsetSampleResolve(*plan,attachment,previous);
        expect(packed.has_value(),"packing accepts the fenced readback");
        const auto back=linearizeXenosTexture2D(plan->storage.description,*packed);
        expect(back.has_value(),"round-trips through the guest layout");
        const auto half=[&](std::size_t at) {
            return unsigned((*back)[at]) | (unsigned((*back)[at+1])<<8);
        };
        expect(half(0)==0 && half(2)==0,"zero remains zero, never midpoint");
        expect(half(4)==0xFFFFu && half(6)==0,"positive endpoint and negative clamp");
        expect(half(8)==0 && half(10)==2,"one signed unit scales to two unsigned units");
        expect(half(12)==32769 && half(14)==0,"nearest rounding and most-negative SNORM");
        expect(*packed!=previous,"real texels were written");
        // Exhaust all source encodings through production pack and addressing;
        // reference arithmetic is independent of the implementation's integer ratio.
        for(unsigned bits=0;bits<65536;++bits) {
            attachment[bits*2]=static_cast<std::uint8_t>(bits);
            attachment[bits*2+1]=static_cast<std::uint8_t>(bits>>8);
        }
        const auto sweep=packNativeOffsetSampleResolve(*plan,attachment,previous);
        expect(sweep.has_value(),"full SNORM encoding sweep packs");
        const auto decodedSweep=linearizeXenosTexture2D(plan->storage.description,*sweep);
        expect(decodedSweep.has_value(),"full SNORM encoding sweep linearizes");
        bool matches=true;
        for(unsigned bits=0;bits<65536;++bits) {
            const int signedValue=bits<32768?int(bits):int(bits)-65536;
            const auto expected=unsigned(std::floor(std::clamp(signedValue/32767.0,0.0,1.0)*65535.0+0.5));
            const auto actual=unsigned((*decodedSweep)[bits*2])|(unsigned((*decodedSweep)[bits*2+1])<<8);
            matches=matches && actual==expected;
        }
        expect(matches,"all SNORM encodings match independent normalized-conversion oracle");
        for(unsigned component=0;component<4;++component) {
            auto words=dst.fetch;words[0]|=1u<<(2+2*component);
            const auto signedDestination=XenosTextureDescription::decode(words);
            expect(signedDestination.has_value(),"signed destination decodes for negative test");
            refuses(request(0xec000000),a,*signedDestination,"destination-numeric",fixed);
        }
        {auto words=dst.fetch;words[3]|=1u;
         const auto integerDestination=XenosTextureDescription::decode(words);
         expect(integerDestination.has_value(),"integer destination decodes for negative test");
         refuses(request(0xec000000),a,*integerDestination,"destination-numeric",fixed);}
        // The float route must not acquire the bias.
        const char* floatWhy=nullptr;
        const auto floatPlan=planNativeOffsetSampleResolve(source(),request(),a,destination(32),&floatWhy);
        expect(floatPlan.has_value() && floatPlan->copy==NativeOffsetSampleResolveCopy::ByteIdentical,
            "float resolve stays byte-identical");
    }
    // A fixed-point source into a float destination, or the reverse, is a
    // silent reinterpretation.
    refuses(request(0xec000000),a,destination(32),"destination-format",source(4));
    refuses(request(),a,destination(25),"destination-format");

    // ABI23 into a single-byte destination: k_8_8_8_8 is bitwise equivalent to
    // texture formats 6, 14 and 50 but not to 2, so one component is kept.
    {
        const auto rgba=source(0);
        const auto dst=destination(2);
        const char* why=nullptr;
        const auto plan=planNativeOffsetSampleResolve(rgba,request(0),a,dst,&why);
        expect(plan.has_value(),"k_8 destination admitted");
        expect(plan->copy==NativeOffsetSampleResolveCopy::RedChannelToR8,"one component is kept");
        expect(plan->storage.bytesPerTexel==1,"one byte per texel");
        expect(plan->storage.linearBytes==320u*180u,"linear extent is one byte per pixel");
        expect(nativeOffsetSampleResolveKeepsRed(0,2),"the route names its formats");
        expect(!nativeOffsetSampleResolveKeepsRed(0,6),"an equivalent destination copies instead");
        expect(!nativeOffsetSampleResolveKeepsRed(4,2),"the fixed point source does not");
        // Component 0 of each RGBA8 texel, and nothing else.
        std::vector<std::uint8_t> attachment(320u*180u*4u);
        for(std::size_t i=0;i<attachment.size();++i)attachment[i]=static_cast<std::uint8_t>(i*7u+1u);
        const std::vector<std::uint8_t> previous(plan->storage.sourceBytes,0x5A);
        const auto packed=packNativeOffsetSampleResolve(*plan,attachment,previous);
        expect(packed.has_value(),"packing accepts the fenced readback");
        const auto back=linearizeXenosTexture2D(plan->storage.description,*packed);
        expect(back.has_value() && back->size()==320u*180u,"round-trips at one byte per pixel");
        bool kept=true;
        for(std::size_t at=0;at<back->size();++at)kept=kept && (*back)[at]==attachment[at*4];
        expect(kept,"every kept byte is its texel's component 0");
        // A wrong-sized readback is refused rather than reinterpreted.
        expect(!packNativeOffsetSampleResolve(*plan,std::vector<std::uint8_t>(320u*180u),previous),
            "an already-narrowed readback refused");
    }

    std::printf("NATIVE_OFFSET_SAMPLE_RESOLVE_PASS captured_source=1 single_target=1 "
        "byte_identical=1 normalized_conversion=1 snorm_encodings=65536 kept_component=1 cases=%u gpu=0 gameplay=0\n",passed);
    return 0;
}
