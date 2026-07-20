#include "buffers/buf_image.h"
#include "editor/editor.h"
#include "input/mouse.h"
#include "test.h"

#include <math.h>

namespace {

struct Fixture {
  Arena *arena;
  Editor ed;
};

Fixture MakeFixture(const char *text) {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(64));
  EditorInit(&f.ed, f.arena, RectS32{0, 0, 80, 25});

  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  BufferSetText(&f.ed, buffer, Str8C(text));
  ViewSetCursor(EditorFocusedView(&f.ed), buffer, 0);
  return f;
}

void Destroy(Fixture *f) {
  EditorDestroy(&f->ed);
  ArenaRelease(f->arena);
}

MouseEvent MouseAt(f32 grid_x, f32 grid_y) {
  MouseEvent event = {};
  event.grid_x = grid_x;
  event.grid_y = grid_y;
  event.x = (i32)floorf(grid_x);
  event.y = (i32)floorf(grid_y);
  return event;
}

MouseHit Hit(Fixture *f, f32 grid_x, f32 grid_y) {
  return EditorMouseHitTest(&f->ed, MouseAt(grid_x, grid_y));
}

}  // namespace

TEST(mouse_hit_text_accounts_for_gutter_and_scroll) {
  Fixture f = MakeFixture("zero\none two\nthree four\nfive");

  View *view = EditorFocusedView(&f.ed);
  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  Panel *panel = f.ed.focused_panel;
  RectS32 text = EditorPanelTextRect(&f.ed, panel);

  view->scroll_line = 1;
  view->scroll_column = 2;

  MouseHit hit = Hit(&f, (f32)text.x0 + 1.2f, (f32)text.y0 + 1.4f);
  CHECK_EQ((u32)hit.kind, (u32)MouseHitKind::Text);
  CHECK(hit.panel == panel);
  CHECK(hit.view == view);
  CHECK(hit.buffer == buffer);
  CHECK(hit.has_offset);
  CHECK_EQ(hit.offset, BufferOffsetFromColumn(buffer, 2, 3));

  Destroy(&f);
}

TEST(mouse_hit_text_clamps_utf8_and_short_lines) {
  Fixture f = MakeFixture("éx\n\nab");

  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  RectS32 text = EditorPanelTextRect(&f.ed, f.ed.focused_panel);

  MouseHit utf8 = Hit(&f, (f32)text.x0 + 1.1f, (f32)text.y0 + 0.1f);
  CHECK_EQ((u32)utf8.kind, (u32)MouseHitKind::Text);
  CHECK(utf8.has_offset);
  CHECK_EQ(utf8.offset, 2);

  MouseHit empty = Hit(&f, (f32)text.x0 + 12.0f, (f32)text.y0 + 1.1f);
  CHECK(empty.has_offset);
  CHECK_EQ(empty.offset, BufferOffsetFromLine(buffer, 1));

  MouseHit short_line = Hit(&f, (f32)text.x0 + 12.0f, (f32)text.y0 + 2.1f);
  CHECK(short_line.has_offset);
  CHECK_EQ(short_line.offset, BufferLineEnd(buffer, 2));

  Destroy(&f);
}

TEST(mouse_hit_gutter_returns_visible_line_start) {
  Fixture f = MakeFixture("zero\none\ntwo\nthree\nfour");

  View *view = EditorFocusedView(&f.ed);
  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  Panel *panel = f.ed.focused_panel;
  RectS32 text = EditorPanelTextRect(&f.ed, panel);

  view->scroll_line = 2;

  MouseHit hit = Hit(&f, (f32)panel->rect.x0 + 1.0f, (f32)text.y0 + 1.2f);
  CHECK_EQ((u32)hit.kind, (u32)MouseHitKind::Gutter);
  CHECK(hit.has_offset);
  CHECK_EQ(hit.offset, BufferOffsetFromLine(buffer, 3));

  Destroy(&f);
}

TEST(mouse_hit_panel_status_and_command_row) {
  Fixture f = MakeFixture("one\ntwo");

  Buffer *command = EditorBufferForView(&f.ed, f.ed.command_view);
  BufferSetText(&f.ed, command, Str8C("éabc"));
  f.ed.command_view->scroll_column = 1;

  MouseHit status = Hit(&f, 10.0f, (f32)(f.ed.focused_panel->rect.y1 - 1) + 0.2f);
  CHECK_EQ((u32)status.kind, (u32)MouseHitKind::StatusLine);
  CHECK(!status.boundary.valid);

  MouseHit inactive = Hit(&f, 10.0f, (f32)(f.ed.screen.y1 - 1) + 0.2f);
  CHECK_EQ((u32)inactive.kind, (u32)MouseHitKind::Outside);

  f.ed.command_line_active = true;
  MouseHit active = Hit(&f, 0.1f, (f32)(f.ed.screen.y1 - 1) + 0.2f);
  CHECK_EQ((u32)active.kind, (u32)MouseHitKind::CommandLine);
  CHECK(active.view == f.ed.command_view);
  CHECK(active.buffer == command);
  CHECK(active.has_offset);
  CHECK_EQ(active.offset, 2);

  Destroy(&f);
}

TEST(mouse_hit_vertical_boundary_precedence_threshold) {
  Fixture f = MakeFixture("left\nright");
  f.ed.line_number_mode = LineNumberMode::Off;
  Panel *right = EditorSplit(&f.ed, Axis2::X);
  EditorLayout(&f.ed);
  Panel *left = f.ed.root_panel->first_child;
  i32 boundary_x = left->rect.x1;

  MouseHit inside = Hit(&f, (f32)boundary_x + 0.124f, 5.5f);
  CHECK_EQ((u32)inside.kind, (u32)MouseHitKind::VerticalBoundary);
  CHECK(inside.boundary.valid);
  CHECK(inside.panel == right);

  MouseHit outside = Hit(&f, (f32)boundary_x + 0.126f, 5.5f);
  CHECK_EQ((u32)outside.kind, (u32)MouseHitKind::Text);

  MouseHit status_corner =
      Hit(&f, (f32)boundary_x - 0.124f, (f32)(left->rect.y1 - 1) + 0.2f);
  CHECK_EQ((u32)status_corner.kind, (u32)MouseHitKind::VerticalBoundary);

  Destroy(&f);
}

TEST(mouse_hit_status_line_boundary_only_with_panel_below) {
  Fixture f = MakeFixture("top\nmid\nbot");
  Panel *right = EditorSplit(&f.ed, Axis2::X);
  EditorSplit(&f.ed, Axis2::Y);
  EditorLayout(&f.ed);

  Panel *left = f.ed.root_panel->first_child;
  Panel *right_top = right->first_child;

  MouseHit left_status = Hit(&f, (f32)left->rect.x0 + 2.0f, (f32)(left->rect.y1 - 1) + 0.2f);
  CHECK_EQ((u32)left_status.kind, (u32)MouseHitKind::StatusLine);
  CHECK(!left_status.boundary.valid);

  MouseHit right_status =
      Hit(&f, (f32)right_top->rect.x0 + 2.0f, (f32)(right_top->rect.y1 - 1) + 0.2f);
  CHECK_EQ((u32)right_status.kind, (u32)MouseHitKind::StatusLine);
  CHECK(right_status.boundary.valid);
  CHECK_EQ((u32)right_status.boundary.axis, (u32)Axis2::Y);

  Destroy(&f);
}

TEST(mouse_hit_nested_vertical_boundary_targets_correct_leaf) {
  Fixture f = MakeFixture("one\ntwo");
  f.ed.line_number_mode = LineNumberMode::Off;
  Panel *right = EditorSplit(&f.ed, Axis2::X);
  Panel *far_right = EditorSplit(&f.ed, Axis2::X);
  EditorLayout(&f.ed);

  Panel *left = f.ed.root_panel->first_child;
  Panel *middle = right->first_child;

  MouseHit outer = Hit(&f, (f32)left->rect.x1 + 0.01f, 4.0f);
  CHECK_EQ((u32)outer.kind, (u32)MouseHitKind::VerticalBoundary);
  CHECK(outer.panel == middle);

  MouseHit inner = Hit(&f, (f32)middle->rect.x1 + 0.01f, 4.0f);
  CHECK_EQ((u32)inner.kind, (u32)MouseHitKind::VerticalBoundary);
  CHECK(inner.panel == far_right);

  Destroy(&f);
}

TEST(mouse_hit_image_content) {
  Fixture f = MakeFixture("summary");

  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  buffer->kind = BufferKind::Image;
  buffer->user_data = PushStruct(f.arena, ImageInfo);

  RectS32 text = EditorPanelTextRect(&f.ed, f.ed.focused_panel);
  MouseHit hit = Hit(&f, (f32)text.x0 + 1.0f, (f32)text.y0 + 1.0f);
  CHECK_EQ((u32)hit.kind, (u32)MouseHitKind::Image);
  CHECK(hit.panel == f.ed.focused_panel);
  CHECK(hit.view == EditorFocusedView(&f.ed));
  CHECK(hit.buffer == buffer);
  CHECK(!hit.has_offset);

  Destroy(&f);
}

TEST(mouse_hit_outside_coordinates) {
  Fixture f = MakeFixture("one");

  CHECK_EQ((u32)Hit(&f, -0.1f, 1.0f).kind, (u32)MouseHitKind::Outside);
  CHECK_EQ((u32)Hit(&f, 1.0f, -0.1f).kind, (u32)MouseHitKind::Outside);
  CHECK_EQ((u32)Hit(&f, 80.0f, 1.0f).kind, (u32)MouseHitKind::Outside);
  CHECK_EQ((u32)Hit(&f, 1.0f, 25.0f).kind, (u32)MouseHitKind::Outside);

  Destroy(&f);
}
