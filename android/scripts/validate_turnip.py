#!/usr/bin/env python3
"""Static contract checks for FivePocket's optional AdrenoTools/Turnip path."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

def source(path):
    p = ROOT / path
    if not p.is_file():
        raise AssertionError(f"missing Turnip integration file: {path}")
    return p.read_text(encoding="utf-8")

def require(text, pattern, label):
    if re.search(pattern, text, re.MULTILINE | re.DOTALL) is None:
        raise AssertionError(f"missing Turnip contract: {label}")

def main():
    store = source("app/src/main/java/dev/recompbootstrap/smoke/TurnipDriverStore.java")
    screen = source("app/src/main/java/dev/recompbootstrap/smoke/GpuDriverActivity.java")
    native = source("app/src/main/cpp/turnip_bootstrap.cpp")
    cmake = source("app/src/main/cpp/CMakeLists.txt")
    adrenotools_cmake = source("../ext/libadrenotools/CMakeLists.txt")
    hook_cmake = source("../ext/libadrenotools/src/hook/CMakeLists.txt")
    adrenotools_modules = source("../ext/libadrenotools/.gitmodules")
    gradle = source("app/build.gradle.kts")
    manifest = source("app/src/main/AndroidManifest.xml")
    activity = source("app/src/debug/java/dev/recompbootstrap/smoke/PrivateTraceActivity.java")
    about = source("app/src/main/java/dev/recompbootstrap/smoke/AboutScreen.java")
    launcher = source("app/src/main/java/dev/recompbootstrap/smoke/LauncherScreen.java")
    shell = source("app/src/main/java/dev/recompbootstrap/smoke/ShellPages.java")
    modules = (ROOT.parent / ".gitmodules").read_text(encoding="utf-8")

    require(store, r'ZipInputStream', "streaming ZIP extraction")
    require(store, r'new File\(stage,"meta\.json"\)', "preserve ADPKG metadata")
    require(store, r'!safe\(n\)', "reject unsafe ZIP paths")
    require(store, r'MAX_ENTRIES', "bound ZIP entry count")
    require(store, r'MAX_TOTAL', "bound extracted package size")
    require(store, r'verifyElf\(dest\)', "validate every shared library")
    require(store, r'0x7f.*183', "require 64-bit ARM ELF")
    require(store, r'type!=3', "require an ELF shared object")
    require(store, r'libraryName\(m,d\)', "list ADPKGs without optional libraryName")
    require(store, r'Qualcomm.*Mesa', "restrict ADPKG vendor to schema values")
    require(store, r'version\.matches\(', "validate ADPKG driver version format")
    require(store, r'AtomicFile', "atomic active-driver selection")
    require(store, r'Build\.VERSION\.SDK_INT<28', "require Android 9 or newer")
    require(store, r'getFilesDir\(\).*FOLDER', "install in app-private storage")
    require(screen, r'ACTION_OPEN_DOCUMENT', "use Android document picker")
    require(screen, r'Use System Vulkan', "provide system Vulkan fallback")
    require(screen, r'store\.select\(d\)', "activate imported driver")
    require(native, r'private_trace', "limit hook installation to private process")
    require(native, r'selectionPath = root + "/selected"', "read matching selected-driver path")
    require(native, r'std::getline\(selection, folder\).*std::getline\(selection, library\)', "read two-line selection protocol")
    require(native, r'adrenotools_open_libvulkan', "install AdrenoTools Vulkan hook")
    require(native, r'vkGetInstanceProcAddr', "validate Vulkan entry point")
    require(cmake, r'add_subdirectory\([^\n]*ext/libadrenotools', "build libadrenotools")
    require(cmake, r'add_library\(recomp_turnip_bootstrap SHARED turnip_bootstrap\.cpp\)', "build JNI bootstrap")
    require(cmake, r'target_link_libraries\(recomp_turnip_bootstrap PRIVATE adrenotools', "link AdrenoTools")
    require(adrenotools_cmake, r'add_subdirectory\(lib/linkernsbypass\)', "build nested linker namespace bypass")
    require(adrenotools_cmake, r'add_subdirectory\(src/hook\)', "build AdrenoTools hook libraries")
    require(hook_cmake, r'add_library\(main_hook SHARED', "build main linker hook")
    require(hook_cmake, r'add_library\(file_redirect_hook SHARED', "build file redirect hook")
    require(hook_cmake, r'add_library\(gsl_alloc_hook SHARED', "build GSL allocator hook")
    require(hook_cmake, r'add_library\(hook_impl SHARED', "build hook implementation")
    require(adrenotools_modules, r'lib/linkernsbypass', "pin nested linker namespace bypass submodule")
    require(gradle, r'useLegacyPackaging\s*=\s*true', "extract linker hook libraries")
    require(manifest, r'android:name="\.GpuDriverActivity"', "register driver screen")
    require(about, r'ABOUT_GPU_DRIVERS', "add GPU Drivers About page")
    require(shell, r'"GPU Drivers"', "show GPU Drivers menu item")
    require(launcher, r'about\.startGpuDrivers\(\)', "open driver manager from menu")
    require(activity, r'System\.loadLibrary\("recomp_turnip_bootstrap"\).*prepareVulkanDriver.*System\.loadLibrary\("recomp_private_debug"\)', "install hook before Vulkan-linked loader")
    require(activity, r'catch \(UnsatisfiedLinkError unavailable\).*system Vulkan', "preserve system fallback if bootstrap cannot load")
    require(modules, r'ext/libadrenotools', "pin AdrenoTools source submodule")
    print("TURNIP STATIC CHECKS PASS")

if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print(f"TURNIP STATIC CHECKS FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
