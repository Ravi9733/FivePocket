#include "guest_import_registry.h"

#include <algorithm>
#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;
namespace {
class SyntheticFile final : public GuestReadOnlyFile {
public:
    bool fail{false};
    [[nodiscard]] std::uint64_t size() const noexcept override { return 2345; }
    [[nodiscard]] bool read(std::uint64_t offset, std::uint8_t* destination, std::size_t bytes) const noexcept override {
        if (offset > size() || bytes > size() - offset) { return false; }
        for (std::size_t i = 0; i < bytes; ++i) { destination[i] = static_cast<std::uint8_t>((offset + i) % 251); }
        return !fail; // Can fail after writing its host staging buffer.
    }
};
void append(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned size) {
    for (unsigned i = size; i > 0; --i) { out.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8))); }
}
void record(std::vector<std::uint8_t>& out, std::string_view name, bool directory, std::uint64_t size = 0) {
    append(out, name.size(), 4); out.insert(out.end(), name.begin(), name.end());
    for (const auto value : {1ULL, 2ULL, 3ULL, 4ULL}) { append(out, value, 8); }
    append(out, directory ? 0 : 4096, 8); append(out, size, 8); append(out, directory ? 0x11 : 1, 4);
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> out{'G','F','M','E','T','A','1',0};
    append(out, 3, 4); append(out, 5, 4);
    for (const auto c : std::string("game:")) { out.push_back(c); }
    record(out, "game:\\", true); record(out, "game:\\folder", true); record(out, "game:\\folder\\a.bin", false, 2345);
    return out;
}
void write(GuestAddressSpace& space, std::uint32_t address, std::uint64_t value, unsigned size = 4) {
    auto* p = space.translate(address);
    for (unsigned i = 0; i < size; ++i) { p[i] = static_cast<std::uint8_t>(value >> ((size - i - 1) * 8)); }
}
GuestImportDispatchResult call(GuestImportRegistry& registry, GuestImportCall& call) {
    call = {}; call.arguments[0] = 0x20000000; call.arguments[1] = 0x20001000;
    call.returnValue = 0xABCDEF;
    return registry.dispatch("__imp__NtQueryFullAttributesFile", call);
}
}
int main() {
    for (const auto path : {"", "../a", "game:relative", "game:\\..\\a", "game:\\.\\a", "game:\\\\a",
            "game:\\a?", "game:\\a:stream", "game:\\a.", "game:\\a ", "game:\\a|b", "!game:\\a"}) {
        assert(!normalizeGuestFilePath(path));
    }
    assert(normalizeGuestFilePath("\\??\\GAME:/Folder/A.BIN") == "game:\\folder\\a.bin");
    assert(normalizeGuestFilePath("game:") == "game:\\");
    assert(normalizeGuestFilePath("1GAME:/a") == "1game:\\a");
    GuestFileMetadataCatalog catalog;
    const auto bytes = fixture();
    assert(catalog.load(bytes) && catalog.size() == 3);
    assert(!catalog.pathAt(0) && !catalog.pathAt(4));
    for (std::uint32_t i = 1; i <= 3; ++i) { assert(catalog.recordIndex(*catalog.pathAt(i)) == i); }
    assert(catalog.query("GAME:/Folder/A.bin").metadata.values[5] == 2345);
    assert(catalog.query("game:\\absent").state == GuestFileQueryState::NotFound);
    assert(catalog.query("other:\\absent").state == GuestFileQueryState::Unavailable);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        assert(!catalog.load(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + size)));
        assert(catalog.size() == 0); // Failed loads cannot retain a stale successful namespace.
    }
    auto corrupt = bytes; corrupt.push_back(0); assert(!catalog.load(corrupt));
    corrupt = bytes; corrupt[11] = 2; assert(!catalog.load(corrupt));
    corrupt = bytes; corrupt.back() = 0x80; assert(!catalog.load(corrupt));
    assert(catalog.load(bytes));

    GuestAddressSpace space; assert(space.reserve());
    GuestImportCall arguments;
    std::uint32_t value = 0xA5;
    arguments.arguments[7] = 0xFEDCBA9876543210ULL;
    assert(arguments.readArgument32(space, 7, value) && value == 0x76543210);
    value = 0xA5;
    assert(!arguments.readArgument32(space, 8, value) && value == 0xA5);
    arguments.stackPointer = 0x21000000;
    for (std::size_t i = 8; i < 16; ++i) {
        write(space, arguments.stackPointer + 0x54 + static_cast<std::uint32_t>((i - 8) * 8), 0xAB00 + i);
        assert(arguments.readArgument32(space, i, value) && value == 0xAB00 + i);
    }
    assert(!arguments.readArgument32(space, 16, value));
    for (auto stack : {1U, 0x21000001U, 0xFFFFFFF0U}) {
        arguments.stackPointer = stack; value = 0xA5;
        assert(!arguments.readArgument32(space, 8, value) && value == 0xA5);
    }
    GuestImportRegistry registry;
    assert(!registry.bindNoRawStorageDevices());
    assert(!registry.bindFileMetadataQuery([](auto) { return GuestFileQueryResult{}; }));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    GuestImportCall c;
    assert(!call(registry, c).handled());
    bool corruptProvider = false;
    assert(registry.bindFileMetadataQuery([&](auto path) {
        auto result = catalog.query(path);
        if (corruptProvider) { result.metadata.values[4] = 1; }
        return result;
    }));
    assert(!registry.bindFileMetadataQuery([](auto) { return GuestFileQueryResult{}; }));
    constexpr std::uint32_t Object = 0x20000000, Name = Object + 0x100, Text = Object + 0x200, Out = Object + 0x1000;
    write(space, Object, 0xFFFFFFFD); write(space, Object + 4, Name); write(space, Object + 8, 0x40);
    const auto setPath = [&](std::string_view path) {
        write(space, Name, path.size(), 2); write(space, Name + 2, path.size(), 2); write(space, Name + 4, Text);
        std::memcpy(space.translate(Text), path.data(), path.size()); // Length-delimited, no required null.
        std::memset(space.translate(Out), 0xA5, 64);
    };
    setPath("GAME:/Folder/A.BIN");
    assert(call(registry, c).handled() && c.returnValue == 0);
    assert(space.translate(Out)[7] == 1 && space.translate(Out)[39] == 0 && space.translate(Out)[38] == 0x10);
    assert(space.translate(Out)[46] == 9 && space.translate(Out)[47] == 41 && space.translate(Out)[51] == 1);
    assert(space.translate(Out)[52] == 0xA5 && space.translate(Out)[63] == 0xA5); // ABI padding untouched.
    for (const auto path : {"game:\\absent", "other:\\a", "game:\\..\\a"}) {
        setPath(path);
        const auto result = call(registry, c);
        if (std::string_view(path) == "game:\\absent") { assert(result.handled() && c.returnValue == 0xC000000FU); }
        else if (std::string_view(path) == "other:\\a") { assert(!result.handled() && c.returnValue == 0xABCDEF); }
        else { assert(result.handled() && c.returnValue == 0xC0000033U); }
        for (unsigned i = 0; i < 64; ++i) { assert(space.translate(Out)[i] == 0xA5); }
    }
    setPath("game:\\folder\\a.bin");
    for (const auto root : {1U, 0xFFFFFFFEU}) {
        write(space, Object, root); assert(!call(registry, c).handled());
    }
    write(space, Object, 0);
    write(space, Name + 2, 1, 2); assert(!call(registry, c).handled());
    setPath("game:\\folder\\a.bin");
    write(space, Object + 8, 0); assert(!call(registry, c).handled());
    write(space, Object + 8, 0x40);
    write(space, Name + 4, Out); assert(!call(registry, c).handled());
    write(space, Name + 4, Text);
    corruptProvider = true; assert(!call(registry, c).handled());
    assert(space.translate(Out)[0] == 0xA5 && c.returnValue == 0xABCDEF);
    corruptProvider = false;
    const auto open = [&] {
        c = {}; c.arguments = {{Out, 0x80100080U, Object, Out + 16, 0, 0x80, 1, 1}};
        c.stackPointer = 0x21000000; c.returnValue = 0xABCDEF;
        return registry.dispatch("__imp__NtCreateFile", c);
    };
    write(space, 0x21000054, 0x60);
    setPath("game:\\missing.bin");
    assert(open().handled() && c.returnValue == 0xC000000FU);
    assert(space.translate(Out)[0] == 0xA5 && space.translate(Out + 16)[0] == 0xC0
            && space.translate(Out + 16)[3] == 0x0F && space.translate(Out + 20)[0] == 0);
    assert(registry.lastFileCreateRequest()->lookup == 2 && registry.lastFileCreateRequest()->options == 0x60);
    setPath("game:\\folder\\a.bin");
    assert(!open().handled() && c.returnValue == 0xABCDEF && registry.lastFileCreateRequest()->lookup == 1);
    setPath("unknown:\\a.bin");
    assert(!open().handled() && registry.lastFileCreateRequest()->lookup == 3);
    setPath("\\Device\\Harddisk0\\Partition0");
    assert(!open().handled());
    assert(registry.lastFileCreateRequest()->path == "\\Device\\Harddisk0\\Partition0");
    assert(registry.lastFileCreateRequest()->objectRoot == 0 && registry.lastFileCreateRequest()->objectFlags == 0x40);
    assert(registry.bindNoRawStorageDevices());
    const auto opensBeforeRaw = registry.fileIoStatistics().opens;
    for (const auto path : {"\\Device\\Harddisk0\\partition0", "\\device\\HARDDISK12\\PARTITION3"}) {
        setPath(path);
        write(space, 0x21000054, 0x22);
        c = {}; c.arguments = {{Out, 0xC0100000U, Object, Out + 16, 0, 4, 1, 1}};
        c.stackPointer = 0x21000000; c.returnValue = 0xABCDEF;
        assert(registry.dispatch("__imp__NtCreateFile", c).handled() && c.returnValue == 0xC000000FU);
        assert(registry.lastFileCreateRequest()->lookup == 2);
        for (unsigned i = 0; i < 4; ++i) assert(space.translate(Out)[i] == 0xFF);
        assert(space.translate(Out + 16)[0] == 0xC0 && space.translate(Out + 16)[3] == 0x0F);
        assert(space.translate(Out + 20)[3] == 5 && space.translate(Out + 24)[0] == 0xA5);
    }
    assert(registry.fileIoStatistics().opens == opensBeforeRaw);
    setPath("\\Device\\HarddiskX\\Partition0");
    assert(!open().handled() && space.translate(Out)[0] == 0xA5);
    setPath("game:\\missing.bin");
    write(space, 0x21000054, 0x10); // Alertable mode remains unsupported.
    assert(!open().handled() && registry.lastFileCreateRequest()->lookup == 0);
    assert(space.translate(Out)[0] == 0xA5 && space.translate(Out + 16)[0] == 0xA5);
    auto source = std::make_shared<SyntheticFile>();
    assert(registry.bindReadOnlyFiles([source](auto path) -> std::shared_ptr<const GuestReadOnlyFile> {
        return path == "game:\\folder\\a.bin" ? source : nullptr;
    }));
    assert(!registry.bindReadOnlyFiles([source](auto) { return source; }));
    write(space, 0x21000054, 0x60);
    constexpr auto LinkName=0x20008000u, LinkTarget=0x20008100u;
    const auto ansi=[&](auto descriptor,std::string_view text) {
        write(space,descriptor,text.size(),2); write(space,descriptor+2,text.size(),2);
        write(space,descriptor+4,descriptor+16);
        std::memcpy(space.translate(descriptor+16),text.data(),text.size());
        write(space,descriptor+8,0xABCD1234);
    };
    const auto link=[&](bool create) {
        c={}; c.arguments[0]=LinkName; c.arguments[1]=LinkTarget; c.returnValue=0xABCDEF;
        return registry.dispatch(create?"__imp__ObCreateSymbolicLink":"__imp__ObDeleteSymbolicLink",c);
    };
    ansi(LinkName,"\\??\\xbmovie:");
    assert(link(false).handled() && c.returnValue==0xC0000001);
    ansi(LinkName,"\\??\\ALIAS:"); ansi(LinkTarget,"game:\\folder");
    assert(link(true).handled() && c.returnValue==0);
    assert(link(true).handled() && c.returnValue==0xC0000001);
    setPath("alias:\\a.bin"); assert(call(registry,c).handled() && c.returnValue==0);
    ansi(LinkName,"game:"); assert(!link(false).handled() && c.returnValue==0xABCDEF);
    assert(!link(true).handled());
    ansi(LinkName,"bad:"); ansi(LinkTarget,"\\Device\\CdRom0"); assert(!link(true).handled());
    ansi(LinkName,"alias:"); ansi(LinkTarget,"game:\\folder");
    write(space,LinkName+2,1,2); assert(!link(false).handled()); ansi(LinkName,"alias:");
    assert(space.translate(LinkName+8)[0]==0xAB && space.translate(LinkTarget+8)[0]==0xAB);
    setPath("alias:\\a.bin");
    assert(open().handled() && c.returnValue == 0);
    auto read32 = [&](auto at) { const auto* p = space.translate(at); return
        (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3]; };
    const auto fileHandle = read32(Out);
    assert(fileHandle != 0 && read32(Out + 20) == 1);
    assert(link(false).handled() && c.returnValue==0); // Existing handle survives link removal.
    assert(!call(registry,c).handled()); // New lookup no longer resolves the removed alias.
    setPath("game:\\folder\\a.bin");
    constexpr auto Buffer = Out + 0x100, Offset = Out + 0x400;
    const auto read = [&](std::uint32_t token, std::uint32_t length, std::uint32_t offset = 0) {
        c = {}; c.arguments = {{token, 0, 0, 0, Out + 16, length ? Buffer : 0, length, offset}};
        c.returnValue = 0xABCDEF;
        return registry.dispatch("__imp__NtReadFile", c);
    };
    write(space, Offset, 100, 8);
    assert(read(fileHandle, 16, Offset).handled() && c.returnValue == 0 && read32(Out + 20) == 16);
    assert(space.translate(Buffer)[0] == 100 && space.translate(Buffer)[15] == 115);
    assert(read(fileHandle, 16).handled() && space.translate(Buffer)[0] == 116);
    write(space, Offset, 0xFFFFFFFFFFFFFFFEULL, 8);
    assert(read(fileHandle, 16, Offset).handled() && space.translate(Buffer)[0] == 132);
    const auto query = [&](std::uint32_t type, std::uint32_t bytes) {
        c = {}; c.arguments = {{fileHandle, Out + 16, Buffer, bytes, type}}; c.returnValue = 0xABCDEF;
        std::memset(space.translate(Buffer), 0xA5, 64);
        return registry.dispatch("__imp__NtQueryInformationFile", c);
    };
    assert(query(14, 8).handled() && c.returnValue == 0 && read32(Buffer + 4) == 148 && read32(Out + 20) == 8);
    assert(query(17, 4).handled() && c.returnValue == 0 && read32(Buffer) == 0);
    assert(query(26, 4).handled() && c.returnValue == 0xC000000DU && read32(Out + 20) == 0 && read32(Buffer) == 0xA5A5A5A5U);
    assert(query(34, 56).handled() && c.returnValue == 0 && read32(Buffer + 44) == 2345 && read32(Buffer + 48) == 1);
    assert(read32(Buffer + 52) == 0xA5A5A5A5U && read32(Out + 20) == 56);
    assert(query(14, 4).handled() && c.returnValue == 0xC0000004U && read32(Buffer) == 0xA5A5A5A5U);
    assert(!query(5, 24).handled() && c.returnValue == 0xABCDEF);
    (void)query(14, 8); c.arguments[2] = Out + 16;
    assert(!registry.dispatch("__imp__NtQueryInformationFile", c).handled());
    (void)query(14, 8); c.arguments[0] = 0;
    assert(registry.dispatch("__imp__NtQueryInformationFile", c).handled() && c.returnValue == 0xC0000008U);
    assert(open().handled());
    const auto secondHandle = read32(Out);
    assert(secondHandle != fileHandle && read(secondHandle, 1).handled() && space.translate(Buffer)[0] == 0);
    write(space, Offset, 2338, 8);
    assert(read(fileHandle, 20, Offset).handled() && c.returnValue == 0 && read32(Out + 20) == 7);
    std::memset(space.translate(Buffer), 0xA5, 20);
    assert(read(fileHandle, 1).handled() && c.returnValue == 0xC0000011U && read32(Out + 20) == 0);
    assert(space.translate(Buffer)[0] == 0xA5);
    assert(read(fileHandle, 0).handled() && c.returnValue == 0);
    source->fail = true;
    write(space, Offset, 0, 8);
    assert(!read(fileHandle, 20, Offset).handled() && c.returnValue == 0xABCDEF);
    assert(space.translate(Buffer)[0] == 0xA5); // Partial host output never leaks into guest data.
    source->fail = false;
    assert(read(fileHandle, 1).handled() && c.returnValue == 0xC0000011U); // Failed read preserved the cursor.
    assert(!read(fileHandle, GuestImportRegistry::MaximumFileReadBytes + 1).handled());
    write(space, Offset, UINT64_MAX, 8); assert(!read(fileHandle, 1, Offset).handled());
    (void)read(fileHandle, 0);
    c.arguments[1] = 1; assert(!registry.dispatch("__imp__NtReadFile", c).handled());
    c.arguments[1] = 0; c.arguments[5] = Out + 16; c.arguments[6] = 8;
    assert(!registry.dispatch("__imp__NtReadFile", c).handled());
    const auto seek = [&](std::uint64_t position) {
        write(space, Offset, position, 8);
        c = {}; c.arguments = {{fileHandle, Out + 16, Offset, 8, 14}};
        return registry.dispatch("__imp__NtSetInformationFile", c);
    };
    assert(seek(0).handled() && c.returnValue == 0 && read32(Out + 20) == 8);
    assert(read(fileHandle, 5).handled() && c.returnValue == 0 && space.translate(Buffer)[4] == 4);
    assert(seek(100000).handled() && c.returnValue == 0);
    assert(read(fileHandle, 1).handled() && c.returnValue == 0xC0000011U);
    assert(seek(UINT64_MAX).handled() && c.returnValue == 0xC000000DU);
    assert(query(14, 8).handled() && read32(Buffer + 4) == 100000);
    c = {}; c.arguments = {{fileHandle, Out + 16, Offset, 8, 20}};
    assert(!registry.dispatch("__imp__NtSetInformationFile", c).handled()); // Never truncate a read-only source.
    c.arguments[4] = 14; c.arguments[3] = 4;
    assert(!registry.dispatch("__imp__NtSetInformationFile", c).handled());
    const auto stats = registry.fileIoStatistics();
    assert(stats.opens == 2 && stats.seeks == 2 && stats.bytes >= 5 && stats.reads > 0);
    c = {}; c.arguments[0] = fileHandle;
    assert(registry.dispatch("__imp__NtClose", c).handled() && c.returnValue == 0);
    assert(read(fileHandle, 1).handled() && c.returnValue == 0xC0000008U);
    assert(read(secondHandle, 1).handled() && c.returnValue == 0 && space.translate(Buffer)[0] == 1);
    c = {}; c.arguments = {{secondHandle, 0, 0, 0xDEADBEEFU, Out + 16, Buffer, 1, 0}};
    assert(registry.dispatch("__imp__NtReadFile", c).handled() && c.returnValue == 0 && space.translate(Buffer)[0] == 2);
    c.arguments[2] = 1; // A non-null APC routine remains unsupported, even with the same token.
    assert(!registry.dispatch("__imp__NtReadFile", c).handled());
    registry.reset(); assert(!call(registry, c).handled());
    assert(!registry.lastFileCreateRequest());
    assert(registry.fileIoStatistics().opens == 0 && registry.fileIoStatistics().bytes == 0);
}
