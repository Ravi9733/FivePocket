#pragma once
#include "gta5_logical_tile_clear_fixture.h"
#include "gta5_logical_tile_resolve.h"

namespace recomp::gpu::test {
// Authored API arguments/destination. An optional supplied capture retains its
// actual state/header provenance, but never makes these resolve arguments live.
struct AuthoredTileResolve {
    Gta5ClearCapture surfaces;
    NativeOffscreenResolveRequest request;
    NativeOffscreenResolveArguments arguments;
    XenosTextureDescription destination;
};
inline AuthoredTileResolve authoredTileResolve(Gta5ClearCapture surfaces=authoredLogicalClear()) {
    AuthoredTileResolve f;f.surfaces=std::move(surfaces);
    f.request={0x70010000,0x110,0x70020000,0x70020020,0,0,0x70020040};
    f.arguments.rectangle=std::vector<std::uint8_t>(16);f.arguments.point=std::vector<std::uint8_t>(8);
    f.arguments.color=std::vector<std::uint8_t>(16);f.arguments.stack92=std::vector<std::uint8_t>(4);
    f.arguments.stack100=std::vector<std::uint8_t>(4);
    tileWord(*f.arguments.rectangle,8,1280);tileWord(*f.arguments.rectangle,12,384);
    tileWord(*f.surfaces.state,13084,3);
    for(unsigned i=0;i<4;++i)tileFloat(*f.arguments.color,i*4,std::array<float,4>{0.25f,0.5f,0.75f,1.f}[i]);
    const auto decoded=XenosTextureDescription::decode({2u|(40u<<22)|0x80000000u,
        0x1000006,1279|(719<<13),0xD10,0,0x200});assert(decoded);f.destination=*decoded;
    return f;
}
inline std::optional<Gta5LogicalTileResolve> tileResolvePlan(const AuthoredTileResolve& f) {
    return planGta5LogicalTileResolve(f.surfaces,f.request,f.arguments,f.destination);
}
}
