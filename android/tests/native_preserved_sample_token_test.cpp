#include "native_preserved_sample_token.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace recomp::gpu;
int main() {
    int image=0,other=0;
    NativePreservedSampleToken token{&image,0x1000,7,3};
    assert(token.matches(&image,0x1000,7,3));
    assert(!token.matches(&other,0x1000,7,3));
    assert(!token.matches(&image,0x2000,7,3));
    assert(!token.matches(&image,0x1000,8,3));
    assert(!token.matches(&image,0x1000,7,4));
    assert(!NativePreservedSampleToken{}.matches(&image,0x1000,7,3));
    for(auto bad:{NativePreservedSampleToken{nullptr,0x1000,7,3},
                 NativePreservedSampleToken{&image,0,7,3},
                 NativePreservedSampleToken{&image,0x1000,0,3},
                 NativePreservedSampleToken{&image,0x1000,7,0},
                 NativePreservedSampleToken{&image,0x1000,7,UINT64_MAX}})
        assert(!bad.matches(bad.image,bad.object,bad.contentSerial,bad.mutationSerial));
    std::puts("NATIVE_PRESERVED_SAMPLE_TOKEN_PASS changed_identity_content_mutation_rejected=1");
}
