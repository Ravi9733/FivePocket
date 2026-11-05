#pragma once
#include "native_color8_clear_execution.h"
#include <cassert>
#include <fstream>
#include <string>

// Capture payloads stay private. call.bin is a lossless big-endian encoding of
// the recorded clear.json call fields, not a replacement authored request.
struct Color8ClearExecutionFixture {
    recomp::gpu::Gta5ClearCapture capture;
    static std::vector<std::uint8_t> bytes(const std::string& path) {
        std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
        const auto n=f.tellg();assert(n>0 && n<=65536);
        std::vector<std::uint8_t> b(n);f.seekg(0);assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
    }
    static unsigned word(const auto& b,unsigned at) {
        assert(at+4<=b.size());return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    }
    explicit Color8ClearExecutionFixture(const std::string& dir) {
        auto& c=capture;const auto call=bytes(dir+"/call.bin");assert(call.size()==36);
        c.call={word(call,0),word(call,4),word(call,8),word(call,12),word(call,16),word(call,20),
            word(call,24),(std::uint64_t{word(call,28)}<<32)|word(call,32)};
        c.state=bytes(dir+"/device-state.bin");assert(c.state->size()==13864);
        c.rectangle=bytes(dir+"/clear-rectangle.bin");c.color=bytes(dir+"/clear-color.bin");
        assert(c.rectangle->size()==16 && c.color->size()==16);
        for(unsigned i=0;i<5;++i) {
            c.surfaces[i].object=word(*c.state,12816+4*i);c.surfaces[i].tileCache=word(*c.state,13104+4*i);
            if(c.surfaces[i].object)c.surfaces[i].header=bytes(dir+"/surface-"+std::to_string(i)+"-resource.bin");
        }
    }
};

inline void seedColor8ClearPhysical(recomp::gpu::NativeEdram32Bands& physical) {
    std::vector<std::uint8_t> old(80*16*4);
    for(std::size_t i=0;i<old.size();++i)old[i]=std::uint8_t(i*37+11);
    assert(physical.recordCompletedRawColorRows(720,80,80,16,old,physical.nextSerial()));
}
