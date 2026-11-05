#include "guest_native_read_only_directory.h"
#include "guest_import_registry.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>

using namespace recomp::bootstrap;
int main() {
    char pattern[] = "/tmp/recomp-cache-test.XXXXXX";
    const char* created = ::mkdtemp(pattern);
    assert(created);
    const std::filesystem::path root(created);
    GuestNativeReadOnlyDirectory cache("cache:", created);
    assert(cache.ready());
    assert(cache.query("cache:\\cloud_d94335e1.dat").state == GuestFileQueryState::NotFound);
    assert(cache.query("other:\\absent").state == GuestFileQueryState::Unavailable);
    assert(cache.query("cache:\\..\\absent").state == GuestFileQueryState::Unavailable);
    assert(cache.query("cache:").metadata.attributes == 0x11);
    assert(::mkdir((root / "Update").c_str(), 0700) == 0);
    GuestNativeReadOnlyDirectory update("update:", (root / "Update").c_str());
    assert(update.ready() && update.owns("UPDATE:/update.img") && !cache.owns("update:\\update.img"));
    assert(update.query("update:\\update.img").state == GuestFileQueryState::NotFound);
    assert(::mkdir((root / "GameCache").c_str(), 0700) == 0);
    GuestNativeReadOnlyDirectory gamecache("gamecache:",(root / "GameCache").c_str());
    assert(gamecache.ready() && gamecache.owns("GAMECACHE:/data/gta5_cache_x.dat"));
    assert(!cache.owns("gamecache:\\data") && !update.owns("gamecache:\\data"));
    assert(::mkdir((root / "Mixed").c_str(), 0700) == 0);
    const auto actual = root / "Mixed" / "Existing.DAT";
    int fd = ::open(actual.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && ::write(fd, "native bytes", 12) == 12);
    ::close(fd);
    auto metadata = cache.query("CACHE:/MIXED/existing.dat");
    assert(metadata.state == GuestFileQueryState::Found && metadata.metadata.values[5] == 12);
    auto file = cache.open("cache:\\mixed\\existing.dat");
    std::uint8_t output[16]{};
    assert(file && file->size() == 12 && file->read(0, output, 12));
    assert(std::memcmp(output, "native bytes", 12) == 0);
    assert(file->read(7, output, 5) && std::memcmp(output, "bytes", 5) == 0);
    assert(!file->read(8, output, 5) && !cache.open("cache:\\mixed"));
    assert(::symlink(actual.c_str(), (root / "link").c_str()) == 0);
    assert(::symlink("/tmp", (root / "escape").c_str()) == 0);
    assert(cache.query("cache:\\link").state == GuestFileQueryState::Unavailable);
    assert(cache.query("cache:\\escape\\absent").state == GuestFileQueryState::Unavailable);
    assert(::mkfifo((root / "pipe").c_str(), 0600) == 0);
    assert(cache.query("cache:\\pipe").state == GuestFileQueryState::Unavailable);
    assert(!cache.open("cache:\\pipe"));

    // Exercise the observed guest open through the existing registry, first
    // absent, then present. No hard-coded missing-cloud-file special case.
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry; assert(registry.initialize(space, 0x10000000, 0x10000));
    assert(registry.bindFileMetadataQuery([&](auto path) {
        return gamecache.owns(path)?gamecache.query(path):update.owns(path)?update.query(path):cache.query(path);
    }));
    assert(registry.bindReadOnlyFiles([&](auto path) {
        return gamecache.owns(path)?gamecache.open(path):update.owns(path)?update.open(path):cache.open(path);
    }));
    const auto put = [&](std::uint32_t address, std::uint32_t value, unsigned bytes = 4) {
        for (unsigned i = 0; i < bytes; ++i) space.translate(address)[i] = value >> ((bytes - i - 1) * 8);
    };
    constexpr auto Object = 0x20000000U, Name = Object + 0x100, Text = Object + 0x200;
    constexpr auto Handle = Object + 0x300, Iosb = Object + 0x400, Stack = Object + 0x1000;
    put(Object, 0xFFFFFFFD); put(Object + 4, Name); put(Object + 8, 0x40);
    const auto request = [&](const char* path) {
        put(Name, std::strlen(path), 2); put(Name + 2, std::strlen(path), 2); put(Name + 4, Text);
        std::memcpy(space.translate(Text), path, std::strlen(path));
        put(Stack + 0x54, 0x60);
        GuestImportCall c; c.stackPointer = Stack;
        c.arguments = {Handle, 0x80100080U, Object, Iosb, 0, 128, 3, 1};
        assert(registry.dispatch("__imp__NtCreateFile", c).handled());
        return c.returnValue;
    };
    put(Handle, 0xA5A5A5A5);
    assert(request("cache:\\cloud_d94335e1.dat") == 0xC000000FU);
    assert(space.translate(Handle)[0] == 0xA5 && registry.fileIoStatistics().opens == 0);
    assert(request("cache:\\mixed\\existing.dat") == 0 && registry.fileIoStatistics().opens == 1);
    const auto attributes = [&](const char* path) {
        put(Name, std::strlen(path), 2); put(Name + 2, std::strlen(path), 2); put(Name + 4, Text);
        std::memcpy(space.translate(Text), path, std::strlen(path));
        std::memset(space.translate(Iosb), 0xA5, 64);
        GuestImportCall c; c.arguments = {Object,Iosb};c.returnValue=0xABCDEF;
        const auto result=registry.dispatch("__imp__NtQueryFullAttributesFile",c);
        return std::make_pair(result.handled(),c.returnValue);
    };
    auto queried=attributes("update:\\update.img");
    assert(queried.first && queried.second==0xC000000F);
    for(unsigned i=0;i<64;++i)assert(space.translate(Iosb)[i]==0xA5);
    queried=attributes("gamecache:\\data\\gta5_cache_x.dat");
    assert(queried.first && queried.second==0xC000000F);
    for(unsigned i=0;i<64;++i)assert(space.translate(Iosb)[i]==0xA5);
    queried=attributes("unbound:\\update.img");
    assert(!queried.first && queried.second==0xABCDEF); // Unknown is not absent.
    const auto updateFile=root / "Update" / "Update.IMG";
    fd=::open(updateFile.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
    assert(fd>=0 && ::write(fd,"patch bytes",11)==11);::close(fd);
    queried=attributes("UPDATE:/update.img");
    assert(queried.first && queried.second==0 && space.translate(Iosb)[47]==11);
    assert(space.translate(Iosb)[51]==1 && space.translate(Iosb)[52]==0xA5);
    auto patch=update.open("update:\\update.img");
    assert(patch && patch->read(0,output,11) && std::memcmp(output,"patch bytes",11)==0);
    // An already opened file detects later size changes instead of publishing
    // a short read as complete data.
    assert(::truncate(actual.c_str(), 2) == 0);
    assert(!file->read(0, output, 12));
    // Explicit, individually known fixture files only; no recursive deletion.
    assert(::unlink((root / "link").c_str()) == 0);
    assert(::unlink((root / "escape").c_str()) == 0);
    assert(::unlink((root / "pipe").c_str()) == 0);
    assert(::unlink(actual.c_str()) == 0);
    assert(::unlink(updateFile.c_str()) == 0);
    assert(::rmdir((root / "Update").c_str()) == 0);
    assert(::rmdir((root / "GameCache").c_str()) == 0);
    assert(::rmdir((root / "Mixed").c_str()) == 0);
    assert(::rmdir(root.c_str()) == 0);
    std::cout << "native cache/update directories, guest open and metadata PASS\n";
}
