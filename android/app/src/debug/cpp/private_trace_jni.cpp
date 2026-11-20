#include <jni.h>
#include <android/dlext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <android/log.h>
#include "native_gamepad_state.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "private_trace_arguments.h"
#include "private_frame_surface.h"
#include <android/native_window_jni.h>

namespace {
std::atomic_flag prepared=ATOMIC_FLAG_INIT;
std::atomic_flag checked=ATOMIC_FLAG_INIT;
constexpr char LibraryName[]="private-trace-library.so";
constexpr char WindowLibraryName[]="private-trace-window.so";
constexpr char LiveLibraryName[]="private-trace-live.so";
const char* libraryName(int mode){return mode>=3?LiveLibraryName:(mode==2?WindowLibraryName:LibraryName);}
PrivateFrameSurface frameSurface;
recomp::input::GamepadSnapshot gamepad;
std::atomic_flag reportedInputRead=ATOMIC_FLAG_INIT;
std::atomic_uint32_t lastReportedInputPacket{0};
std::atomic_uint32_t reportedInputPackets{0};

bool limit(int resource,rlim_t amount){
    const struct rlimit wanted{amount,amount};struct rlimit observed{};
    return !setrlimit(resource,&wanted) && !getrlimit(resource,&observed) &&
        observed.rlim_cur==amount && observed.rlim_max==amount;
}

bool privateProcess(){
    char name[128]{};
    const int fd=open("/proc/self/cmdline",O_RDONLY|O_CLOEXEC);
    if(fd<0)return false;
    const ssize_t bytes=read(fd,name,sizeof(name)-1);close(fd);
    return bytes>0 && std::strcmp(name,"dev.recompbootstrap.smoke:private_trace")==0;
}

bool libraryFile(int fd){
    struct stat info{};
    unsigned char header[64]{};
    if(fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() ||
        (info.st_mode&0777)!=0400 || info.st_nlink!=1 || info.st_size<64 ||
        info.st_size>512LL*1024*1024 || pread(fd,header,sizeof(header),0)!=sizeof(header))return false;
    constexpr unsigned char elf[]{0x7f,'E','L','F',2,1,1};
    constexpr unsigned char machine[]{3,0,0xb7,0};
    constexpr unsigned char entry[8]{};
    return !std::memcmp(header,elf,sizeof(elf)) && !std::memcmp(header+16,machine,4) &&
        !std::memcmp(header+24,entry,8);
}

std::string directoryString(JNIEnv* env,jstring directory){
    if(!directory || env->GetStringUTFLength(directory)>4000)return {};
    const char* value=env->GetStringUTFChars(directory,nullptr);
    if(!value)return {};
    std::string result(value);env->ReleaseStringUTFChars(directory,value);
    if(result.empty() || result[0]!='/')return {};
    return result;
}

int ownedDirectory(const std::string& path){
    const int parent=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(parent<0)return -1;
    struct stat info{};
    // Android files/ is771 with the app's own group. Do not chmod it.
    if(fstat(parent,&info) || !S_ISDIR(info.st_mode) || info.st_uid!=geteuid() ||
        info.st_gid!=getegid() || (info.st_mode&0002)){close(parent);return -1;}
    const int child=openat(parent,"private-trace",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(parent);
    if(child<0)return -1;
    if(fstat(child,&info) || info.st_uid!=geteuid() || (info.st_mode&0777)!=0700){close(child);return -1;}
    return child;
}
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_claimRequest(JNIEnv* env,jclass,jstring directory,jstring requestId){
    if(!privateProcess() || !requestId || env->GetStringUTFLength(requestId)!=32)return -1;
    const auto path=directoryString(env,directory);if(path.empty())return -1;
    const char* id=env->GetStringUTFChars(requestId,nullptr);if(!id)return -1;
    char marker[38]="used-";
    bool valid=true;
    for(int i=0;i<32;++i){valid=valid && ((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f'));marker[i+5]=id[i];}
    env->ReleaseStringUTFChars(requestId,id);
    if(!valid)return -1;
    const int parent=ownedDirectory(path);if(parent<0)return -1;
    // Persist admission before any game-library load. Android may recreate a
    // foreground activity after process death: the same request must not rerun.
    const int fd=openat(parent,marker,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0400);
    const int error=errno;close(parent);
    if(fd<0)return error==EEXIST?1:-1;
    return close(fd)==0?0:-1;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_prepareProcess(JNIEnv*,jclass,jboolean traceRun){
    if(!privateProcess() || prepared.test_and_set())return false;
    // The Android framework/driver may attempt optional cache writes while a
    // diagnostic is alive. Exceeding the selected ceiling returns EFBIG instead
    // of SIGXFSZ killing the process. ABI checks permit zero file bytes; explicit
    // game runs permit files up to 256 MiB (2026-09-27: saves; was 64 KiB), never unbounded.
    struct sigaction fileSize{};fileSize.sa_handler=SIG_IGN;sigemptyset(&fileSize.sa_mask);
    if(sigaction(SIGXFSZ,&fileSize,nullptr))return false;
    if(!limit(RLIMIT_CORE,0) || !limit(RLIMIT_FSIZE,traceRun?(rlim_t{1}<<28):0) ||
        !limit(RLIMIT_CPU,traceRun?RLIM_INFINITY:10))return false;
    struct sigaction timeout{};timeout.sa_handler=SIG_DFL;sigemptyset(&timeout.sa_mask);
    if(sigaction(SIGALRM,&timeout,nullptr))return false;
    sigset_t unblocked;sigemptyset(&unblocked);sigaddset(&unblocked,SIGALRM);
    if(sigprocmask(SIG_UNBLOCK,&unblocked,nullptr))return false;
    // Game runs are played interactively: a 1200-CPU-second ceiling ended play
    // after ~8 minutes at ~2.5 cores. Game runs have no CPU or wall ceiling (the
    // host harness stops them); ABI checks keep their 10 CPU-s / 15 s bounds.
    // The installed 2026-09-27 APK carries exactly this as a two-instruction
    // patch (local-analysis/apk-limit-2026-09-27/patch_and_sign.py).
    alarm(traceRun?0:15); // Includes hashing, linker constructors and execution.
    return true;
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_openArguments(JNIEnv* env,jclass,jstring directory,jstring requestId){
    if(!privateProcess() || !requestId || env->GetStringUTFLength(requestId)!=32)return -1;
    const char* id=env->GetStringUTFChars(requestId,nullptr);if(!id)return -1;
    const bool valid=recomp::private_trace::requestId(id);
    char name[37]="run-";if(valid)std::memcpy(name+4,id,32);
    env->ReleaseStringUTFChars(requestId,id);if(!valid)return -1;
    const auto path=directoryString(env,directory);if(path.empty())return -1;
    const int parent=ownedDirectory(path);if(parent<0)return -1;
    const int run=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(parent);
    if(run<0)return -1;
    struct stat info{};
    if(fstat(run,&info) || info.st_uid!=geteuid() || (info.st_mode&0777)!=0700){close(run);return -1;}
    const int fd=openat(run,"arguments.bin",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);close(run);
    if(fd<0)return -1;
    if(fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() ||
        (info.st_mode&0777)!=0400 || info.st_nlink!=1 || info.st_size<1 || info.st_size>65536){close(fd);return -1;}
    return fd;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_runTrace(JNIEnv* env,jclass,jint descriptor,
        jstring directory,jstring requestId,jbyteArray manifest,jint frameMode){
    if(frameMode!=0 && frameMode!=2 && frameMode!=3 && frameMode!=4)return nullptr;
    if(!privateProcess() || checked.test_and_set() || !manifest || !requestId)return nullptr;
    const auto path=directoryString(env,directory);
    const auto count=env->GetArrayLength(manifest);
    if(path.empty() || count<1 || count>65536 || env->GetStringUTFLength(requestId)!=32)return nullptr;
    const char* text=env->GetStringUTFChars(requestId,nullptr);if(!text)return nullptr;
    char id[33]{};std::memcpy(id,text,32);env->ReleaseStringUTFChars(requestId,text);
    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
        env->GetByteArrayRegion(manifest,0,count,reinterpret_cast<jbyte*>(bytes.data()));
        if(env->ExceptionCheck())return nullptr;
        std::vector<std::string> arguments;
        if(!recomp::private_trace::parseArguments(bytes.data(),bytes.size(),path+"/private-trace",id,arguments))return nullptr;
        std::vector<char*> argv;for(auto& argument:arguments)argv.push_back(argument.data());argv.push_back(nullptr);
        const int fd=fcntl(descriptor,F_DUPFD_CLOEXEC,0);if(fd<0)return nullptr;
        if(!libraryFile(fd)){close(fd);return nullptr;}
        android_dlextinfo extension{};extension.flags=ANDROID_DLEXT_USE_LIBRARY_FD;extension.library_fd=fd;
        void* library=android_dlopen_ext((path+"/private-trace/"+libraryName(frameMode)).c_str(),RTLD_NOW|RTLD_LOCAL,&extension);
#ifdef RECOMP_LOADER_OPEN_LOG
        // The build that is handed out only (CMake option RECOMP_LOADER_OPEN_LOG; the debug build's loader stays the
        // qualified code): the linker's own words when the library is not opened (a refused mapping, an unresolved
        // symbol, a page size the file was not built for). Without them this refusal cannot be told apart from the
        // ones below: the game window shows REJECT NATIVE RUN for all of them, and nobody can look into the app's
        // storage of a build that is not debuggable. The text names files of the app only, never game data.
        if(!library){
            const char* why=dlerror();
            __android_log_print(ANDROID_LOG_ERROR,"RecompPrivateTrace","LIBRARY_OPEN_REFUSED %s: %s",
                libraryName(frameMode),why?why:"the linker gave no reason");
        }else __android_log_print(ANDROID_LOG_INFO,"RecompPrivateTrace","LIBRARY_OPENED %s",libraryName(frameMode));
#endif
        close(fd);if(!library)return nullptr;
        using Entry=int (*)(int,char**) noexcept;
        const auto entry=reinterpret_cast<Entry>(dlsym(library,"recomp_run_private_trace_v1"));
        const auto entryV2=reinterpret_cast<decltype(&recomp_run_private_trace_v2)>(dlsym(library,"recomp_run_private_trace_v2"));
        const auto entryV3=reinterpret_cast<decltype(&recomp_run_private_trace_v3)>(dlsym(library,"recomp_run_private_trace_v3"));
        const auto entryV4=reinterpret_cast<decltype(&recomp_run_private_trace_v4)>(dlsym(library,"recomp_run_private_trace_v4"));
        const auto entryV5=reinterpret_cast<decltype(&recomp_run_private_trace_v5)>(dlsym(library,"recomp_run_private_trace_v5"));
        // V6 (attempt v255): V5 plus the window, for libraries that export it (they present only when their own switch is on).
        const auto entryV6=reinterpret_cast<decltype(&recomp_run_private_trace_v6)>(dlsym(library,"recomp_run_private_trace_v6"));
        const bool nativeInput=std::find(arguments.begin(),arguments.end(),"--native-input")!=arguments.end();
#ifdef RECOMP_LOADER_OPEN_LOG
        if((nativeInput && (frameMode!=4 || !entryV5)) ||
            !entry || (frameMode==2 && !entryV2) || (frameMode==3 && !entryV3) || (frameMode==4 && !entryV4)){
            __android_log_print(ANDROID_LOG_ERROR,"RecompPrivateTrace","LIBRARY_ENTRY_MISSING mode=%d v1=%d v2=%d v3=%d v4=%d v5=%d",
                frameMode,entry!=nullptr,entryV2!=nullptr,entryV3!=nullptr,entryV4!=nullptr,entryV5!=nullptr);
        }
#endif
        if(nativeInput && (frameMode!=4 || !entryV5))return nullptr;
        if(!entry || (frameMode==2 && !entryV2) || (frameMode==3 && !entryV3) || (frameMode==4 && !entryV4))return nullptr;
        // Retain the private library to process death, including guest TLS.
        // Capture only a bounded protocol in memory. Never print it to logcat.
        std::vector<jbyte> protocol(65536+4);std::size_t captured=0;
        int output[2];if(pipe2(output,O_CLOEXEC))return nullptr;
        const int original=fcntl(STDOUT_FILENO,F_DUPFD_CLOEXEC,0);
        const int null=open("/dev/null",O_RDWR|O_CLOEXEC);
        if(original<0 || null<0){close(output[0]);close(output[1]);if(original>=0)close(original);if(null>=0)close(null);return nullptr;}
        std::thread reader;
        try {
            reader=std::thread([&]{
                jbyte buffer[8192];
                for(;;){
                    const ssize_t received=read(output[0],buffer,sizeof(buffer));
                    if(received==0)break;
                    if(received<0){if(errno==EINTR)continue;_exit(125);}
                    if(static_cast<std::size_t>(received)>65536-captured)_exit(126);
                    std::memcpy(protocol.data()+4+captured,buffer,static_cast<std::size_t>(received));
                    captured+=static_cast<std::size_t>(received);
                }
                close(output[0]);
            });
        }catch(...){close(output[0]);close(output[1]);close(original);close(null);return nullptr;}
        std::fflush(stdout);
        if(dup2(output[1],STDOUT_FILENO)<0 || dup2(null,STDERR_FILENO)<0 || dup2(null,STDIN_FILENO)<0)_exit(125);
        close(output[1]);close(null);
        const RecompPrivateTraceCallbacksV2 callbacks{sizeof(RecompPrivateTraceCallbacksV2),2,0,0,nullptr,
            [](void*,const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height)noexcept{
                return frameSurface.consume(rgba,bytes,width,height);}};
        const RecompPrivateTraceCallbacksV3 liveCallbacks{sizeof(RecompPrivateTraceCallbacksV3),3,0,0,nullptr,
            [](void*,const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height)noexcept{
                return frameSurface.consume(rgba,bytes,width,height,true);}};
        const RecompPrivateTraceCallbacksV4 displayCallbacks{sizeof(RecompPrivateTraceCallbacksV4),4,0,0,nullptr,
            [](void*,std::uint32_t sequence,const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height)noexcept{
                return frameSurface.consumeSequenced(sequence,rgba,bytes,width,height);}};
        const RecompPrivateTraceCallbacksV5 inputCallbacks{sizeof(RecompPrivateTraceCallbacksV5),5,0,0,displayCallbacks,nullptr,
            [](void*,std::uint32_t user,recomp::input::GamepadState* state)noexcept{
                if(!state)return -1;
                const int connected=gamepad.read(user,*state);
                if(connected && !reportedInputRead.test_and_set())
                    __android_log_print(ANDROID_LOG_INFO,"RecompNativeInput","guest queried actual connected controller packet=%u",state->packet);
                // Bounded evidence of what the guest actually sampled, distinct
                // from Android key delivery. Never log every polling call.
                if(connected && lastReportedInputPacket.exchange(state->packet)!=state->packet &&
                   reportedInputPackets.fetch_add(1)<128)
                    __android_log_print(ANDROID_LOG_INFO,"RecompNativeInput",
                        "GUEST_SAMPLE packet=%u buttons=%04x LT=%u RT=%u LX=%d LY=%d RX=%d RY=%d",
                        state->packet,state->buttons,state->leftTrigger,state->rightTrigger,
                        state->leftX,state->leftY,state->rightX,state->rightY);
                return connected;
            },[](void*,std::uint32_t user,recomp::input::GamepadState* state)noexcept{
                return state?gamepad.readCapabilities(user,*state):-1;
            }};
        const RecompPrivateTraceCallbacksV6 windowCallbacks{sizeof(RecompPrivateTraceCallbacksV6),6,0,0,inputCallbacks,nullptr,
            [](void*,std::uint64_t* generation)noexcept{return frameSurface.acquireForLibrary(generation);},
            [](void*,ANativeWindow* window)noexcept{PrivateFrameSurface::releaseForLibrary(window);},
            [](void*)noexcept{return frameSurface.windowGeneration();}};
        const int result=nativeInput && entryV6?entryV6(static_cast<int>(arguments.size()),argv.data(),&windowCallbacks):
            nativeInput?entryV5(static_cast<int>(arguments.size()),argv.data(),&inputCallbacks):
            (frameMode==4?entryV4(static_cast<int>(arguments.size()),argv.data(),&displayCallbacks):
            (frameMode==3?entryV3(static_cast<int>(arguments.size()),argv.data(),&liveCallbacks):
            (frameMode==2?entryV2(static_cast<int>(arguments.size()),argv.data(),&callbacks):
                entry(static_cast<int>(arguments.size()),argv.data()))));
        std::fflush(stdout);
        if(dup2(original,STDOUT_FILENO)<0)_exit(125);
        close(original);reader.join();
        // Small binary envelope: big-endian return code, then raw private protocol.
        for(int i=0;i<4;++i)protocol[static_cast<std::size_t>(i)]=static_cast<jbyte>(static_cast<unsigned>(result)>>(24-i*8));
        const auto length=static_cast<jsize>(captured+4);
        jbyteArray reply=env->NewByteArray(length);if(!reply)return nullptr;
        env->SetByteArrayRegion(reply,0,length,protocol.data());return reply;
    } catch(...) {return nullptr;}
}

extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_updateGamepad(JNIEnv*,jclass,jboolean connected,
        jint buttons,jint leftTrigger,jint rightTrigger,jint leftX,jint leftY,jint rightX,jint rightY){
    const auto axis=[](int value){return static_cast<std::int16_t>(std::clamp(value,-32768,32767));};
    recomp::input::GamepadState state;
    state.buttons=static_cast<std::uint16_t>(buttons&0xF7FF);
    state.leftTrigger=static_cast<std::uint8_t>(std::clamp(leftTrigger,0,255));
    state.rightTrigger=static_cast<std::uint8_t>(std::clamp(rightTrigger,0,255));
    state.leftX=axis(leftX);state.leftY=axis(leftY);state.rightX=axis(rightX);state.rightY=axis(rightY);
    gamepad.publish(connected,state);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_updateGamepadCapabilities(JNIEnv*,jclass,jint buttons,jint axes,jint triggers){
    recomp::input::GamepadState state;state.buttons=static_cast<std::uint16_t>(buttons&0xF7FF);
    state.leftX=(axes&1)?32767:0;state.leftY=(axes&2)?32767:0;
    state.rightX=(axes&4)?32767:0;state.rightY=(axes&8)?32767:0;
    state.leftTrigger=(triggers&1)?255:0;state.rightTrigger=(triggers&2)?255:0;
    gamepad.capabilities(state);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_readGamepadForDiagnostics(JNIEnv* env,jclass){
    recomp::input::GamepadState state;
    const jint connected=gamepad.read(0,state);
    const jint values[]{connected,static_cast<jint>(state.packet),state.buttons,state.leftTrigger,state.rightTrigger,
            state.leftX,state.leftY,state.rightX,state.rightY};
    jintArray result=env->NewIntArray(9);
    if(result)env->SetIntArrayRegion(result,0,9,values);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_openLibrary(JNIEnv* env,jclass,jstring directory,jint frameMode){
    if(!privateProcess() || (frameMode!=0 && frameMode!=2 && frameMode!=3 && frameMode!=4))return -1;
    const auto path=directoryString(env,directory);if(path.empty())return -2;
    const int child=ownedDirectory(path);
    if(child<0)return -7;
    const int fd=openat(child,libraryName(frameMode),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);close(child);
    if(fd<0)return -5;
    if(!libraryFile(fd)){close(fd);return -6;}
    return fd;
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_checkAbi(JNIEnv* env,jclass,jint descriptor,jstring directory){
    if(!privateProcess() || checked.test_and_set())return 1;
    const auto path=directoryString(env,directory);if(path.empty())return 2;
    const int fd=fcntl(descriptor,F_DUPFD_CLOEXEC,0);
    if(fd<0)return 3;
    if(!libraryFile(fd)){close(fd);return 4;}
    android_dlextinfo extension{};extension.flags=ANDROID_DLEXT_USE_LIBRARY_FD;extension.library_fd=fd;
    void* library=android_dlopen_ext((path+"/private-trace/"+LibraryName).c_str(),RTLD_NOW|RTLD_LOCAL,&extension);
    close(fd);
    if(!library)return 5;
    // Intentionally retain the library for the rest of this dedicated process.
    // No unload/reload is supported, including after rejected ABI behavior.
    using Entry=int (*)(int,char**) noexcept;
    const auto entry=reinterpret_cast<Entry>(dlsym(library,"recomp_run_private_trace_v1"));
    if(!entry || dlsym(library,"main") || dlsym(library,"_xstart"))return 6;
    char name[]="/private-abi-fixture";char* valid[]{name,nullptr};char* nullString[]{nullptr};
    char longArgument[4098];std::memset(longArgument,'x',sizeof(longArgument));longArgument[4097]=0;
    char* invalid[]{longArgument,nullptr};
    if(entry(0,nullptr)!=2 || entry(-1,valid)!=2 || entry(1025,valid)!=2 ||
        entry(1,nullptr)!=2 || entry(1,nullString)!=2 || entry(1,invalid)!=2)return 7;
    int first=-1,second=-1;
    try {
        std::thread one([&]{first=entry(1,valid);});
        try {std::thread two([&]{second=entry(1,valid);});two.join();}
        catch(...){one.join();return 8;}
        one.join();
    } catch(...){return 8;}
    if(!((first==2 && second==3) || (first==3 && second==2)) || entry(1,valid)!=3)return 9;
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_finishCheck(JNIEnv*,jclass){
    if(privateProcess())alarm(0);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_attachFrameSurface(JNIEnv* env,jclass,jobject surface,jint width,jint height,jboolean reject,jboolean failAfterPresent,jboolean rejectSecond){
    if(!privateProcess() || !surface)return false;
    return frameSurface.attach(ANativeWindow_fromSurface(env,surface),width,height,reject,failAfterPresent,rejectSecond);
}
extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_resizeFrameSurface(JNIEnv*,jclass,jint width,jint height){
    if(privateProcess())frameSurface.resize(width,height);
}
extern "C" JNIEXPORT void JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_detachFrameSurface(JNIEnv*,jclass){
    if(privateProcess())frameSurface.detach();
}
