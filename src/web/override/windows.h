#pragma once
// Stub mínimo de <windows.h> para compilar arquivos portáteis do SDK em WebAssembly.

#include <cstdint>
#include <cstdio>

#ifndef MB_ICONERROR
#define MB_ICONERROR 0x00000010L
#endif
#ifndef MB_TOPMOST
#define MB_TOPMOST 0x00040000L
#endif
#ifndef MB_OK
#define MB_OK 0x00000000L
#endif

#ifndef _fseeki64
#define _fseeki64 fseeko
#endif

inline int MessageBoxW(void*, const wchar_t* text, const wchar_t* caption, unsigned int) {
    std::fprintf(stderr, "[MessageBoxW] %ls: %ls\n", caption ? caption : L"", text ? text : L"");
    return 1;
}
