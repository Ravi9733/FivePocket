#include "native_indexed_lod_plan.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace recomp::gpu;
int main() {
    const std::array<float,4> viewport{.625f,-.703125f,-.375f,-.296875f};
    const auto legacy=NativeIndexedLodRaster::LegacyBatches,windows=NativeIndexedLodRaster::ClippedWindows;
    const std::vector<std::array<float,4>> good{{0,0,.5,1},{1,1,.75,1},{-1,-1,-.5,-1}};
    const char* why=nullptr;
    assert(validateNativeIndexedLodPositions(good,viewport,windows,&why));
    assert(!validateNativeIndexedLodPositions(good,viewport,legacy,&why) && std::strcmp(why,"indexed-lod-position-w")==0);
    assert(validateNativeIndexedLodPositions(std::span(good).first(2),viewport,legacy));
    unsigned negatives=0;
    for(unsigned lane=0;lane<4;++lane)for(float v:{std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        auto bad=good;bad.back()[lane]=v;assert(!validateNativeIndexedLodPositions(bad,viewport,windows));++negatives;
    }
    for(float w:{0.f,-0.f,std::numeric_limits<float>::denorm_min()}) {
        auto bad=good;bad.back()[3]=w;assert(!validateNativeIndexedLodPositions(bad,viewport,windows));++negatives;
    }
    assert(!validateNativeIndexedLodPositions({},viewport,windows));
    assert(!validateNativeIndexedLodPositions(good,viewport,static_cast<NativeIndexedLodRaster>(99)));
    auto bad=good;bad.back()[0]=std::numeric_limits<float>::max();auto transform=viewport;transform[0]=2;
    assert(!validateNativeIndexedLodPositions(bad,transform,windows,&why));
    assert(std::strcmp(why,"indexed-lod-position-viewport")==0);
    transform=viewport;transform[2]=std::numeric_limits<float>::infinity();
    assert(!validateNativeIndexedLodPositions(good,transform,windows));
    std::printf("INDEXED_LOD_POSITIONS_PASS mixed_w_valid=1 legacy_negative_w_refused=1 nonfinite_zero_divide=%u transform_overflow_refused=1\n",negatives);
}
