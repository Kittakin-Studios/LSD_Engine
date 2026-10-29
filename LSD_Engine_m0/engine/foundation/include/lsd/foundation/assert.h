#pragma once
#include "lsd/foundation/log.h"
#include <cstdlib>

// LSD_ASSERT is active in all non-shipping builds. LSD_VERIFY always evaluates its expression.
#if defined(LSD_SHIPPING)
  #define LSD_ASSERT(expr, ...) ((void)0)
#else
  #define LSD_ASSERT(expr, ...)                                                        \
    do {                                                                               \
      if (!(expr)) {                                                                   \
        ::lsd::log::write(::lsd::log::Level::Fatal, "assert", __FILE__, __LINE__,     \
                          "Assertion failed: " #expr);                                 \
        std::abort();                                                                  \
      }                                                                                \
    } while (0)
#endif

#define LSD_VERIFY(expr) do { if (!(expr)) { LSD_ASSERT(false && #expr); } } while (0)
