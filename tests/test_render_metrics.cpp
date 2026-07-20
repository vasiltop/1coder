#include "render/render_metrics.h"
#include "test.h"

TEST(render_cell_metric_rounds_and_clamps) {
  CHECK_EQ(RenderCellMetric(8.25f), 8.0f);
  CHECK_EQ(RenderCellMetric(8.75f), 9.0f);
  CHECK_EQ(RenderCellMetric(0.25f), 1.0f);
}

TEST(render_cell_metric_does_not_accumulate_fractional_drift) {
  constexpr u64 kColumns = 80;
  f32 cell_width = RenderCellMetric(8.25f);

  f32 text_end = 0.0f;
  for (u64 column = 0; column < kColumns; column += 1) {
    text_end += cell_width;
  }

  f32 cursor_x = (f32)kColumns * cell_width;
  CHECK_EQ(text_end, cursor_x);
  CHECK_EQ(cursor_x, 640.0f);
}
