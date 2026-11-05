#include "native_color_program_input.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
using namespace recomp::gpu;
int main(){
#if defined(__ANDROID__)
    char pattern[]="/data/local/tmp/recomp-color-program-input.XXXXXX";
#else
    char pattern[]="/tmp/recomp-color-program-input.XXXXXX";
#endif
    const char* temporary=mkdtemp(pattern);assert(temporary);const std::string directory=temporary;
    struct File{std::string path;std::vector<std::uint8_t> bytes;};std::vector<File> files;
    const auto put=[](const File& file){const int fd=open(file.path.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);assert(fd>=0);
        std::size_t written=0;while(written<file.bytes.size()){const auto n=write(fd,file.bytes.data()+written,file.bytes.size()-written);
            if(n<0 && errno==EINTR)continue;assert(n>0);written+=n;}assert(close(fd)==0);};
    constexpr std::array<const char*,9> roles{{"depth/vertex","r8/vertex","r8/fragment","small/vertex","small/fragment","store","copy","load","depth/store"}};
    NativeColorChainProgram::Identities active;
    for(unsigned i=0;i<9;++i){
        std::filesystem::create_directories(directory+"/"+roles[i]);
        const unsigned model=i>=5?5:(i==2 || i==4?4:0);
        const std::vector<std::uint32_t> words{0x07230203,0x10300,0,16,0,(2u<<16)|17,1,(5u<<16)|15,model,1,0x6E69616D,0};
        File module{directory+"/"+roles[i]+"/dataflow.spv",{}};
        for(auto w:words)for(unsigned b=0;b<4;++b)module.bytes.push_back(w>>(b*8));files.push_back(module);
        if(i<5){File guest{directory+"/"+roles[i]+"/guest-code.bin",std::vector<std::uint8_t>(12,static_cast<std::uint8_t>(i))};
            active[i]={model==4?1u:0u,guest.bytes};files.push_back(guest);}
    }
    for(const auto& f:files)put(f);
    const auto profile=NativeColorCoordinateProfile::Padded1280W1;
    auto loaded=readNativeColorProgram(directory,profile);assert(loaded && loaded->matches(active) && loaded->coordinates()==profile);
    unsigned rejects=0;
    const auto reject=[&](const std::string& dir,NativeColorCoordinateProfile mode){assert(!readNativeColorProgram(dir,mode));++rejects;};
    reject("",profile);reject("relative",profile);reject(directory+"/absent",profile);reject(directory,static_cast<NativeColorCoordinateProfile>(99));
    for(const auto& f:files)for(unsigned mode=0;mode<5;++mode){
        if(mode==0)put({f.path,{}});
        if(mode==1){auto bad=f;bad.bytes.push_back(0);put(bad);}
        if(mode==2){assert(unlink(f.path.c_str())==0);assert(symlink("missing",f.path.c_str())==0);}
        if(mode==3)assert(unlink(f.path.c_str())==0);
        if(mode==4)put({f.path,std::vector<std::uint8_t>(65540)});
        reject(directory,profile);
        if(mode==2)assert(unlink(f.path.c_str())==0);
        put(f);
    }
    loaded=readNativeColorProgram(directory,NativeColorCoordinateProfile::DiagnosticPositive);
    assert(loaded && loaded->matches(active) && loaded->coordinates()==NativeColorCoordinateProfile::DiagnosticPositive);
    // Exact synthetic test files/directories only; no recursive cleanup target.
    for(const auto& f:files)assert(unlink(f.path.c_str())==0);
    for(const auto* role:roles)assert(rmdir((directory+"/"+role).c_str())==0);
    for(const auto* role:{"depth","r8","small"})assert(rmdir((directory+"/"+role).c_str())==0);
    assert(rmdir(directory.c_str())==0);
    std::printf("NATIVE_COLOR_PROGRAM_INPUT: %u bounded-file rejections; synthetic framing only, no GPU PASS\n",rejects);
}
