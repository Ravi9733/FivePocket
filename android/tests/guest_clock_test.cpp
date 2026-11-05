#include "guest_clock.h"
#include "guest_import_registry.h"

#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;
int main() {
    assert(fileTimeFromFields({1601,1,1,0,0,0,0,-1}) == 0);
    assert(fileTimeFromFields({1970,1,1,0,0,0,0,7}) == 116444736000000000ULL);
    assert(fileTimeFromFields({2000,2,29,0,0,0,0,0}) == 125962560000000000ULL);
    assert(fileTimeFromFields({2013,9,17,0,0,0,0,0}) == 130238496000000000ULL);
    assert(fileTimeFromFields({9999,12,31,23,59,59,999,0}) == 2650467743999990000ULL);
    assert(!fileTimeFromFields({32767,12,31,0,0,0,0,0})); // Positive LARGE_INTEGER bound.
    assert(!fileTimeFromFields({1900,2,29,0,0,0,0,0}) && !fileTimeFromFields({2100,2,29,0,0,0,0,0}));
    // An independent day-by-day counter covers one full Gregorian 400-year cycle.
    std::uint64_t days = 0;
    for (int year = 1601; year <= 2000; ++year) {
        for (int month = 1; month <= 12; ++month) {
            int length = 31;
            if (month == 4 || month == 6 || month == 9 || month == 11) { length = 30; }
            if (month == 2) { length = 28 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)); }
            for (int day = 1; day <= length; ++day, ++days) {
                assert(fileTimeFromFields({year,month,day,0,0,0,0,99}) == days * 864000000000ULL);
            }
            assert(!fileTimeFromFields({year,month,length + 1,0,0,0,0,0}));
        }
    }
    assert(days == 146097);
    const std::array<std::int32_t, 8> valid{2013,9,17,12,34,56,789,0};
    for (std::size_t i = 0; i < 7; ++i) {
        auto invalid = valid; invalid[i] = -1; assert(!fileTimeFromFields(invalid));
        invalid[i] = 32768; assert(!fileTimeFromFields(invalid));
    }
    assert(fileTimeFromUnixTicks(0) == static_cast<std::uint64_t>(FileTimeUnixEpoch));
    assert(fileTimeFromUnixTicks(-FileTimeUnixEpoch) == 0);
    assert(!fileTimeFromUnixTicks(-FileTimeUnixEpoch - 1));
    assert(fileTimeFromUnixTicks(INT64_MAX - FileTimeUnixEpoch) == static_cast<std::uint64_t>(INT64_MAX));
    assert(!fileTimeFromUnixTicks(INT64_MAX - FileTimeUnixEpoch + 1));
    assert(advanceFileTime(100, 23) == 123 && !advanceFileTime(100, -1));
    assert(!advanceFileTime(INT64_MAX, 1) && !advanceFileTime(UINT64_MAX, 0));
    GuestAddressSpace space;
    assert(space.reserve());
    GuestImportRegistry registry;
    std::optional<std::uint64_t> ticks = 0x123456789ABCDEFULL;
    const auto source = [&] { return ticks; };
    assert(!registry.bindClockSources(source, 1));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    constexpr std::uint32_t Out = 0x30000000;
    constexpr std::uint64_t Sentinel = 0xAAAABBBBCCCCDDDDULL;
    GuestImportCall c;
    c.arguments[0] = Out; c.returnValue = Sentinel;
    assert(!registry.dispatch("__imp__KeQuerySystemTime", c).handled());
    assert(!registry.dispatch("__imp__KeQueryPerformanceFrequency", c).handled());
    assert(!registry.bindClockSources({}, 1) && !registry.bindClockSources(source, 0));
    assert(registry.bindClockSources(source, 1234567));
    assert(!registry.bindClockSources(source, 1234567));
    assert(registry.dispatch("__imp__KeQuerySystemTime", c).handled() && c.returnValue == Sentinel);
    const std::uint8_t expected[]{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    assert(std::memcmp(space.translate(Out), expected, 8) == 0);
    for (const auto bad : {Out + 1, 0xFFFFFFFCU}) {
        c.arguments[0] = bad;
        assert(!registry.dispatch("__imp__KeQuerySystemTime", c).handled());
        assert(c.returnValue == Sentinel);
    }
    c.arguments[0] = 0;
    assert(registry.dispatch("__imp__KeQuerySystemTime", c).handled());
    c.arguments[0] = Out;
    ticks = std::nullopt;
    assert(!registry.dispatch("__imp__KeQuerySystemTime", c).handled());
    ticks = UINT64_MAX;
    assert(!registry.dispatch("__imp__KeQuerySystemTime", c).handled());
    assert(std::memcmp(space.translate(Out), expected, 8) == 0 && c.returnValue == Sentinel);
    assert(registry.dispatch("__imp__KeQueryPerformanceFrequency", c).handled() && c.returnValue == 1234567);
    registry.reset();
    assert(registry.initialize(space, 0x10000000, 0x10000));
    assert(!registry.dispatch("__imp__KeQueryPerformanceFrequency", c).handled());
    constexpr std::uint32_t Fields = Out + 0x100;
    auto* input = space.translate(Fields);
    for (unsigned i = 0; i < valid.size(); ++i) { input[2*i] = valid[i] >> 8; input[2*i+1] = valid[i]; }
    input[14] = input[15] = 0xFF; // Weekday ignored.
    std::memset(space.translate(Out), 0xA5, 16);
    c = {}; c.arguments = {Fields, Out, 0, 0, 0, 0, 0, 0};
    assert(registry.dispatch("__imp__RtlTimeFieldsToTime", c).handled() && c.returnValue == 1);
    const auto expectedTicks = *fileTimeFromFields(valid);
    for (unsigned i = 0; i < 8; ++i) { assert(space.translate(Out)[i] == static_cast<std::uint8_t>(expectedTicks >> (56 - 8*i))); }
    assert(space.translate(Out)[8] == 0xA5);
    input[3] = 13; std::memset(space.translate(Out), 0xA5, 8);
    assert(registry.dispatch("__imp__RtlTimeFieldsToTime", c).handled() && c.returnValue == 0);
    for (unsigned i = 0; i < 8; ++i) { assert(space.translate(Out)[i] == 0xA5); }
    input[3] = 9;
    for (const auto& pair : {std::pair{0U, Out}, {Fields+1, Out}, {Fields, 0U}, {Fields, Out+1},
                            {Fields, Fields}, {0xFFFFFFFEU, Out}, {Fields, 0xFFFFFFFCU}}) {
        c = {}; c.arguments[0] = pair.first; c.arguments[1] = pair.second; c.returnValue = Sentinel;
        assert(!registry.dispatch("__imp__RtlTimeFieldsToTime", c).handled() && c.returnValue == Sentinel);
    }
}
