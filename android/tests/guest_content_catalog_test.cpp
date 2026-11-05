#include "guest_import_registry.h"
#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;
namespace {
void write(std::uint8_t* data, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) { data[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i)); }
}
std::uint32_t word(const std::uint8_t* data) {
    return (std::uint32_t{data[0]} << 24) | (std::uint32_t{data[1]} << 16) | (std::uint32_t{data[2]} << 8) | data[3];
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> bytes(16 + 2 * 308);
    std::memcpy(bytes.data(), "GXCONT1", 7); write(bytes.data() + 8, 0x12345678); write(bytes.data() + 12, 2);
    for (unsigned i = 0; i < 2; ++i) {
        auto* record = bytes.data() + 16 + i * 308;
        write(record, 1); write(record + 4, 2); record[9] = 'A'; record[264] = '0' + i;
    }
    return bytes;
}
GuestImportCall create(std::uint32_t count = 1) {
    GuestImportCall call; call.arguments = {0xFE, 0, 2, 0, count, 0x2000, 0x2004, 0}; call.returnValue = 0xABCD; return call;
}
GuestImportCall enumerate(std::uint32_t handle, std::uint32_t count = 1) {
    GuestImportCall call; call.arguments = {handle, 0, 0x10000, count * 308, 0x2008, 0, 0, 0}; call.returnValue = 0xABCD; return call;
}
class File final : public GuestReadOnlyFile {
public:
    [[nodiscard]] std::uint64_t size() const noexcept override { return 16; }
    [[nodiscard]] bool read(std::uint64_t offset, std::uint8_t* out, std::size_t count) const noexcept override {
        if (offset > 16 || count > 16 - offset) { return false; }
        for (std::size_t i = 0; i < count; ++i) { out[i] = static_cast<std::uint8_t>(0x30 + offset + i); }
        return true;
    }
};
GuestFileMetadataCatalog fileCatalog() {
    std::vector<std::uint8_t> bytes{'G','F','M','E','T','A','1',0};
    const auto append = [&](std::uint32_t value) {
        const auto offset = bytes.size(); bytes.resize(offset + 4); write(bytes.data() + offset, value);
    };
    append(2); append(8);
    const std::string mount = "content:"; bytes.insert(bytes.end(), mount.begin(), mount.end());
    for (const auto& path : {std::string("content:\\"), std::string("content:\\f.bin")}) {
        append(static_cast<std::uint32_t>(path.size())); bytes.insert(bytes.end(), path.begin(), path.end());
        for (unsigned i = 0; i < 4; ++i) { append(0); append(i + 1); }
        const bool directory = path.back() == '\\';
        append(0); append(directory ? 0 : 2048); append(0); append(directory ? 0 : 16); append(directory ? 0x11 : 1);
    }
    GuestFileMetadataCatalog result; assert(result.load(bytes)); return result;
}
GuestImportCall mount() {
    GuestImportCall call; call.stackPointer = 0x8000;
    call.arguments = {0xFF, 0x2100, 0x2200, 3, 0x200C, 0, 0, 0}; call.returnValue = 0xABCD; return call;
}
}
int main() {
    const auto bytes = fixture();
    GuestContentCatalog catalog;
    assert(catalog.load(bytes) && catalog.title() == 0x12345678 && catalog.records().size() == 2);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        assert(!catalog.load({bytes.begin(), bytes.begin() + size}) && catalog.title() == 0 && catalog.records().empty());
    }
    for (const auto offset : {0U, 12U, 19U, 23U, 16U + 264, 16U + 305}) {
        auto bad = bytes; bad[offset] = 0xFF; assert(!catalog.load(bad));
    }
    auto bad = bytes; write(bad.data() + 8, 0); assert(!catalog.load(bad));
    bad = bytes; bad.push_back(0); assert(!catalog.load(bad));
    bad = bytes; bad[16 + 308 + 264] = '0'; assert(!catalog.load(bad));
    bad = bytes; bad[16 + 8] = 0xD8; bad[16 + 9] = 0; assert(!catalog.load(bad));
    bad = bytes; bad[16 + 8] = 0xDC; bad[16 + 9] = 0; assert(!catalog.load(bad));
    bad = bytes; bad[16 + 13] = 'B'; assert(!catalog.load(bad));
    bad = bytes; bad[16 + 8] = 0xD8; bad[16 + 9] = 0; bad[16 + 10] = 0xDC; assert(catalog.load(bad));
    assert(catalog.load(bytes));

    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry;
    assert(!registry.bindInstalledContent(catalog, 0x12345678));
    assert(registry.initialize(space, 0x40000, 0x10000));
    auto c = create(); assert(!registry.dispatch("__imp__XamContentCreateEnumerator", c).handled() && c.returnValue == 0xABCD);
    assert(!registry.bindInstalledContent(catalog, 0x12345679));
    assert(registry.bindInstalledContent(catalog, 0x12345678));
    assert(!registry.bindInstalledContent(catalog, 0x12345678));
    // Registry owns an immutable copy; a failed caller reload cannot clear it.
    assert(!catalog.load({}));
    for (const auto& change : {std::pair{0U, 0U}, {1U, 2U}, {2U, 1U}, {3U, 1U}, {4U, 0U}, {4U, 65U},
                             {5U, 0U}, {5U, 0x2004U}, {5U, 0xFFFFFFFEU}, {6U, 0x2005U}}) {
        c = create(); c.arguments[change.first] = change.second;
        std::memset(space.translate(0x2000), 0xA5, 12);
        assert(!registry.dispatch("__imp__XamContentCreateEnumerator", c).handled() && c.returnValue == 0xABCD);
        assert(word(space.translate(0x2000)) == 0xA5A5A5A5 && word(space.translate(0x2004)) == 0xA5A5A5A5);
    }
    assert(registry.bindGuestThreadToken(0x30000));
    assert(registry.bindGuestThreadTlsStorage(0x30000, 0x30004, 16));
    c = create(); c.arguments[6] = 0x30004;
    assert(!registry.dispatch("__imp__XamContentCreateEnumerator", c).handled());
    c = create(); assert(registry.dispatch("__imp__XamContentCreateEnumerator", c).handled() && c.returnValue == 0);
    assert(word(space.translate(0x2000)) == 308);
    const auto first = word(space.translate(0x2004));
    c = create(2); c.arguments[1] = 1;
    assert(registry.dispatch("__imp__XamContentCreateEnumerator", c).handled() && c.returnValue == 0);
    const auto second = word(space.translate(0x2004));
    assert(first != second && word(space.translate(0x2000)) == 616);

    for (const auto& change : {std::pair{1U, 1U}, {2U, 0U}, {2U, 0xFFFFFFFCU}, {3U, 65U * 308},
                             {4U, 0U}, {4U, 0x10004U}, {4U, 0x30004U}, {5U, 1U}}) {
        c = enumerate(first); c.arguments[change.first] = change.second;
        std::memset(space.translate(0x10000), 0xA5, 620); write(space.translate(0x2008), 0xA5A5A5A5);
        assert(!registry.dispatch("__imp__XamEnumerate", c).handled() && c.returnValue == 0xABCD);
        assert(word(space.translate(0x2008)) == 0xA5A5A5A5 && word(space.translate(0x10000)) == 0xA5A5A5A5);
    }
    c = enumerate(first); c.arguments[3] = 307;
    assert(registry.dispatch("__imp__XamEnumerate", c).handled() && c.returnValue == 122 && word(space.translate(0x2008)) == 0);
    for (unsigned i = 0; i < 2; ++i) {
        c = enumerate(first); c.arguments[1] = i == 0 ? 0 : 0x20;
        assert(registry.dispatch("__imp__XamEnumerate", c).handled() && c.returnValue == 0);
        assert(word(space.translate(0x2008)) == 1);
        assert(std::memcmp(space.translate(0x10000), bytes.data() + 16 + i * 308, 308) == 0);
        assert(word(space.translate(0x10000 + 308)) == 0xA5A5A5A5);
    }
    c = enumerate(first); assert(registry.dispatch("__imp__XamEnumerate", c).handled() && c.returnValue == 18 && word(space.translate(0x2008)) == 0);
    c = enumerate(second, 2); assert(registry.dispatch("__imp__XamEnumerate", c).handled() && c.returnValue == 0);
    assert(word(space.translate(0x2008)) == 2 && std::memcmp(space.translate(0x10000), bytes.data() + 16, 616) == 0);
    c = {}; c.arguments[0] = first; assert(!registry.dispatch("__imp__NtWaitForSingleObjectEx", c).handled());
    c = {}; c.arguments[0] = first; assert(registry.dispatch("__imp__NtClose", c).handled() && c.returnValue == 0);
    c = enumerate(first); assert(registry.dispatch("__imp__XamEnumerate", c).handled() && c.returnValue == 6 && word(space.translate(0x2008)) == 0);

    std::memcpy(space.translate(0x2200), bytes.data() + 16, 308);
    std::memcpy(space.translate(0x2100), "1pack", 6); write(space.translate(0x8054), 0);
    c = mount(); assert(!registry.dispatch("__imp__XamContentCreateEx", c).handled() && c.returnValue == 0xABCD);
    const auto metadata = fileCatalog();
    GuestImportRegistry::ContentFiles sources{{"content:\\f.bin", std::make_shared<const File>()}};
    assert(!registry.bindContentPackage(0, metadata, sources));
    assert(!registry.bindContentPackage(3, metadata, sources));
    assert(!registry.bindContentPackage(1, metadata, {}));
    auto extra = sources; extra.emplace("content:\\unknown", std::make_shared<const File>());
    assert(!registry.bindContentPackage(1, metadata, extra));
    assert(registry.bindContentPackage(1, metadata, sources));
    assert(!registry.bindContentPackage(1, metadata, sources));
    c = {}; c.arguments = {1, 0x4000, 0, 0, 0, 0, 0, 0};
    assert(!registry.dispatch("__imp__XamContentGetDeviceData", c).handled());
    assert(registry.bindContentPackage(2, metadata, sources));
    for (auto output : {0U, 0x4001U, 0xFFFFFFF8U, 0x30008U}) {
        c = {}; c.arguments = {1, output, 0, 0, 0, 0, 0, 0}; c.returnValue = 0xABCD;
        assert(!registry.dispatch("__imp__XamContentGetDeviceData", c).handled() && c.returnValue == 0xABCD);
    }
    std::memset(space.translate(0x4000), 0xA5, 88);
    c = {}; c.arguments = {2, 0x4000, 0, 0, 0, 0, 0, 0};
    assert(registry.dispatch("__imp__XamContentGetDeviceData", c).handled() && c.returnValue == 1167);
    assert(word(space.translate(0x4000)) == 0xA5A5A5A5);
    c.arguments[0] = 1; assert(registry.dispatch("__imp__XamContentGetDeviceData", c).handled() && c.returnValue == 0);
    assert(word(space.translate(0x4000)) == 1 && word(space.translate(0x4004)) == 1);
    assert(word(space.translate(0x4008)) == 0 && word(space.translate(0x400C)) == 4096);
    assert(word(space.translate(0x4010)) == 0 && word(space.translate(0x4014)) == 0);
    assert(space.translate(0x4000)[24] == 0 && space.translate(0x4000)[25] == 'N');
    assert(word(space.translate(0x4050)) == 0xA5A5A5A5);
    for (const auto& change : {std::pair{0U, 0U}, {1U, 0U}, {2U, 0xFFFFFFFCU}, {3U, 1U}, {3U, 2U},
                              {3U, 4U}, {3U, 5U}, {4U, 0x2200U}, {4U, 0x2100U}, {4U, 0x8054U},
                              {4U, 0x30004U}, {5U, 0x2000U}, {6U, 1U}, {7U, 1U}}) {
        c = mount(); c.arguments[change.first] = change.second; write(space.translate(0x200C), 0xA5A5A5A5);
        assert(!registry.dispatch("__imp__XamContentCreateEx", c).handled() && c.returnValue == 0xABCD);
        assert(word(space.translate(0x200C)) == 0xA5A5A5A5);
    }
    c = mount(); c.stackPointer = 0; assert(!registry.dispatch("__imp__XamContentCreateEx", c).handled());
    c = mount(); write(space.translate(0x8054), 1); assert(!registry.dispatch("__imp__XamContentCreateEx", c).handled());
    write(space.translate(0x8054), 0);
    for (const auto* root : {"", "../pack", "pack:", "pack\\bad", "a b"}) {
        std::strcpy(reinterpret_cast<char*>(space.translate(0x2100)), root);
        c = mount(); assert(!registry.dispatch("__imp__XamContentCreateEx", c).handled());
    }
    std::memcpy(space.translate(0x2100), "1pack", 6);
    space.translate(0x2200)[264] = '9'; c = mount();
    assert(registry.dispatch("__imp__XamContentCreateEx", c).handled() && c.returnValue == 3);
    space.translate(0x2200)[264] = '0';
    // Display metadata is not the package lookup identity.
    space.translate(0x2200)[9] = 'Z'; c = mount();
    assert(registry.dispatch("__imp__XamContentCreateEx", c).handled() && c.returnValue == 0 && word(space.translate(0x200C)) == 2);
    std::memcpy(space.translate(0x2100), "1PACK", 6); c = mount();
    assert(registry.dispatch("__imp__XamContentCreateEx", c).handled() && c.returnValue == 183);
    const auto ansi = [&](std::uint32_t address, std::string_view value) {
        write(space.translate(address), static_cast<std::uint32_t>((value.size() << 16) | value.size()));
        write(space.translate(address + 4), address + 16);
        std::memcpy(space.translate(address + 16), value.data(), value.size());
    };
    const auto link = [&](bool createLink) {
        c = {}; c.arguments = {0x6000, 0x6100}; c.returnValue = 0xABCD;
        return registry.dispatch(createLink ? "__imp__ObCreateSymbolicLink" : "__imp__ObDeleteSymbolicLink", c);
    };
    ansi(0x6000, "1pack:"); ansi(0x6100, "game:\\");
    assert(!link(false).handled() && c.returnValue == 0xABCD);
    assert(!link(true).handled() && c.returnValue == 0xABCD);
    ansi(0x6000, "alias:"); ansi(0x6100, "1PACK:\\");
    assert(link(true).handled() && c.returnValue == 0);
    std::memcpy(space.translate(0x2100), "alias", 6); c = mount();
    assert(registry.dispatch("__imp__XamContentCreateEx", c).handled() && c.returnValue == 183);
    std::memcpy(space.translate(0x2100), "1PACK", 6);
    // Route ordinary NT metadata and read-only opens through the mounted root.
    const std::string filePath = "alias:/f.bin";
    std::memcpy(space.translate(0x2480), filePath.data(), filePath.size());
    write(space.translate(0x2400), 0); write(space.translate(0x2404), 0x2420); write(space.translate(0x2408), 0x40);
    write(space.translate(0x2420), static_cast<std::uint32_t>((filePath.size() << 16) | filePath.size()));
    write(space.translate(0x2424), 0x2480);
    c = {}; c.arguments = {0x2400, 0x2800, 0, 0, 0, 0, 0, 0};
    assert(registry.dispatch("__imp__NtQueryFullAttributesFile", c).handled() && c.returnValue == 0);
    assert(word(space.translate(0x2800 + 44)) == 16);
    c = {}; c.stackPointer = 0x8000; write(space.translate(0x8054), 0x60);
    c.arguments = {0x2010, 0x80100001, 0x2400, 0x2018, 0, 0, 7, 1};
    assert(registry.dispatch("__imp__NtCreateFile", c).handled() && c.returnValue == 0);
    const auto fileHandle = word(space.translate(0x2010));
    // Removing the alias preserves the open source; closing its resolved mount
    // must still invalidate that handle, not leave it detached under alias:.
    assert(link(false).handled() && c.returnValue == 0);
    c = {}; c.arguments = {fileHandle, 0, 0, 0, 0x2018, 0x3000, 16, 0};
    assert(registry.dispatch("__imp__NtReadFile", c).handled() && c.returnValue == 0);
    for (unsigned i = 0; i < 16; ++i) { assert(space.translate(0x3000)[i] == 0x30 + i); }
    c = {}; c.arguments = {0x2100, 1, 0, 0, 0, 0, 0, 0};
    assert(!registry.dispatch("__imp__XamContentClose", c).handled());
    c.arguments[1] = 0; assert(registry.dispatch("__imp__XamContentClose", c).handled() && c.returnValue == 0);
    c = {}; c.arguments[0] = fileHandle; assert(registry.dispatch("__imp__NtClose", c).handled() && c.returnValue == 0xC0000008);
    c = {}; c.arguments[0] = 0x2100; assert(registry.dispatch("__imp__XamContentClose", c).handled() && c.returnValue == 2);
    c = {}; c.arguments = {0x2400, 0x2800, 0, 0, 0, 0, 0, 0}; assert(!registry.dispatch("__imp__NtQueryFullAttributesFile", c).handled());
    // Closing a mount permits reopening it from the retained read-only source.
    write(space.translate(0x8054), 0); c = mount();
    assert(registry.dispatch("__imp__XamContentCreateEx", c).handled() && c.returnValue == 0);
    // Every handle kind shares the existing cap; failure leaves outputs untouched.
    for (std::size_t i = 1; i < GuestImportRegistry::MaximumKernelHandles; ++i) {
        c = create(); assert(registry.dispatch("__imp__XamContentCreateEnumerator", c).handled() && c.returnValue == 0);
    }
    write(space.translate(0x2000), 0xA5A5A5A5); write(space.translate(0x2004), 0xA5A5A5A5);
    c = create(); assert(registry.dispatch("__imp__XamContentCreateEnumerator", c).handled() && c.returnValue == 8);
    assert(word(space.translate(0x2000)) == 0xA5A5A5A5 && word(space.translate(0x2004)) == 0xA5A5A5A5);
    registry.reset(); assert(registry.initialize(space, 0x40000, 0x10000));
    ansi(0x6000, "alias:"); ansi(0x6100, "game:\\");
    assert(link(true).handled() && c.returnValue == 0);
    registry.reset(); assert(registry.initialize(space, 0x40000, 0x10000));
    assert(link(false).handled() && c.returnValue == 0xC0000001);
    c = create(); assert(!registry.dispatch("__imp__XamContentCreateEnumerator", c).handled());
    assert(catalog.load(bytes) && registry.bindInstalledContent(catalog, 0x12345678));
    c = enumerate(second); assert(registry.dispatch("__imp__XamEnumerate", c).handled() && c.returnValue == 6);
}
