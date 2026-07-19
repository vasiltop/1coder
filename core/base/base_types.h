#pragma once

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;
typedef i32      b32;

#define ArrayCount(a) (sizeof(a) / sizeof((a)[0]))

#define Min(a, b) ((a) < (b) ? (a) : (b))
#define Max(a, b) ((a) > (b) ? (a) : (b))
#define Clamp(lo, x, hi) Min(Max(x, lo), hi)

#define KB(n) ((u64)(n) << 10)
#define MB(n) ((u64)(n) << 20)
#define GB(n) ((u64)(n) << 30)

#define Stmt(s) do { s } while (0)

#define AlignPow2(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#if defined(NDEBUG)
#  define Assert(c) ((void)0)
#else
#  include <stdio.h>
#  include <stdlib.h>
#  define Assert(c)                                                          \
     Stmt(if (!(c)) {                                                        \
       fprintf(stderr, "assert failed: %s\n  at %s:%d\n", #c, __FILE__, __LINE__); \
       abort();                                                              \
     })
#endif

#define Unreachable() Assert(!"unreachable")
#define NotImplemented() Assert(!"not implemented")

// Scope-exit helper. Used sparingly, mostly to pair Scratch begin/end.
namespace base {

template <typename F>
struct DeferHolder {
  F f;
  ~DeferHolder() { f(); }
};

struct DeferMaker {
  template <typename F>
  DeferHolder<F> operator+(F f) const { return DeferHolder<F>{f}; }
};

}  // namespace base

#define DeferGlue_(a, b) a##b
#define DeferGlue(a, b) DeferGlue_(a, b)
#define defer auto DeferGlue(defer_, __LINE__) = ::base::DeferMaker{} + [&]()

// Bitwise operators for a scoped enum used as a flag set. Keeps flags type-safe
// without giving up `|` and `&` at the call site.
#define ENUM_FLAG_OPS(T)                                                       \
  constexpr T operator|(T a, T b) {                                            \
    using U = std::underlying_type_t<T>;                                       \
    return (T)((U)a | (U)b);                                                   \
  }                                                                            \
  constexpr T operator&(T a, T b) {                                            \
    using U = std::underlying_type_t<T>;                                       \
    return (T)((U)a & (U)b);                                                   \
  }                                                                            \
  constexpr T operator^(T a, T b) {                                            \
    using U = std::underlying_type_t<T>;                                       \
    return (T)((U)a ^ (U)b);                                                   \
  }                                                                            \
  constexpr T operator~(T a) {                                                 \
    using U = std::underlying_type_t<T>;                                       \
    return (T)(~(U)a);                                                         \
  }                                                                            \
  constexpr T &operator|=(T &a, T b) { return a = a | b; }                     \
  constexpr T &operator&=(T &a, T b) { return a = a & b; }                     \
  constexpr T &operator^=(T &a, T b) { return a = a ^ b; }                     \
  [[nodiscard]] constexpr bool HasFlag(T set, T flag) {                        \
    using U = std::underlying_type_t<T>;                                       \
    return ((U)set & (U)flag) == (U)flag && (U)flag != 0;                      \
  }                                                                            \
  [[nodiscard]] constexpr bool HasAny(T set, T flag) {                         \
    using U = std::underlying_type_t<T>;                                       \
    return ((U)set & (U)flag) != 0;                                            \
  }                                                                            \
  constexpr void SetFlag(T &set, T flag, bool on) {                            \
    if (on) set |= flag; else set &= ~flag;                                    \
  }
