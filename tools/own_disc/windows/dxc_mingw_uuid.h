/* Interface identifiers of the DirectX Shader Compiler for a MinGW build of the shader translator.

   DXC's dxcapi.h gives each interface its identifier with __declspec(uuid("...")). MinGW's headers do not read that:
   their __uuidof is a template (__mingw_uuidof<T>) that wants one __CRT_UUID_DECL per interface, and without it the
   translator does not link (undefined __mingw_uuidof<IDxcCompiler3>() and three more). This header declares the four
   interfaces the translator asks for, with the identifiers dxcapi.h names for them. It is handed to the compiler
   with -include, so no file of the translator or of DXC is changed. A wrong identifier here is refused by the
   compiler library at run time (no interface), it cannot give a different shader.

   Nothing here comes from the game. The identifiers are those of DXC release v1.9.2607 (inc/dxcapi.h). */
#if defined(__cplusplus) && defined(__MINGW32__)
#include <guiddef.h>
struct IDxcBlob;
struct IDxcBlobUtf8;
struct IDxcResult;
struct IDxcCompiler3;
__CRT_UUID_DECL(IDxcBlob, 0x8BA5FB08, 0x5195, 0x40e2, 0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02)
__CRT_UUID_DECL(IDxcBlobUtf8, 0x3DA636C9, 0xBA71, 0x4024, 0xA3, 0x01, 0x30, 0xCB, 0xF1, 0x25, 0x30, 0x5B)
__CRT_UUID_DECL(IDxcResult, 0x58346CDA, 0xDDE7, 0x4497, 0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59)
__CRT_UUID_DECL(IDxcCompiler3, 0x228B4687, 0x5A6A, 0x4730, 0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54)
#endif
