#pragma once
// v318: helpers of the input-to-display latency probe (pure; tests/latency_probe_v318_test.cpp). The probe itself records
// timeline events (diag_timeline.h InputEdge .. Displayed) only while the timeline is armed.
namespace v318 {
// The game-visible content of two gamepad states, without the packet number (the synthetic reader renumbers every read, so
// the packet alone would report a change on every poll).
template<class S> bool sameInput(const S& a,const S& b) {
#if defined(RECOMP_V318_MUTANT_PACKET)
    if(a.packet!=b.packet)return false;
#endif
    return a.buttons==b.buttons && a.leftTrigger==b.leftTrigger && a.rightTrigger==b.rightTrigger &&
           a.leftX==b.leftX && a.leftY==b.leftY && a.rightX==b.rightX && a.rightY==b.rightY;
}
// The start (ms after the sequence began) of the step active `at` ms after the sequence began; -1 outside the sequence.
template<class Steps> long long stepStartMs(const Steps& steps,long long at) {
    if(at<0)return -1;
    long long edge=0;
    for(const auto& s:steps) {
#if defined(RECOMP_V318_MUTANT_EDGE)
        if(at<edge+s.ms)return edge+s.ms;
#else
        if(at<edge+s.ms)return edge;
#endif
        edge+=s.ms;
    }
    return -1;
}
}
