#!/usr/bin/env python3
"""Host-side structural checks for the asset-free Android bootstrap."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "dev.recompbootstrap.smoke"
ABI = "arm64-v8a"


def require(path: str, pattern: str, description: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    if re.search(pattern, text, re.MULTILINE) is None:
        raise AssertionError(f"{path}: missing {description}")


def main() -> int:
    required_files = (
        "settings.gradle.kts",
        "build.gradle.kts",
        "gradlew",
        "gradlew.bat",
        "gradle/wrapper/gradle-wrapper.jar",
        "gradle/wrapper/gradle-wrapper.properties",
        "app/build.gradle.kts",
        "app/src/main/AndroidManifest.xml",
        "app/src/main/java/dev/recompbootstrap/smoke/BootstrapActivity.java",
        "app/src/main/java/dev/recompbootstrap/smoke/NativeBridge.java",
        "app/src/main/cpp/CMakeLists.txt",
        "app/src/main/cpp/guest_address_space.cpp",
        "app/src/main/cpp/guest_address_space.h",
        "app/src/main/cpp/guest_execution_guard.cpp",
        "app/src/main/cpp/guest_execution_guard.h",
        "app/src/main/cpp/guest_cooperative_executor.cpp",
        "app/src/main/cpp/guest_cooperative_executor.h",
        "app/src/main/cpp/guest_thread_entry.h",
        "app/src/main/cpp/guest_image_loader.cpp",
        "app/src/main/cpp/guest_image_loader.h",
        "app/src/main/cpp/guest_import_registry.cpp",
        "app/src/main/cpp/guest_import_registry.h",
        "app/src/main/cpp/guest_thread_storage_pool.cpp",
        "app/src/main/cpp/guest_thread_storage_pool.h",
        "app/src/main/cpp/guest_title_environment.cpp",
        "app/src/main/cpp/guest_title_environment.h",
        "app/src/main/cpp/guest_xex_header.cpp",
        "app/src/main/cpp/guest_xex_header.h",
        "app/src/main/cpp/smoke_jni.cpp",
        "app/src/main/cpp/smoke_runtime.cpp",
        "app/src/main/cpp/vulkan_smoke.cpp",
        "app/src/main/cpp/vulkan_smoke.h",
    )
    missing = [path for path in required_files if not (ROOT / path).is_file()]
    if missing:
        raise AssertionError("missing files: " + ", ".join(missing))

    require("app/build.gradle.kts", rf'applicationId\s*=\s*"{re.escape(PACKAGE)}"', "package ID")
    require("app/build.gradle.kts", rf'abiFilters\s*\+=\s*"{ABI}"', "ARM64-only ABI filter")
    require("app/build.gradle.kts", r'buildToolsVersion\s*=\s*"35\.0\.0"', "pinned Android build tools")
    require("app/build.gradle.kts", r'ndkVersion\s*=\s*"[0-9.]+"', "pinned NDK")
    require("app/build.gradle.kts", r'version\s*=\s*"[0-9.]+"', "pinned CMake")
    require("build.gradle.kts", r'com\.android\.application.*version\s*"[0-9.]+"', "pinned Android plugin")
    require("gradle/wrapper/gradle-wrapper.properties", r'gradle-8\.9-bin\.zip', "pinned Gradle 8.9 distribution")
    require("gradle/wrapper/gradle-wrapper.properties", r'distributionSha256Sum=[0-9a-f]{64}', "Gradle distribution checksum")
    require("app/src/main/AndroidManifest.xml", r'android:usesCleartextTraffic="false"', "cleartext network opt-out")
    require("app/src/debug/AndroidManifest.xml", r'android:process=":private_trace"', "separate private diagnostic process")
    require("app/src/debug/cpp/private_trace_jni.cpp", r'ANDROID_DLEXT_USE_LIBRARY_FD', "same-descriptor private library loading")
    require("app/src/debug/cpp/private_trace_jni.cpp", r'O_NOFOLLOW', "private library symlink rejection")
    require("app/src/debug/cpp/private_trace_jni.cpp", r'alarm\(traceRun\?45:15\)', "bounded per-mode wall deadline")
    require("app/src/debug/java/dev/recompbootstrap/smoke/PrivateTraceActivity.java", r'MessageDigest.getInstance\("SHA-256"\)', "explicit library integrity check")
    for path in ("app/src/main/AndroidManifest.xml", "app/src/main/java/dev/recompbootstrap/smoke/BootstrapActivity.java"):
        if "PrivateTraceActivity" in (ROOT/path).read_text(encoding="utf-8"):
            raise AssertionError("private trace launch must stay in the debug source set")
    require("app/src/main/cpp/smoke_jni.cpp", r'Java_dev_recompbootstrap_smoke_NativeBridge_create', "JNI create entry point")
    require("app/src/main/cpp/smoke_jni.cpp", r'ANativeWindow_fromSurface', "JNI native-window adapter")
    require("app/src/main/cpp/guest_address_space.h", r'std::uint64_t\{1\}\s*<<\s*32U', "4 GiB guest address space")
    require("app/src/main/cpp/guest_execution_guard.cpp", r'FunctionEntryBudgetExceeded', "cooperative guest entry budget")
    require("app/src/main/cpp/guest_execution_guard.cpp", r'UnimplementedImport', "structured missing-import stop")
    require("app/src/main/cpp/guest_image_loader.cpp", r'loadExplicitDecodedImage', "explicit decoded-image loader")
    require("app/src/main/cpp/guest_image_loader.cpp", r'is_absolute\(\)', "absolute-path requirement")
    require("app/src/main/cpp/guest_image_loader.cpp", r'symlink_status', "symlink rejection")
    require("app/src/main/cpp/guest_image_loader.cpp", r'O_NOFOLLOW', "race-resistant POSIX open")
    require("app/src/main/cpp/guest_image_loader.cpp", r'FILE_FLAG_OPEN_REPARSE_POINT', "race-resistant Windows open")
    require("app/src/main/cpp/guest_image_loader.cpp", r'GuestAddressSpace::Size\s*-\s*expectedSize', "guest range boundary check")
    require("app/src/main/cpp/guest_import_registry.cpp", r'ImplementedBootImports', "bounded boot import registry")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__ExCreateThread', "bounded suspended-thread creation")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__ObReferenceObjectByHandle', "thread object reference lifetime")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__KeQueryBasePriorityThread', "relative thread priority metadata")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__NtCreateSemaphore', "bounded semaphore creation")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__NtCreateEvent', "bounded event creation")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__NtWaitForSingleObjectEx', "cooperative immediate waits")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__DbgPrint', "portable logging shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__NtAllocateVirtualMemory', "portable guest heap allocation shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__NtFreeVirtualMemory', "portable guest heap release shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__MmAllocatePhysicalMemoryEx', "bounded physical-memory allocation shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__MmAllocatePhysicalMemory', "default physical-memory allocation shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__MmFreePhysicalMemory', "owned physical-memory release shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__MmGetPhysicalAddress', "physical-address translation shim")
    require("app/src/main/cpp/guest_import_registry.h", r'bindPhysicalMemoryArena', "explicit physical-memory arena binding")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__RtlCompareMemoryUlong', "bounded memory comparison shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__RtlInitAnsiString', "ANSI descriptor shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__RtlInitUnicodeString', "Unicode descriptor shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__RtlImageXexHeaderField', "bounded XEX-header lookup shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__RtlInitializeCriticalSection', "critical-section initializer shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__RtlInitializeCriticalSectionAndSpinCount', "critical-section spin initializer shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__XexCheckExecutablePrivilege', "executable privilege shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__KeTlsAlloc', "portable TLS slot allocation")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__KeTlsGetValue', "portable TLS value access")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__RtlEnterCriticalSection', "cooperative critical-section enter shim")
    require("app/src/main/cpp/guest_import_registry.cpp", r'__imp__RtlLeaveCriticalSection', "cooperative critical-section leave shim")
    require("app/src/main/cpp/guest_import_registry.h", r'bindGuestThreadToken', "explicit guest thread-token binding")
    require("app/src/main/cpp/guest_import_registry.h", r'bindGuestThreadTlsStorage', "explicit dynamic guest TLS backing")
    require("app/src/main/cpp/guest_import_registry.h", r'teardownGuestThread', "guest thread TLS teardown")
    require("app/src/main/cpp/CMakeLists.txt", r'guest_thread_storage_pool\.cpp', "portable thread storage pool build")
    require("app/src/main/cpp/guest_title_environment.cpp", r'ModuleXexHeaderOffset', "synthetic module XEX-header link")
    require("app/src/main/cpp/guest_title_environment.cpp", r'originalImportSlotValue_', "reversible executable-module data import")
    require("app/src/main/cpp/guest_xex_header.cpp", r'MaximumOptionalHeaders', "bounded XEX optional-header count")
    require("app/src/main/cpp/vulkan_smoke.cpp", r'vkCreateAndroidSurfaceKHR', "Vulkan Android surface")
    require("app/src/main/cpp/vulkan_smoke.cpp", r'VK_ATTACHMENT_LOAD_OP_CLEAR', "Vulkan clear pass")
    require("app/src/main/cpp/vulkan_smoke.cpp", r'vkQueuePresentKHR', "Vulkan presentation")
    require("app/src/main/java/dev/recompbootstrap/smoke/BootstrapActivity.java", r'SurfaceHolder\.Callback', "SurfaceView lifecycle")

    startup_text = "\n".join(
        (ROOT / path).read_text(encoding="utf-8")
        for path in (
            "app/src/main/cpp/smoke_jni.cpp",
            "app/src/main/cpp/smoke_runtime.cpp",
        )
    )
    if "GuestImageLoader" in startup_text or "loadExplicitDecodedImage" in startup_text:
        raise AssertionError("asset-free JNI/runtime startup must not invoke the guest image loader")
    if "GuestImportRegistry" in startup_text or "_xstart" in startup_text:
        raise AssertionError("asset-free JNI/runtime startup must not dispatch imports or guest entry")
    if "GuestTitleEnvironment" in startup_text:
        raise AssertionError("asset-free JNI/runtime startup must not initialize a guest title")
    if "GuestThreadStoragePool" in startup_text:
        raise AssertionError("asset-free JNI/runtime startup must not prepare guest threads")
    if "GuestCooperativeExecutor" in startup_text:
        raise AssertionError("asset-free JNI/runtime startup must not execute guest threads")

    game_payload_suffixes = {
        ".xex", ".xexp", ".iso", ".rpf", ".xcp",
        ".awc", ".fxc", ".xtd", ".xft", ".xdd",
    }
    payloads = [
        path for path in ROOT.rglob("*")
        if path.is_file() and path.suffix.lower() in game_payload_suffixes
    ]
    if payloads:
        raise AssertionError("game payloads must not be packaged: " + ", ".join(map(str, payloads)))

    print("Android bootstrap configuration is valid (ARM64, asset-free, pinned toolchain).")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        sys.exit(1)
