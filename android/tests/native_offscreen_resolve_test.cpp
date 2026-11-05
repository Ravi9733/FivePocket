#include "native_offscreen_resolve_request.h"
#include "xenos_color_copy.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
using Bytes=std::vector<std::uint8_t>;
int main() {
    unsigned cases=0;
    for(auto extent:std::array<std::array<unsigned,3>,5>{{{512,256,512},{300,200,320},
            {128,260,128},{32,32,32},{256,128,288}}})
    for(unsigned endian=0;endian<4;++endian)for(unsigned red:{0u,2u}) {
        const std::array<std::uint32_t,6> fetch{0x80000002u|(extent[2]/32<<22),0x10000000u|6u|(endian<<6),
            (extent[0]-1)|((extent[1]-1)<<13),2*(red|8u|((2-red)<<6)|(3u<<9)),3u<<6,0x20000200};
        const auto original=XenosTextureDescription::decode(fetch);assert(original);
        const auto chain=planXenosUnpackedMipChain(fetch);assert(chain);
        const Bytes base(chain->baseBytes,0xA5),mips(chain->mipBytes,0x73);
        for(unsigned level=0;level<4;++level) {
            const auto selected=planNativeOffscreenResolveDestination(*original,level);assert(selected);
            assert(selected->original.fetch==fetch && original->fetch==fetch && selected->level==level);
            const auto& s=selected->storage;const auto& d=s.description;
            const auto offset=chain->levels[level].planeOffset;
            const auto& plane=level?mips:base;const auto unchanged=plane;
            const Bytes before(plane.begin()+offset,plane.begin()+offset+s.sourceBytes);
            NativeOffscreenDrawPlan source{0x1000,d.width,d.height,((d.width+79)/80)*80,0,0};
            Bytes rgba(d.width*d.height*4);
            for(unsigned i=0;i<rgba.size();++i)rgba[i]=(i*17+level*31+i/53)%256;
            NativeOffscreenResolveRequest request;request.stack=0x10000;request.mip=level;
            NativeOffscreenResolveArguments args;args.stack92=Bytes(4);args.stack100=Bytes(4);
            assert(auditNativeOffscreenResolve(source,request,args,&*original).plan);
            if(level)assert(!auditNativeOffscreenResolve(source,request,args).plan);
            const auto packed=packNativeOffscreenResolveLevel(source,*original,level,rgba,before);assert(packed);
            // Independent address witness also checks padding was preserved.
            Bytes expected=before;std::vector<bool> touched(s.sourceBytes,false);
            constexpr unsigned xorMask[]{0,1,3,2};
            for(unsigned y=0;y<d.height;++y)for(unsigned x=0;x<d.width;++x) {
                unsigned address=0;assert(colorCopyOffset(d.pitchTexels,(d.height+31)&~31u,x,y,address));
                for(unsigned c=0;c<4;++c) {
                    const auto at=(address+c)^xorMask[endian];assert(at<expected.size() && !touched[at]);
                    touched[at]=true;expected[at]=rgba[(y*d.width+x)*4+(red==2 && c<3?2-c:c)];
                }
            }
            assert(*packed==expected && plane==unchanged);
            // Connected producer/consumer: a mip-sized top-left draw remains
            // inside the original larger surface; the copy must use its row
            // stride, not reinterpret the full source as a smaller image.
            auto large=source;large.width=extent[0];large.height=extent[1];
            Bytes producer(large.width*large.height*4,0xCE);
            for(unsigned y=0;y<d.height;++y)
                std::copy_n(rgba.begin()+std::size_t{y}*d.width*4,d.width*4,producer.begin()+std::size_t{y}*large.width*4);
            const auto intactProducer=producer;
            request.rectangle=0x11000;args.rectangle=Bytes(16);
            const auto put=[](Bytes& b,unsigned at,unsigned v){for(unsigned c=0;c<4;++c)b[at+c]=v>>(24-8*c);};
            put(*args.rectangle,8,d.width);put(*args.rectangle,12,d.height);
            const auto regionAudit=auditNativeOffscreenMipRegionResolve(large,request,args,*original);
            assert(regionAudit.plan && regionAudit.plan->width==d.width && regionAudit.plan->height==d.height);
            auto clearRequest=request;clearRequest.flags=0x100;clearRequest.clearColor=0x12000;
            auto clearArgs=args;clearArgs.color=Bytes(16);
            const auto clearAudit=auditNativeOffscreenMipRegionResolve(large,clearRequest,clearArgs,*original);
            assert(bool(clearAudit.plan)==(d.width%8==0 && d.height%8==0));
            if(!clearAudit.plan)assert(clearAudit.issues&ResolveClearColor);
            if(level)assert(!auditNativeOffscreenResolve(large,request,args,&*original).plan);
            const auto region=packNativeOffscreenResolveMipRegion(large,*original,level,d.width,d.height,producer,before);
            assert(region==packed && producer==intactProducer);
            auto differentOutside=producer;
            for(unsigned y=0;y<large.height;++y)for(unsigned x=0;x<large.width;++x)
                if(x>=d.width || y>=d.height)for(unsigned c=0;c<4;++c)differentOutside[(y*large.width+x)*4+c]=0x31;
            assert(packNativeOffscreenResolveMipRegion(large,*original,level,d.width,d.height,differentOutside,before)==packed);
            for(unsigned at:{0u,4u,8u,12u}) {
                auto badArgs=args;put(*badArgs.rectangle,at,at<8?1:0);
                assert(!auditNativeOffscreenMipRegionResolve(large,request,badArgs,*original).plan);
            }
            auto badArgs=args;badArgs.rectangle->pop_back();
            assert(!auditNativeOffscreenMipRegionResolve(large,request,badArgs,*original).plan);
            auto noRect=request;noRect.rectangle=0;
            if(level)assert(!auditNativeOffscreenMipRegionResolve(large,noRect,args,*original).plan);
            assert(!packNativeOffscreenResolveMipRegion(large,*original,level,d.width+1,d.height,producer,before));
            auto shortProducer=producer;shortProducer.pop_back();
            assert(!packNativeOffscreenResolveMipRegion(large,*original,level,d.width,d.height,shortProducer,before));
            request.rectangle=0;args.rectangle.reset();
            auto committed=plane;std::copy(packed->begin(),packed->end(),committed.begin()+offset);
            assert(std::equal(committed.begin(),committed.begin()+offset,plane.begin()));
            assert(std::equal(committed.begin()+offset+s.sourceBytes,committed.end(),plane.begin()+offset+s.sourceBytes));
            auto bad=request;bad.slice=1;assert(!auditNativeOffscreenResolve(source,bad,args,&*original).plan);
            bad=request;bad.mip=4;assert(!auditNativeOffscreenResolve(source,bad,args,&*original).plan);
            auto wrong=source;++wrong.width;assert(!auditNativeOffscreenResolve(wrong,request,args,&*original).plan);
            assert(!packNativeOffscreenResolveLevel(wrong,*original,level,rgba,before));
            auto shortBytes=before;shortBytes.pop_back();assert(!packNativeOffscreenResolveLevel(source,*original,level,rgba,shortBytes));
            assert(before==Bytes(unchanged.begin()+offset,unchanged.begin()+offset+s.sourceBytes));++cases;
        }
        auto bad=fetch;bad[5]|=0x800;assert(!planNativeOffscreenResolveDestination(*XenosTextureDescription::decode(bad),0));
        bad=fetch;bad[5]=0x10000200;assert(!planNativeOffscreenResolveDestination(*XenosTextureDescription::decode(bad),0));
        bad=fetch;bad[0]|=4;assert(!planNativeOffscreenResolveDestination(*XenosTextureDescription::decode(bad),0));
        bad=fetch;bad[5]&=0xFFF;assert(!planNativeOffscreenResolveDestination(*XenosTextureDescription::decode(bad),0));
        // Legacy single-level callers keep their old contract.
        bad=fetch;bad[4]=0;bad[5]=0x200;
        assert(planNativeOffscreenResolveDestination(*XenosTextureDescription::decode(bad),0));
        assert(!planNativeOffscreenResolveDestination(*XenosTextureDescription::decode(bad),1));
    }
    std::printf("OFFSCREEN_MIP_RESOLVE_PASS cases=%u levels=0..3 independent_address_witness=1 padding_other_levels_preserved=1 top_left_region_row_stride=1 unchanged_source_outside_region=1 negative_cases=1\n",cases);
}
