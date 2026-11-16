#pragma once
// Wrapper de core/diag.h para WebAssembly evitando _wgetenv do Windows CRT.
#ifndef _wgetenv
#define _wgetenv(x) ((const wchar_t*)nullptr)
#define BLACK_UNDEF_WGETENV 1
#endif

#include_next "core/diag.h"

#ifdef BLACK_UNDEF_WGETENV
#undef _wgetenv
#undef BLACK_UNDEF_WGETENV
#endif
