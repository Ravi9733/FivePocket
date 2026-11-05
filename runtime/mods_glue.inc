// script-mods (2026-10-25): the library side of script_mods.h, included in trace_runner.cpp after NativeReadOnlyFile. The file
// bindings (bindFileMetadataQuery / bindReadOnlyFiles) pass every opened guest file through recomp_mods::view; with the setting
// off (default) or no mods found, view returns the original object and nothing else happens.
#include "script_mods.h"
#include <dirent.h>

namespace recomp_mods {
constexpr std::uint32_t ArchiveKey=0x83BD4C98u;   // the RPF table key (sub_831A86B8's r3 for sub_831A6AB0, 32 bytes)
constexpr unsigned MaxFiles=512,MaxDepth=12;
void log(const char* line) {__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","%s",line);}
recomp::mods::Layer& layer() {static recomp::mods::Layer l(log);return l;}
std::atomic<bool> enabled{false};
std::atomic<unsigned> found{0};

void scan(int dir,const std::string& prefix,std::map<std::string,recomp::mods::File>& out,unsigned depth,unsigned& refused) {
    DIR* d=::fdopendir(dir);
    if(!d){::close(dir);return;}
    while(const auto* e=::readdir(d)) {
        const std::string name=e->d_name;
        if(name.empty() || name[0]=='.')continue;          // ".", "..", hidden files
        std::string lower=name;for(auto& c:lower)c=char(std::tolower(static_cast<unsigned char>(c)));
        struct stat st{};
        if(::fstatat(::dirfd(d),name.c_str(),&st,AT_SYMLINK_NOFOLLOW))continue;
        const std::string key=prefix.empty()?lower:prefix+"/"+lower;
        if(S_ISDIR(st.st_mode)) {
            if(depth>=MaxDepth){++refused;continue;}
            const int sub=::openat(::dirfd(d),name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(sub>=0)scan(sub,key,out,depth+1,refused);
        } else if(S_ISREG(st.st_mode)) {
            if(out.size()>=MaxFiles || prefix.empty() || st.st_size<=0 || std::uint64_t(st.st_size)>(8ull<<30)){++refused;continue;}
            const int fd=::openat(::dirfd(d),name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
            if(fd<0){++refused;continue;}
            out.emplace(key,std::make_shared<NativeReadOnlyFile>(fd,std::uint64_t(st.st_size)));
        }
    }
    ::closedir(d);
}
// Once, before the bindings: files/settings/mods.txt, then files/mods/.
void setup() {
    char text[512]{};std::size_t n=0;
    if(FILE* f=std::fopen(RECOMP_APP_PATH("/settings/mods.txt"),"rb")){n=std::fread(text,1,sizeof text-1,f);std::fclose(f);}
    const bool on=recomp::mods::parseEnabled(text,n);
    enabled.store(on,std::memory_order_relaxed);
    if(!on){log("RENDER_MODS enabled=0");return;}
    std::map<std::string,recomp::mods::File> mods;unsigned refused=0;
    const int root=::open(RECOMP_APP_PATH("/mods"),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root>=0)scan(root,"",mods,0,refused);
    char line[200];std::snprintf(line,sizeof line,"RENDER_MODS enabled=1 files=%zu refused=%u (files/mods/<device>/<path>)",mods.size(),refused);log(line);
    for(const auto& [key,file]:mods){std::snprintf(line,sizeof line,"RENDER_MODS found %s bytes=%llu",key.c_str(),
        static_cast<unsigned long long>(file->size()));log(line);}
    found.store(unsigned(mods.size()),std::memory_order_relaxed);
    layer().setMods(std::move(mods));
}
// The file the guest gets: the original unless a mod covers it (the archive key is read from guest memory on the first such file).
recomp::mods::File view(std::string_view path,const recomp::mods::File& original,const std::uint8_t* base) {
    if(!layer().active() || !layer().covers(path))return original;
    static std::once_flag keyOnce;
    std::call_once(keyOnce,[&]{if(base)layer().setKey(base+ArchiveKey);});
    return layer().view(path,original);
}
} // namespace recomp_mods

// The app's status line: the number of mods applied; out = "enabled=.. found=.. applied=.. skipped=.." then one line per mod.
extern "C" __attribute__((visibility("default"),used)) const char recomp_mods_marker[]="recomp-script-mods-v1";
extern "C" __attribute__((visibility("default"))) int recomp_mods_status_v1(char* out,int cap) {
    const auto results=recomp_mods::layer().results();
    int applied=0,skipped=0;for(const auto& r:results)(r.ok?applied:skipped)+=1;
    if(out && cap>0) {
        std::string s="enabled="+std::to_string(recomp_mods::enabled.load()?1:0)+" found="+std::to_string(recomp_mods::found.load())+
            " applied="+std::to_string(applied)+" skipped="+std::to_string(skipped)+"\n";
        for(const auto& r:results)s+=(r.ok?"applied ":"skipped ")+r.key+": "+r.detail+"\n";
        std::snprintf(out,std::size_t(cap),"%s",s.c_str());
    }
    return applied;
}
