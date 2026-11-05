#pragma once
#include "color8_clear_execution_fixture.h"
#include "native_single_color8_resolve.h"
#include "native_resolve_failure_capture.h"
#include "native_color8_resolve.h"
#include "xenos_color_copy.h"

struct SingleColor8ResolveFixture {
    using Bytes=std::vector<std::uint8_t>;
    Color8ClearExecutionFixture clear;
    recomp::gpu::Gta5Color8ClearAudit audit;
    Bytes state,header;
    recomp::gpu::Gta5BoundSurfaceCapture bindings;
    recomp::gpu::XenosTextureDescription destination;
    recomp::gpu::NativeOffscreenResolveRequest request;
    recomp::gpu::NativeOffscreenResolveArguments arguments;
    explicit SingleColor8ResolveFixture(const std::string& path):clear(path+"/clear"),audit(recomp::gpu::auditGta5Color8Clear(clear.capture)) {
        const auto read=[&](const auto& n){return clear.bytes(path+"/native-resolve-failure-"+n+".bin");};
        state=read(std::string("state"));header=read(std::string("surface-0"));
        assert(!audit.issues && header==*clear.capture.surfaces[0].header);
        for(unsigned i=0;i<5;++i) {
            auto& b=bindings.surfaces[i];b.object=clear.word(state,12816+4*i);b.tileCacheObject=clear.word(state,13104+4*i);
            if(i==0)b.header=header;else assert(!b.object);
        }
        const auto d=recomp::gpu::describeGta5TextureHeader(read(std::string("destination-header")));assert(d);destination=*d;
        const auto call=clear.bytes(path+"/resolve-call.bin");assert(call.size()==40);
        request={clear.word(call,0),clear.word(call,8),clear.word(call,12),clear.word(call,20),clear.word(call,24),clear.word(call,28),clear.word(call,32)};
        assert(!request.flags && request.rectangle && !request.destinationPoint && !request.mip && !request.slice && !request.clearColor);
        arguments.rectangle=read(std::string("rectangle"));arguments.stack92=read(std::string("stack92"));arguments.stack100=read(std::string("stack100"));
    }
    auto plan(bool owned=true,unsigned display=0x10059AE0,bool uniform=true)const {
        return recomp::gpu::planNativeSingleColor8Resolve(audit.plan,owned,display,uniform,state,bindings,header,destination,request,arguments);
    }
    Bytes prior()const {
        const auto p=plan();assert(p);Bytes b(p->destination.storage.sourceBytes);
        for(std::size_t i=0;i<b.size();++i)b[i]=static_cast<std::uint8_t>(i*17+0xA5);return b;
    }
    void verifyPixels(const Bytes& rgba,const Bytes& before,const Bytes& after)const {
        const auto p=plan();assert(p);const auto& d=p->destination.storage.description;
        auto expected=before;std::vector<bool> touched(before.size(),false);
        constexpr unsigned endianXor[]{0,1,3,2};
        for(unsigned y=0;y<d.height;++y)for(unsigned x=0;x<d.width;++x) {
            unsigned address=0;assert(recomp::gpu::colorCopyOffset(d.pitchTexels,(d.height+31)&~31u,x,y,address));
            for(unsigned c=0;c<4;++c) {
                const auto at=(address+c)^endianXor[d.endian];assert(at<expected.size() && !touched[at]);touched[at]=true;
                expected[at]=rgba[(y*d.width+x)*4+(d.swizzle[0]==2 && c<3?2-c:c)];
            }
        }
        assert(after==expected); // Includes every untouched padding byte.
    }
};
