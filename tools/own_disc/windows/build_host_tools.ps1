# Builds the builder's host programs for Windows and lays them out as the profile names them (${hosttools}).
#
#   build_host_tools.ps1 -Source DIR -Generator DIR -GeneratorExe FILE -Dxc DIR -Mingw DIR -CMake FILE
#                        -Ninja FILE -Work NEW_DIR -Output NEW_DIR
#
# -Source        the host sources as sent from the first machine: tools\xex_image, tools\xmem_lzx, the shader
#                translator's base tree (xenos\base, itself the plain v41 state) and the changed files of its other
#                states (xenos\main, xenos\gradient, xenos\reglod; since 2026-10-24 xenos\coissue and
#                xenos\coissue-reglod, the fix tables' translators, when present), and support\ (dxc_mingw_uuid.h,
#                win_binary_stdio.cpp from tools\own_disc\windows).
# -Generator     the code generator's source tree (XenonRecomp with the project's patches); the two small tools
#                build against its third-party folder. -GeneratorExe is its build with the calling-wrapper form.
# -Dxc           the publisher's DirectX Shader Compiler release, unpacked (bin\x64, lib\x64, inc).
# -Mingw         llvm-mingw's bin folder; -CMake and -Ninja the two programs.
# -Work, -Output two folders that must not exist: staging and build trees, and the finished host-tools folder.
#
# Nothing outside -Work and -Output is written. Every program is linked statically, so the folder needs nothing
# installed beside it; the compiler library (dxcompiler.dll) lies in dxc\lib, and the shader tools put that folder in
# front of PATH for the programs they start (shader_toolchain.py). The folder holds no path of this machine: the
# shader stages name its programs themselves, so it can be moved or packed as it is.
param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Generator, [Parameter(Mandatory)][string]$GeneratorExe,
      [Parameter(Mandatory)][string]$Dxc, [Parameter(Mandatory)][string]$Mingw,
      [Parameter(Mandatory)][string]$CMake, [Parameter(Mandatory)][string]$Ninja,
      [Parameter(Mandatory)][string]$Work, [Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
foreach ($new in $Work, $Output) { if (Test-Path $new) { throw "$new exists already: nothing is written over" } }
foreach ($need in "$Source\tools\xex_image", "$Source\tools\xmem_lzx", "$Source\xenos\base", "$Source\support\dxc_mingw_uuid.h",
                  "$Source\support\win_binary_stdio.cpp", "$Generator\XenonUtils\ppc_context.h", $GeneratorExe,
                  "$Dxc\bin\x64\dxc.exe", "$Dxc\bin\x64\dxcompiler.dll", "$Dxc\bin\x64\dxil.dll", "$Dxc\lib\x64\dxcompiler.lib", "$Dxc\inc\dxcapi.h",
                  "$Mingw\clang++.exe", $CMake, $Ninja) {
  if (-not (Test-Path $need)) { throw "missing: $need" }
}
$env:PATH = "$Mingw;" + (Split-Path $Ninja) + ';' + $env:PATH
function Slashes($path) { $path.Replace('\', '/') }
function Sha($file) { (Get-FileHash $file -Algorithm SHA256).Hash.ToLower() }
# (through cmd: Windows PowerShell turns a native program's error stream into errors of its own)
function Configure($name, $sourceDir, $flags) {
  cmd /c "`"$CMake`" -S `"$sourceDir`" -B `"$Work\build\$name`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_C_COMPILER=`"$Mingw\clang.exe`" -DCMAKE_CXX_COMPILER=`"$Mingw\clang++.exe`" `"-DCMAKE_CXX_FLAGS=$flags`" -DCMAKE_EXE_LINKER_FLAGS=-static -DCMAKE_MAKE_PROGRAM=`"$Ninja`" > `"$Work\build\$name-configure.log`" 2>&1"
  if ($LASTEXITCODE -ne 0) { Get-Content "$Work\build\$name-configure.log" -Tail 30; throw "configure of $name failed" }
}
function Build($name, $targets) {
  cmd /c "`"$CMake`" --build `"$Work\build\$name`" $targets > `"$Work\build\$name-build.log`" 2>&1"
  if ($LASTEXITCODE -ne 0) { Get-Content "$Work\build\$name-build.log" -Tail 45; throw "build of $name failed" }
  "built $name"
}
New-Item -ItemType Directory "$Work\src\tools", "$Work\build" | Out-Null

# 1. the two small tools; their build files look for the generator's tree beside them
Copy-Item "$Source\tools\xex_image" "$Work\src\tools\xex_image" -Recurse
Copy-Item "$Source\tools\xmem_lzx" "$Work\src\tools\xmem_lzx" -Recurse
Copy-Item $Generator "$Work\src\tools\XenonRecomp" -Recurse
foreach ($name in 'xex_image', 'xmem_lzx') {
  Configure $name "$Work\src\tools\$name" '-include cstdlib'       # (llvm-mingw's C++ library wants it for the formatting library)
  Build $name ''
}
cmd /c "`"$Work\build\xmem_lzx\xmem_lzx_tests.exe`" > `"$Work\build\xmem_lzx-tests.log`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw 'the decoder tests failed' }

# 2. the shader translator in its states and its checker: one base tree, the changed files of a state laid over a copy
#    of it, one build file that names the DXC release as the compiler library. The base tree's own translator is the
#    plain v41 state (xenos\v41: the control of build_shader_fixes.py).
$support = Slashes "$Source\support"; $dxcAt = Slashes $Dxc
$wrapper = @"
cmake_minimum_required(VERSION 3.20)
project(XenosRecompWindows C CXX)
add_library(Microsoft::DirectXShaderCompiler SHARED IMPORTED GLOBAL)
set_target_properties(Microsoft::DirectXShaderCompiler PROPERTIES
    IMPORTED_LOCATION "$dxcAt/bin/x64/dxcompiler.dll"
    IMPORTED_IMPLIB "$dxcAt/lib/x64/dxcompiler.lib"
    INTERFACE_INCLUDE_DIRECTORIES "$dxcAt/inc")
set(XENOS_RECOMP_AIR OFF CACHE BOOL "" FORCE)
set(XENOS_RECOMP_DXIL OFF CACHE BOOL "" FORCE)
add_subdirectory("`${CMAKE_CURRENT_LIST_DIR}/../src" xenos)
# the checker takes its containers on its standard input: binary there on Windows too
target_sources(XenosShaderPreflight PRIVATE "$support/win_binary_stdio.cpp")
"@
# <atomic>: the translator uses it without naming it; the identifiers: MinGW's headers do not read DXC's own form
$xenosFlags = "-include cstdlib -include atomic -include $support/dxc_mingw_uuid.h"
$states = @('base', 'main', 'gradient', 'reglod') + @('coissue', 'coissue-reglod' | Where-Object { Test-Path "$Source\xenos\$_" })
foreach ($state in $states) {
  $root = "$Work\src\xenos\$state"
  New-Item -ItemType Directory "$root\wrapper" | Out-Null
  Copy-Item "$Source\xenos\base" "$root\src" -Recurse
  if ($state -ne 'base') { Copy-Item "$Source\xenos\$state\*" "$root\src" -Recurse -Force }
  [IO.File]::WriteAllText("$root\wrapper\CMakeLists.txt", $wrapper.Replace("`r`n", "`n"))
  Configure "xenos-$state" "$root\wrapper" $xenosFlags
  Build "xenos-$state" $(if ($state -eq 'base') { '--target XenosShaderPreflight XenosRecomp' } else { '--target XenosRecomp' })
}

# 3. the folder the profile names
New-Item -ItemType Directory $Output, "$Output\dxc\bin", "$Output\dxc\lib" | Out-Null
Copy-Item $GeneratorExe "$Output\XenonRecomp.exe"
Copy-Item "$Generator\XenonUtils\ppc_context.h" "$Output\ppc_context.h"
Copy-Item "$Work\build\xex_image\xex_image.exe" "$Output\xex_image.exe"
Copy-Item "$Work\build\xmem_lzx\xmem-lzx.exe" "$Output\xmem-lzx.exe"
foreach ($state in $states) {
  $folder = if ($state -eq 'base') { 'v41' } else { $state }
  New-Item -ItemType Directory "$Output\xenos\$folder" | Out-Null
  Copy-Item (Get-ChildItem "$Work\build\xenos-$state" -Recurse -Filter XenosRecomp.exe | Select-Object -First 1).FullName "$Output\xenos\$folder\XenosRecomp.exe"
  Copy-Item "$Work\src\xenos\$state\src\XenosRecomp\shader_common.h" "$Output\xenos\$folder\shader_common.h"
}
Copy-Item (Get-ChildItem "$Work\build\xenos-base" -Recurse -Filter XenosShaderPreflight.exe | Select-Object -First 1).FullName "$Output\xenos\main\XenosShaderPreflight.exe"
Copy-Item "$Dxc\bin\x64\dxc.exe" "$Output\dxc\bin\dxc-3.7.exe"
Copy-Item "$Dxc\bin\x64\dxcompiler.dll", "$Dxc\bin\x64\dxil.dll" "$Output\dxc\lib"
Get-ChildItem $Dxc -File | Where-Object { $_.Name -match '^LICEN' } | ForEach-Object { Copy-Item $_.FullName "$Output\dxc\$($_.Name)" }
'--- ' + $Output
Get-ChildItem $Output -Recurse -File | ForEach-Object { "{0,10} {1} {2}" -f $_.Length, (Sha $_.FullName), $_.FullName.Substring($Output.Length + 1) }
