#include "guest_import_registry.h"
#include <algorithm>
#include <array>
#include <cassert>

using namespace recomp::bootstrap;
int main() {
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry;
    assert(!registry.bindUserLanguage(1));
    assert(!registry.bindUserCountry(103));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    constexpr auto Name = "__imp__ExGetXConfigSetting";
    const auto& imports = registry.implementedBootImports();
    auto descriptor = std::find_if(imports.begin(), imports.end(), [](auto d) { return d.name == Name; });
    assert(descriptor != imports.end() && descriptor->writesReturnValue);
    constexpr std::uint32_t Out = 0x3000, Size = 0x3012;
    auto* memory = space.translate(Out);
    auto fill = [&] { std::fill_n(memory - 4, 32, 0xA5); };
    auto unchanged = [&] { assert(std::all_of(memory - 4, memory + 28, [](auto b) { return b == 0xA5; })); };
    auto call = [&](std::uint32_t category, std::uint32_t setting, std::uint32_t output,
                    std::uint32_t bytes, std::uint32_t required, bool expected) {
        GuestImportCall c; c.arguments = {category, setting, output, bytes, required, 0, 0, 0};
        c.returnValue = 0xABCDEF0123456789;
        assert(registry.dispatch(Name, c).handled() == expected);
        assert(c.returnValue == (expected ? 0 : 0xABCDEF0123456789));
    };
    fill(); call(3, 10, Out, 4, Size, false); unchanged();
    assert(registry.bindVirtualVideoMode720p60());
    fill(); call(3, 9, Out, 4, Size, false); unchanged();
    assert(!registry.bindUserLanguage(0) && !registry.bindUserLanguage(10) && !registry.bindUserLanguage(18));
    assert(registry.bindUserLanguage(1));
    assert(!registry.bindUserLanguage(2));
    call(3, 9, Out, 4, Size, true);
    assert(memory[0] == 0 && memory[1] == 0 && memory[2] == 0 && memory[3] == 1);
    assert(memory[18] == 0 && memory[19] == 4 && memory[20] == 0xA5);
    fill(); call(3, 9, 0, 0, Size, true);
    assert(memory[18] == 0 && memory[19] == 4 && memory[0] == 0xA5);
    fill(); call(3, 14, Out, 1, Size, false); unchanged();
    for (auto bad : {0U, 17U, 94U, 110U}) assert(!registry.bindUserCountry(bad));
    assert(registry.bindUserCountry(103) && !registry.bindUserCountry(53));
    call(3, 14, Out + 1, 1, Size, true);
    assert(memory[0] == 0xA5 && memory[1] == 103 && memory[2] == 0xA5);
    assert(memory[18] == 0 && memory[19] == 1 && memory[20] == 0xA5);
    fill(); call(3, 14, 0, 0, Size, true);
    assert(memory[0] == 0xA5 && memory[18] == 0 && memory[19] == 1);
    fill(); call(3, 14, Out, 4, Size, false); unchanged();
    fill(); call(3, 14, Out, 1, Out, false); unchanged();
    fill();
    call(3, 10, Out, 4, Size, true);
    for (int i = -4; i < 28; ++i) {
        const auto expected = i == 0 || i == 2 || i == 3 || i == 18 ? 0 : i == 1 || i == 19 ? 4 : 0xA5;
        assert(memory[i] == expected); // Exactly four data bytes and TWO size bytes.
    }
    fill(); call(3, 10, 0, 0, Size, true);
    assert(memory[18] == 0 && memory[19] == 4);
    assert(std::all_of(memory, memory + 4, [](auto b) { return b == 0xA5; }));
    fill(); call(3, 10, Out, 4, 0, true);
    assert(memory[18] == 0xA5 && memory[19] == 0xA5);
    fill(); call(3, 10, 0, 0, 0, true); unchanged();
    for (auto wrong : {0U, 2U, 4U, 0x10003U}) { fill(); call(wrong, 10, Out, 4, Size, false); unchanged(); }
    for (auto wrong : {0U, 8U, 11U, 0x1000AU}) { fill(); call(3, wrong, Out, 4, Size, false); unchanged(); }
    for (auto wrong : {0U, 1U, 3U, 5U, 0x10004U}) { fill(); call(3, 10, Out, wrong, Size, false); unchanged(); }
    fill(); call(3, 10, 0, 4, Size, false); unchanged();
    for (auto bad : {Out + 1, 0xFFFFFFFEU, 0x7FC80000U, 0x7FC8FFFCU}) {
        fill(); call(3, 10, bad, 4, Size, false); unchanged();
    }
    for (auto bad : {Size + 1, Out, Out + 2, 0xFFFFFFFFU, 0x7FC80000U}) {
        fill(); call(3, 10, Out, 4, bad, false); unchanged();
    }
    // Adjacent outputs are allowed, overlapping outputs are transactional failures.
    call(3, 10, Out, 4, Out + 4, true);
    call(3, 10, Out, 4, Out - 2, true);
    call(3, 10, 0xFFFFFFFCU, 4, Size, true);
    call(3, 10, Out, 4, 0xFFFFFFFEU, true);
    assert(space.translate(0xFFFFFFFEU)[0] == 0 && space.translate(0xFFFFFFFEU)[1] == 4);
    assert(registry.bindGuestThreadToken(0x5000));
    assert(registry.bindGuestThreadTlsStorage(0x5000, 0x6000, 4));
    fill(); call(3, 10, 0x6000, 4, Size, false); unchanged();
    fill(); call(3, 10, Out, 4, 0x6000, false); unchanged();
    registry.reset();
    assert(registry.initialize(space, 0x10000000, 0x10000));
    fill(); call(3, 10, Out, 4, Size, false); unchanged();
    fill(); call(3, 9, Out, 4, Size, false); unchanged();
    fill(); call(3, 14, Out, 1, Size, false); unchanged();
    assert(registry.bindUserLanguage(2));
    call(3, 9, Out, 4, Size, true);
    assert(memory[3] == 2); // Language binding works independently of video.
}
