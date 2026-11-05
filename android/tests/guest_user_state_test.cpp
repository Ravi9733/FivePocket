#include "guest_import_registry.h"
#include <cassert>
using namespace recomp::bootstrap;
int main() {
    GuestImportRegistry registry;
    GuestImportCall call;
    call.returnValue=0xABCD;
    assert(!registry.bindSignedOutUsers());
    GuestAddressSpace space;
    assert(space.reserve());
    assert(registry.initialize(space,0x10000000,0x10000));
    assert(!registry.dispatch("__imp__XamUserGetSigninState",call).handled());
    assert(call.returnValue==0xABCD);
    assert(registry.bindSignedOutUsers());
    for(auto index:{0u,1u,2u,3u,4u,0xFFFFFFFFu}) {
        call.arguments[0]=index;call.returnValue=0xABCD;
        assert(registry.dispatch("__imp__XamUserGetSigninState",call).handled());
        assert(call.returnValue==0);
    }
    registry.reset();
    assert(!registry.bindSignedOutUsers());
    assert(registry.initialize(space,0x10000000,0x10000));
    assert(!registry.dispatch("__imp__XamUserGetSigninState",call).handled());
}
