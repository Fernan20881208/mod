#pragma once

// feature_pack.cpp still contains two legacy CCARRAY_FOREACH loops inside
// iOS-only safe-area code. Geode 5 intentionally turns the old macro into a
// compile-time error. Include Geode first, then provide the legacy iteration
// spelling locally so the existing iOS code can compile without affecting the
// other platform builds.
#include <Geode/Geode.hpp>

#ifdef CCARRAY_FOREACH
#undef CCARRAY_FOREACH
#endif

#define CCARRAY_FOREACH(__array__, __object__) \
    for (unsigned int __ci_index = 0; \
         (__array__) != nullptr && __ci_index < (__array__)->count() && \
             (((__object__) = (__array__)->objectAtIndex(__ci_index)), true); \
         ++__ci_index)
