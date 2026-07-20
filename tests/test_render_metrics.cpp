#include "render/render_metrics.h"
#include "test.h"

TEST(render_cell_metric_rounds_and_clamps) {
  CHECK_EQ(RenderCellMetric(8.25f), 8.0f);
  CHECK_EQ(RenderCellMetric(8.75f), 9.0f);
  CHECK_EQ(RenderCellMetric(0.25f), 1.0f);
}

// RenderCellMetricsFromAtlas must pass advance through verbatim (already
// normalized at the atlas boundary) and only normalize line_height.
TEST(render_cell_metrics_from_atlas_advance_is_verbatim) {
  f32 atlas_advance = RenderCellMetric(8.25f);  // whole pixel from atlas init
  f32 atlas_line_height = 16.5f;                // fractional, normalized here
  RenderCellMetrics m = RenderCellMetricsFromAtlas(atlas_advance, atlas_line_height);
  CHECK_EQ(m.cell_width, atlas_advance);
  CHECK_EQ(m.cell_height, RenderCellMetric(atlas_line_height));
}

// Text accumulates atlas_advance (what DrawText does via Glyph::advance) while
// the cursor uses cell_width from the derived context (column * ctx->cell_width).
// These are separate variables modelling the two code paths; both must land on
// the same whole-pixel position, which only holds when the atlas normalizes.
TEST(render_cell_metric_does_not_accumulate_fractional_drift) {
  constexpr u64 kColumns = 80;
  f32 atlas_advance = RenderCellMetric(8.25f);  // atlas boundary normalization
  RenderCellMetrics metrics = RenderCellMetricsFromAtlas(atlas_advance, 16.5f);
  f32 cell_width = metrics.cell_width;

  f32 text_end = 0.0f;
  for (u64 column = 0; column < kColumns; column += 1) {
    text_end += atlas_advance;  // Glyph::advance accumulation path
  }

  f32 cursor_x = (f32)kColumns * cell_width;  // cursor column * ctx->cell_width path
  CHECK_EQ(text_end, cursor_x);
  CHECK_EQ(cursor_x, 640.0f);
}
