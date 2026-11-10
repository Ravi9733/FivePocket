// SPDX-License-Identifier: GPL-3.0-or-later
// Turnip/custom Adreno driver bootstrap. This library intentionally has no
// libvulkan dependency: it must install the AdrenoTools linker hook BEFORE
// recomp_private_debug (which links libvulkan) is loaded by the private process.

#include <jni.h>
#include <android/log.h>
#include <adrenotools/driver.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {
constexpr char TAG[] = "FivePocketTurnip";
std::atomic_flag attempted = ATOMIC_FLAG_INIT;
void* driverHandle = nullptr;

bool privateProcess() {
    char name[128]{};
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t bytes = read(fd, name, sizeof(name) - 1);
    close(fd);
    return bytes > 0 &&
        std::strcmp(name, "dev.recompbootstrap.smoke:private_trace") == 0;
}

bool safeComponent(const std::string& value) {
    if (value.empty() || value == "." || value == ".." || value.size() > 128) return false;
    for (unsigned char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-')) return false;
    }
    return true;
}

std::string javaString(JNIEnv* env, jstring value) {
    if (!value || env->GetStringUTFLength(value) > 4096) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

bool ownedRegularFile(const std::string& path, off_t maxSize) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 0022) == 0 &&
           st.st_size > 0 && st.st_size <= maxSize;
}

bool ownedDirectory(const std::string& path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 0022) == 0;
}
} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_recompbootstrap_smoke_PrivateTraceActivity_prepareVulkanDriver(
        JNIEnv* env, jclass, jstring filesDirectory, jstring nativeLibraryDirectory,
        jstring cacheDirectory) {
    if (!privateProcess() || attempted.test_and_set()) return JNI_FALSE;

    const std::string files = javaString(env, filesDirectory);
    const std::string nativeLibs = javaString(env, nativeLibraryDirectory);
    const std::string cache = javaString(env, cacheDirectory);
    if (files.empty() || files[0] != '/' || nativeLibs.empty() || nativeLibs[0] != '/') {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Invalid app-private driver paths; using system Vulkan");
        return JNI_FALSE;
    }

    const std::string root = files + "/gpu_drivers";
    const std::string selectionPath = root + "/selected";
    struct stat selectionStat{};
    if (lstat(selectionPath.c_str(), &selectionStat) != 0) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "No custom driver selected; using system Vulkan");
        return JNI_FALSE;
    }
    if (!ownedRegularFile(selectionPath, 1024) || !ownedDirectory(root)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Driver selection is unsafe; using system Vulkan");
        return JNI_FALSE;
    }

    std::ifstream selection(selectionPath);
    std::string folder, library, extra;
    if (!std::getline(selection, folder) || !std::getline(selection, library) ||
        (std::getline(selection, extra) && !extra.empty()) ||
        !safeComponent(folder) || !safeComponent(library) ||
        library.size() < 4 || library.substr(library.size() - 3) != ".so") {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Driver selection is malformed; using system Vulkan");
        return JNI_FALSE;
    }

    const std::string driverDirectory = root + "/" + folder;
    const std::string driverFile = driverDirectory + "/" + library;
    if (!ownedDirectory(driverDirectory) || !ownedRegularFile(driverFile, 512LL * 1024 * 1024)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Selected driver files are missing or unsafe; using system Vulkan");
        return JNI_FALSE;
    }

    const std::string hookDirectory = nativeLibs.back() == '/' ? nativeLibs : nativeLibs + "/";
    const std::string customDirectory = driverDirectory + "/";
    const std::string tempDirectory = cache.empty() ? std::string() :
        (cache.back() == '/' ? cache : cache + "/");

    driverHandle = adrenotools_open_libvulkan(
        RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
        tempDirectory.empty() ? nullptr : tempDirectory.c_str(),
        hookDirectory.c_str(), customDirectory.c_str(), library.c_str(), nullptr, nullptr);

    if (!driverHandle) {
        const char* error = dlerror();
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "AdrenoTools could not activate %s; system Vulkan fallback: %s",
            library.c_str(), error ? error : "unknown error");
        return JNI_FALSE;
    }
    if (!dlsym(driverHandle, "vkGetInstanceProcAddr")) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "Selected driver %s has no vkGetInstanceProcAddr; system Vulkan fallback", library.c_str());
        driverHandle = nullptr;
        return JNI_FALSE;
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Custom Vulkan driver active: %s", driverFile.c_str());
    return JNI_TRUE;
}
