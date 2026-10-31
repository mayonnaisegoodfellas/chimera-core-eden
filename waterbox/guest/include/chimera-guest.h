/* Included before every guest source (guest-toolchain.cmake): what miniBox's
 * C library (musl 1.2.0) does not declare and the emulator's dependencies
 * call. The definitions are in driver/guest-libc.cpp. */
#pragma once
#ifndef __ASSEMBLER__
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* GNU's qsort_r (zstd's dictionary builder; musl has it from 1.2.3) */
void qsort_r(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *, void *), void *arg);
#ifdef __cplusplus
}
#endif
#endif
