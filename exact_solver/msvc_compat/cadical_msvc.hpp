#pragma once

#ifdef _MSC_VER
#include <intrin.h>

// CaDiCaL 2.1.3 directly spells the small GCC/Clang surface below.  Keep the
// pinned upstream tree pristine and provide the MSVC equivalents as a forced
// compatibility include for the cadical target only.
#define __attribute__(arguments)
#define __PRETTY_FUNCTION__ __FUNCSIG__
#define __builtin_prefetch(...) ((void)0)

static inline int cadical_msvc_count_leading_zeros(unsigned value) {
    if (!value) return 32;
    unsigned long index = 0;
    _BitScanReverse(&index, value);
    return 31 - static_cast<int>(index);
}
#define __builtin_clz cadical_msvc_count_leading_zeros
#endif
