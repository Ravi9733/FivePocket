#pragma once
#include "gta5_inline_draw.h"

namespace recomp::gpu {
using Gta5SubmittedTextureSnapshot=
    std::array<std::optional<std::array<std::uint32_t,6>>,32>;

// Failure diagnostics must retain submitted descriptors even if sampling or
// memory-layout admission rejects them. No CPU-shadow fallback, decoding or
// texture memory reads occur here. Caller holds the exclusive execution gate.
inline Gta5SubmittedTextureSnapshot captureGta5SubmittedTextures(
        const Gta5TextureFetchReader& submitted) {
    Gta5SubmittedTextureSnapshot result{};
    if(submitted)for(unsigned slot=0;slot<result.size();++slot)result[slot]=submitted(slot);
    return result;
}

// 823BA1A8 submits selected six-DWORD fetch descriptors from device+1152.
// Later CPU shadow edits are NOT GPU bindings until another upload includes
// that slot. Retain submission order, including invalid/unbound descriptors.
class Gta5DriverTextures {
public:
    bool upload(std::uint32_t device,std::uint64_t mask,const Gta5ResourceReader& heap) {
        if(!device || !mask || (mask&0xFFFFFFFFULL) || !heap)return false;
        auto candidate=slots_;
        for(unsigned slot=0;slot<32;++slot)if(mask&(std::uint64_t{1}<<(63-slot))) {
            const auto address=std::uint64_t{device}+1152+slot*24;
            if(address+24>0x100000000ULL)return false;
            const auto bytes=heap(static_cast<std::uint32_t>(address),24);
            if(!bytes || bytes->size()!=24)return false;
            std::array<std::uint32_t,6> words{};
            for(unsigned i=0;i<6;++i) {
                const auto* p=bytes->data()+i*4;
                words[i]=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|
                    (std::uint32_t{p[2]}<<8)|p[3];
            }
            candidate[slot]=words;
        }
        slots_=candidate;return true;
    }
    // v274: the same upload with the slots read in place from a span the caller validated as a whole ([spanBase,
    // spanBase+spanBytes) of the guest heap, big-endian words), the other slots through `heap` as upload() reads them.
    // Same words, same failures, no per-slot copy (v266 render-thread profile: a std::vector per slot was 1.3% of the thread).
    // v362 (exact): `staged` keeps only the set slots' words until every slot has been read, then stores them in place: the same
    // all-or-nothing result as the whole-array candidate (two 896-byte copies of the 32 optional slots per call, v349: the
    // texture-fetch hook's memcpy ~0.3% of the render thread). Same reads in the same order, same failures.
    bool uploadFromSpan(std::uint32_t device,std::uint64_t mask,const std::uint8_t* span,std::uint32_t spanBase,std::uint32_t spanBytes,
                        const Gta5ResourceReader& heap,bool staged=false) {
        if(!device || !mask || (mask&0xFFFFFFFFULL) || !heap)return false;
        if(staged) {
            std::array<std::array<std::uint32_t,6>,32> words; // (only the set slots are written and read)
            for(unsigned slot=0;slot<32;++slot)if(mask&(std::uint64_t{1}<<(63-slot))) {
                const auto address=std::uint64_t{device}+1152+slot*24;
                if(address+24>0x100000000ULL)return false;
                const std::uint8_t* p=nullptr;std::optional<std::vector<std::uint8_t>> bytes;
                if(span && address>=spanBase && address+24<=std::uint64_t{spanBase}+spanBytes)p=span+(address-spanBase);
                else {bytes=heap(static_cast<std::uint32_t>(address),24);if(!bytes || bytes->size()!=24)return false;p=bytes->data();}
                for(unsigned i=0;i<6;++i){const auto* q=p+i*4;words[slot][i]=(std::uint32_t{q[0]}<<24)|(std::uint32_t{q[1]}<<16)|(std::uint32_t{q[2]}<<8)|q[3];}
#if defined(RECOMP_V362_MUTANT_PARTIAL_COMMIT) // test mutant: each slot stored as soon as it is read (a later failure leaves it)
                slots_[slot]=words[slot];
#endif
            }
            for(unsigned slot=0;slot<32;++slot)if(mask&(std::uint64_t{1}<<(63-slot)))slots_[slot]=words[slot];
            return true;
        }
        auto candidate=slots_;
        for(unsigned slot=0;slot<32;++slot)if(mask&(std::uint64_t{1}<<(63-slot))) {
            const auto address=std::uint64_t{device}+1152+slot*24;
            if(address+24>0x100000000ULL)return false;
            const std::uint8_t* p=nullptr;std::optional<std::vector<std::uint8_t>> bytes;
            if(span && address>=spanBase && address+24<=std::uint64_t{spanBase}+spanBytes)p=span+(address-spanBase);
            else {bytes=heap(static_cast<std::uint32_t>(address),24);if(!bytes || bytes->size()!=24)return false;p=bytes->data();}
            std::array<std::uint32_t,6> words{};
            for(unsigned i=0;i<6;++i){const auto* q=p+i*4;words[i]=(std::uint32_t{q[0]}<<24)|(std::uint32_t{q[1]}<<16)|(std::uint32_t{q[2]}<<8)|q[3];}
            candidate[slot]=words;
        }
        slots_=candidate;return true;
    }
    std::optional<std::array<std::uint32_t,6>> fetch(unsigned slot)const {
        return slot<32?slots_[slot]:std::nullopt;
    }
private:
    Gta5SubmittedTextureSnapshot slots_{};
};
}
