#pragma once
// v279: the inline scene-IRQ replay body (see executeNativeTileSceneInline in native_tile_scene_handoff.h). Pure over guest
// memory (`base` + guest address, big-endian) and the registry's dispatch; GuestImportCall must be declared before this header.
#include <cstdint>
namespace recomp::bootstrap {
inline void putBigEndian32(std::uint8_t* base,std::uint32_t address,std::uint32_t value) {
    auto* p=base+address;p[0]=std::uint8_t(value>>24);p[1]=std::uint8_t(value>>16);p[2]=std::uint8_t(value>>8);p[3]=std::uint8_t(value);
}
inline std::uint32_t getBigEndian32(const std::uint8_t* base,std::uint32_t address) {
    const auto* p=base+address;return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
}
// v279: the inline path is taken only while neither the raised-IRQL lock (+11032) nor the ordinary tile lock (+11156) is
// held (read in the stopped world): the scope refuses an ordinary lock held by its own task, which inline is the delivering
// task (the worker path admits a foreign CPU's holder, v189); otherwise the worker path runs as before.
inline bool nativeSceneInlineEligible(const std::uint8_t* base,unsigned device);
template<class Registry> // (GuestImportRegistry; host tests pass a mock of the scene scope)
inline bool nativeSceneInterruptBody(Registry& registry,std::uint8_t* base,unsigned device,unsigned context) {
    constexpr unsigned Cpu=2,Slot=11576+Cpu*108;
    const auto call=[&](const char* name,std::uint32_t a0,std::uint32_t a1,std::uint32_t a2) {
        GuestImportCall c;c.arguments[0]=a0;c.arguments[1]=a1;c.arguments[2]=a2;
        return registry.dispatch(name,c).handled();
    };
    // sub_836BE1C8(context)
    const auto slot=device+Slot;
    const auto q=getBigEndian32(base,slot+84);
    putBigEndian32(base,slot+92+(q&3)*4,context);
    putBigEndian32(base,slot+84,q+1);
    if(!call("__imp__KeSetEvent",slot+60,1,0))return false;
    // sub_836B1768 after the callback: the CPU2 bit of the scratch mask cleared under the raised-IRQL lock
    const auto lock=device+11032;
    if(!call("__imp__KeAcquireSpinLockAtRaisedIrql",lock,0,0))return false;
    const auto scratch=getBigEndian32(base,device+11028);
#if defined(RECOMP_V279_MUTANT_KEEP_MASK) // test mutant: the mask is left set
    (void)scratch;
#else
    putBigEndian32(base,scratch,(getBigEndian32(base,scratch)&~(1u<<Cpu))&0x3Fu);
#endif
    if(!call("__imp__KeReleaseSpinLockFromRaisedIrql",lock,0,0))return false;
    return true;
}
inline bool nativeSceneInlineEligible(const std::uint8_t* base,unsigned device) {
    return base && device && getBigEndian32(base,device+11032)==0 && getBigEndian32(base,device+11156)==0;
}
} // namespace recomp::bootstrap
