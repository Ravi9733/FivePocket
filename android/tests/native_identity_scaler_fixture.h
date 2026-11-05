#pragma once
#include "native_identity_scaler.h"

namespace recomp::gpu::test {
// Synthetic fixed-profile fixture generated from dimensions/tap/phase rules.
// No copied game record, addresses, assets or command payloads.
inline NativeIdentityScalerCandidate::Inputs identityScalerInputs() {
    return {{0,(1280U<<16)|720,0,(1280U<<16)|720,(1280U<<16)|720,7,7},{}};
}
inline std::array<std::uint8_t,NativeIdentityScalerCandidate::RecordBytes> identityScalerRecord() {
    using Candidate=NativeIdentityScalerCandidate;
    std::array<std::uint8_t,Candidate::RecordBytes> bytes{};
    const auto put=[&](unsigned i,std::uint32_t word) {
        for (unsigned b=0;b<4;++b) bytes[i*4+b]=word>>(24-b*8);
    };
    for (unsigned axis=0;axis<2;++axis) {
        const auto base=axis*Candidate::AxisWords;const unsigned taps=axis?10:5;
        put(base,taps);put(base+1,0x3D800000);put(base+162,1U<<19);
        for (unsigned phase=0;phase<16;++phase) put(base+2+(taps-1)*8+phase,1U<<9);
    }
    const auto controls=NativeIdentityScalerPlan::expectedControls();
    for (unsigned i=0;i<controls.size();++i) put(2*Candidate::AxisWords+i,controls[i]);
    return bytes;
}
}
