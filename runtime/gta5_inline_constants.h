#pragma once
// Attempt v80: float constants the game gives the GPU past the device shadow (+1920 vertex, +6016 pixel) and
// past the 823B9FD0 uploads / 823BB448 literal loads that Gta5DriverConstants follows. Two original paths
// (control flow read from the recompiled code, ppc_recomp.18.cpp / .267.cpp / .337.cpp):
//  - 823BDB28(device, bank, start, count) is called only by 823BBBD8, the skinned draw of the geometry
//    class whose vtable is at 82006F44 (slot 5; slot 1 is the plain draw 823B9888, slot 2 the instanced
//    draw 83210990). 823BBBD8 clears the device's vertex dirty bits (device+0) for the blocks from c64,
//    calls 823BDB28(device, 0, 64, (3 * (n + 1)) & ~3) for n = the geometry's palette size (+62), copies
//    the n palette matrices (3 float4 each: matrix set + 32 + 48 * remap[k], remap at geometry+56) to
//    the pointer it returns, then calls the plain draw. 823BDB28 reserves 4 * count + 5 dwords, pads with
//    type-2 NOPs so that the data is 16-byte aligned, writes the type-3 header 0xC0000000 |
//    (4 * count << 16) | 0x5600 | [device+13092] (opcode 0x56, SET_SHADER_CONSTANTS) and the dword
//    offset ((bank + 16) * 256 + start) * 4 (0x4100 for c64), advances the command cursor (device+48)
//    past the data and returns the data pointer. The matrices exist in the command buffer only.
//  - 836B4920(device, bank, start, address, count) is called only by 83210990: LOAD_ALU_CONSTANT
//    (0xC0022F00 | [device+13092]) of `count` float4 from guest memory at `address` to the dword offset
//    (bank * 256 + start) * 4. 83210990 loads c64 onwards from each instance batch, then draws.
// The skinning packet's data is complete only after the caller's copy, so the tracker commits it lazily:
// before the next upload, literal load, GPU load or draw of that device reads or changes the constants.
// That is the GPU's order: the packet precedes the draw's own dirty-block uploads in the command buffer.
#include "gta5_driver_constants.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace recomp::gpu {

enum class InlineConstantsResult {None,Committed,Observed,Unreadable,BadHeader,OutOfRange};
inline const char* inlineConstantsResultName(InlineConstantsResult r) {
    switch(r) {
    case InlineConstantsResult::Committed: return "committed";
    case InlineConstantsResult::Observed: return "observed";   // read, not committed (capture switched off)
    case InlineConstantsResult::Unreadable: return "unreadable";
    case InlineConstantsResult::BadHeader: return "bad-header";
    case InlineConstantsResult::OutOfRange: return "out-of-range";
    default: return "none";
    }
}
// 823BDB28's arguments and its returned data pointer.
struct InlineConstantPacket {std::uint32_t device=0,bank=0,start=0,count=0,data=0;};
// The two dwords 823BDB28 writes right before the data: the type-3 header (predicate bits from
// device+13092 in the low byte are not checked) and the dword offset.
inline bool setShaderConstantsHeaderMatches(std::uint32_t header,std::uint32_t offset,unsigned bank,unsigned start,unsigned count) {
    return (header>>30)==3 && ((header>>8)&0x7F)==0x56 && ((header>>16)&0x3FFF)==count*4 &&
           offset==((bank+16)*256+start)*4;
}
// The register range fits the ALU constant file (512 float4: vertex 0-255, pixel 256-511).
inline bool aluRangeValid(unsigned bank,unsigned start,unsigned count) {
    const unsigned first=bank*256+start;
    return count && bank<2 && start<256 && first<512 && count<=512-first;
}

// View: const std::uint8_t*(std::uint32_t address, std::uint32_t bytes), the guest bytes in place or
// nullptr where the reader refuses. Not thread-safe; the caller serializes (renderer_boundary_trace
// uses it inside inlineGate).
class InlineConstantsTracker {
public:
    struct Stats {
        std::uint64_t packets=0,committed=0,observed=0,unreadable=0,badHeader=0,outOfRange=0,dropped=0,skippedDraws=0,
            gpuLoads=0,gpuCommitted=0,gpuObserved=0,gpuUnreadable=0,gpuOutOfRange=0;
    };
    bool captureSkin=true;     // commit SET_SHADER_CONSTANTS data (diag-generic-no-skin-constants: v71)
    bool captureGpuLoads=true; // commit LOAD_ALU_CONSTANT data (diag-generic-no-gpu-load-constants: v71)

    bool pending()const noexcept{return has_;}
    std::uint32_t pendingDevice()const noexcept{return pending_.device;}
    const InlineConstantPacket& pendingPacket()const noexcept{return pending_;}
    // 823BDB28 returned a data pointer: its caller fills it next. The caller commits an earlier pending
    // packet (flush) before this; one left pending is counted as dropped.
    void reserve(const InlineConstantPacket& p) {
        if(has_){++frame_.dropped;++total_.dropped;}
        pending_=p;has_=p.data!=0;unavailable_=false;
        ++frame_.packets;++total_.packets;
    }
    // Commits the pending packet of `device` (its data now complete) into `constants`.
    template<class View>
    InlineConstantsResult flush(std::uint32_t device,const View& view,Gta5DriverConstants& constants) {
        if(!has_ || pending_.device!=device)return InlineConstantsResult::None;
        has_=false;const auto p=pending_;
        last_=p;drawsSince_=0;words_.clear();
        auto r=InlineConstantsResult::OutOfRange;
        if(!p.count)r=captureSkin?InlineConstantsResult::Committed:InlineConstantsResult::Observed; // 0 bones: nothing to load
        else if(aluRangeValid(p.bank,p.start,p.count) && p.data>=8) {
            const auto bytes=static_cast<std::uint32_t>(8+p.count*16);
            const std::uint8_t* at=view(p.data-8,bytes);
            if(!at)r=InlineConstantsResult::Unreadable;
            else if(!setShaderConstantsHeaderMatches(be(at),be(at+4),p.bank,p.start,p.count))r=InlineConstantsResult::BadHeader;
            else {
                words_.resize(std::size_t{p.count}*4);
                for(std::size_t i=0;i<words_.size();++i)words_[i]=be(at+8+4*i);
                r=!captureSkin?InlineConstantsResult::Observed:
                  constants.loadWords((p.bank*256+p.start)*4,p.count*4,at+8)?InlineConstantsResult::Committed:InlineConstantsResult::OutOfRange;
            }
        }
        lastResult_=r;
        count(r,frame_);count(r,total_);
        // Fail closed: the draw this packet belongs to would read stale bone rows (v71's exploded peds).
        if(captureSkin && r!=InlineConstantsResult::Committed)unavailable_=true;
        return r;
    }
    // 836B4920 after the original: the pending packet first, then the load (its data is in guest memory).
    template<class View>
    InlineConstantsResult gpuLoad(std::uint32_t device,unsigned bank,unsigned start,std::uint32_t address,unsigned count,
                                  const View& view,Gta5DriverConstants& constants) {
        flush(device,view,constants);
        ++frame_.gpuLoads;++total_.gpuLoads;
        auto r=InlineConstantsResult::OutOfRange;
        if(aluRangeValid(bank,start,count)) {
            const std::uint8_t* at=view(address,count*16);
            if(!at)r=InlineConstantsResult::Unreadable;
            else r=!captureGpuLoads?InlineConstantsResult::Observed:
                   constants.loadWords((bank*256+start)*4,count*4,at)?InlineConstantsResult::Committed:InlineConstantsResult::OutOfRange;
        }
        const auto add=[&](Stats& s){
            if(r==InlineConstantsResult::Committed)++s.gpuCommitted;else if(r==InlineConstantsResult::Observed)++s.gpuObserved;
            else if(r==InlineConstantsResult::Unreadable)++s.gpuUnreadable;else ++s.gpuOutOfRange;};
        add(frame_);add(total_);
        return r;
    }
    // A draw consumed the constants: true when it must be skipped (its skinning packet was not committed).
    bool drawSkipped() {
        ++drawsSince_;
        if(!unavailable_)return false;
        unavailable_=false;++frame_.skippedDraws;++total_.skippedDraws;
        return true;
    }
    // The last flushed packet (census): registers, data words (host order) and draws since its flush
    // (0 while the draw right after it runs; drawSkipped() counts the draws).
    const InlineConstantPacket& last()const noexcept{return last_;}
    InlineConstantsResult lastResult()const noexcept{return lastResult_;}
    const std::vector<std::uint32_t>& lastWords()const noexcept{return words_;}
    std::uint64_t drawsSince()const noexcept{return drawsSince_;}
    const Stats& total()const noexcept{return total_;}
    const Stats& frame()const noexcept{return frame_;}
    // The per-frame line (renderer_boundary_trace prints it at the present, like RENDER_DIAG_DRAW_VERTICES).
    std::string frameLine(unsigned long long frames) {
        char b[640];
        std::snprintf(b,sizeof b,"RENDER_DIAG_SKIN_CONSTANTS frames=%llu capture=%u gpu_capture=%u packets=%llu committed=%llu observed=%llu "
            "unreadable=%llu bad_header=%llu out_of_range=%llu dropped=%llu skipped_draws=%llu gpu_loads=%llu gpu_committed=%llu "
            "gpu_observed=%llu gpu_unreadable=%llu gpu_out_of_range=%llu total_packets=%llu total_committed=%llu total_gpu_loads=%llu last=c%u+%u:%s",
            frames,unsigned(captureSkin),unsigned(captureGpuLoads),u(frame_.packets),u(frame_.committed),u(frame_.observed),u(frame_.unreadable),
            u(frame_.badHeader),u(frame_.outOfRange),u(frame_.dropped),u(frame_.skippedDraws),u(frame_.gpuLoads),u(frame_.gpuCommitted),
            u(frame_.gpuObserved),u(frame_.gpuUnreadable),u(frame_.gpuOutOfRange),u(total_.packets),u(total_.committed),u(total_.gpuLoads),
            last_.bank*256+last_.start,last_.count,inlineConstantsResultName(lastResult_));
        frame_=Stats{};
        return b;
    }
private:
    static unsigned long long u(std::uint64_t v){return static_cast<unsigned long long>(v);}
    static std::uint32_t be(const std::uint8_t* p) {
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
    }
    static void count(InlineConstantsResult r,Stats& s) {
        switch(r) {
        case InlineConstantsResult::Committed: ++s.committed;break;
        case InlineConstantsResult::Observed: ++s.observed;break;
        case InlineConstantsResult::Unreadable: ++s.unreadable;break;
        case InlineConstantsResult::BadHeader: ++s.badHeader;break;
        case InlineConstantsResult::OutOfRange: ++s.outOfRange;break;
        default: break;
        }
    }
    InlineConstantPacket pending_{},last_{};bool has_=false,unavailable_=false;
    InlineConstantsResult lastResult_=InlineConstantsResult::None;
    std::vector<std::uint32_t> words_;
    std::uint64_t drawsSince_=~std::uint64_t{0};
    Stats frame_{},total_{};
};

}
