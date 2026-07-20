#pragma once

#include "base/base_types.h"

[[nodiscard]] constexpr f32 RenderCellMetric(f32 metric) {
  f32 rounded = (f32)(i32)(metric + 0.5f);
  return Max(rounded, 1.0f);
}

// Derives cell dimensions from atlas metrics.  advance is already normalised
// at the atlas boundary (GlyphAtlasInit calls RenderCellMetric), so it is
// passed through verbatim; line_height is fractional and is normalised here.
struct RenderCellMetrics {
  f32 cell_width;
  f32 cell_height;
};

[[nodiscard]] constexpr RenderCellMetrics RenderCellMetricsFromAtlas(f32 atlas_advance,
                                                                     f32 atlas_line_height) {
  return {atlas_advance, RenderCellMetric(atlas_line_height)};
}
