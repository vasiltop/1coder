#pragma once

#include "base/base_types.h"

[[nodiscard]] constexpr f32 RenderCellMetric(f32 metric) {
  f32 rounded = (f32)(i32)(metric + 0.5f);
  return Max(rounded, 1.0f);
}
