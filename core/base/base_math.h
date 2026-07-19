#pragma once

#include "base/base_types.h"

enum class Axis2 : u8 { X, Y, COUNT };

enum class Dir2 : u8 { Left, Right, Up, Down, COUNT };

[[nodiscard]] constexpr Axis2 AxisFromDir(Dir2 d) {
  return (d == Dir2::Left || d == Dir2::Right) ? Axis2::X : Axis2::Y;
}

struct Vec2F32 {
  f32 x, y;
};

struct Vec2S32 {
  i32 x, y;
};

struct Vec4F32 {
  f32 x, y, z, w;
};

// Half-open byte range [min, max). The universal selection/edit type: visual
// mode, operators, undo records and syntax dirty ranges all speak it.
struct RangeU64 {
  u64 min, max;
};

[[nodiscard]] constexpr RangeU64 RangeMake(u64 a, u64 b) {
  return (a <= b) ? RangeU64{a, b} : RangeU64{b, a};
}

[[nodiscard]] constexpr u64 RangeSize(RangeU64 r) { return r.max - r.min; }

[[nodiscard]] constexpr bool RangeEmpty(RangeU64 r) { return r.max <= r.min; }

[[nodiscard]] constexpr bool RangeContains(RangeU64 r, u64 v) {
  return v >= r.min && v < r.max;
}

[[nodiscard]] constexpr bool RangeOverlaps(RangeU64 a, RangeU64 b) {
  return a.min < b.max && b.min < a.max;
}

[[nodiscard]] constexpr RangeU64 RangeIntersect(RangeU64 a, RangeU64 b) {
  u64 lo = Max(a.min, b.min);
  u64 hi = Min(a.max, b.max);
  return (lo < hi) ? RangeU64{lo, hi} : RangeU64{lo, lo};
}

// Rect in cells. The core lays out panels in cells so that nothing below the
// renderer needs font metrics. x1/y1 are exclusive.
struct RectS32 {
  i32 x0, y0, x1, y1;
};

[[nodiscard]] constexpr i32 RectWidth(RectS32 r) { return r.x1 - r.x0; }
[[nodiscard]] constexpr i32 RectHeight(RectS32 r) { return r.y1 - r.y0; }

[[nodiscard]] constexpr bool RectContains(RectS32 r, i32 x, i32 y) {
  return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
}

[[nodiscard]] constexpr bool RectEmpty(RectS32 r) {
  return RectWidth(r) <= 0 || RectHeight(r) <= 0;
}

// Rect in pixels. Renderer-side only.
struct RectF32 {
  f32 x0, y0, x1, y1;
};

[[nodiscard]] constexpr f32 RectWidthF(RectF32 r) { return r.x1 - r.x0; }
[[nodiscard]] constexpr f32 RectHeightF(RectF32 r) { return r.y1 - r.y0; }
