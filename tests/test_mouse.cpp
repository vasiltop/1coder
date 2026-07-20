#include "buffers/buf_image.h"
#include "editor/command.h"
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

MouseEvent MouseEventAt(MouseAction action, MouseButton button, f32 grid_x, f32 grid_y,
                        u8 click_count = 1) {
  MouseEvent event = MouseAt(grid_x, grid_y);
  event.action = action;
  event.button = button;
  event.click_count = click_count;
  return event;
}

void SendMouse(Fixture *f, MouseAction action, MouseButton button, f32 grid_x, f32 grid_y,
               u8 click_count = 1) {
  EditorProcessMouse(&f->ed, MouseEventAt(action, button, grid_x, grid_y, click_count));
}

MouseHit Hit(Fixture *f, f32 grid_x, f32 grid_y) {
  return EditorMouseHitTest(&f->ed, MouseAt(grid_x, grid_y));
}

RangeU64 Selection(View *view, Buffer *buffer) { return ViewSelection(view, buffer); }

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

TEST(mouse_click_focuses_target_and_handles_gutter_image_and_status) {
  Fixture f = MakeFixture("zero\none\ntwo");

  Panel *right = EditorSplit(&f.ed, Axis2::X);
  EditorLayout(&f.ed);
  Panel *left = f.ed.root_panel->first_child;
  Buffer *left_buffer = EditorBufferForView(&f.ed, left->view);
  Buffer *right_buffer = EditorBufferForView(&f.ed, right->view);

  RectS32 right_text = EditorPanelTextRect(&f.ed, right);
  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)right_text.x0 + 2.2f,
            (f32)right_text.y0 + 0.2f);
  CHECK(f.ed.focused_panel == right);
  CHECK_EQ(right->view->cursor, BufferOffsetFromColumn(right_buffer, 0, 2));

  RectS32 left_text = EditorPanelTextRect(&f.ed, left);
  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)left->rect.x0 + 0.2f,
            (f32)left_text.y0 + 1.2f);
  CHECK(f.ed.focused_panel == left);
  CHECK_EQ(left->view->cursor, BufferOffsetFromLine(left_buffer, 1));

  ViewSetCursor(right->view, right_buffer, BufferOffsetFromColumn(right_buffer, 0, 1));
  right_buffer->kind = BufferKind::Image;
  right_buffer->user_data = PushStruct(f.arena, ImageInfo);
  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)right_text.x0 + 1.1f,
            (f32)right_text.y0 + 1.1f);
  CHECK(f.ed.focused_panel == right);
  CHECK_EQ(right->view->cursor, BufferOffsetFromColumn(right_buffer, 0, 1));

  u64 before_status_cursor = left->view->cursor;
  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)left->rect.x0 + 4.0f,
            (f32)(left->rect.y1 - 1) + 0.2f);
  CHECK(f.ed.focused_panel == left);
  CHECK_EQ(left->view->cursor, before_status_cursor);

  Destroy(&f);
}

TEST(mouse_click_cancels_pending_input_without_touching_macros_or_registers) {
  Fixture f = MakeFixture("abcdef");

  View *view = EditorFocusedView(&f.ed);
  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  RectS32 text = EditorPanelTextRect(&f.ed, f.ed.focused_panel);

  KeyChord prefix = KeyChordKey(Key::W, KeyMod::Ctrl);
  EditorProcessChord(&f.ed, prefix);
  view->vim.count = 23;
  view->vim.has_count = true;
  view->vim.pending_operator = OperatorKind::Delete;
  view->vim.operator_count = 7;
  view->vim.has_operator_count = true;
  view->vim.pending_register = 'a';
  f.ed.input.awaiting_char_command = CommandId::find_char_forward;
  f.ed.input.awaiting_register = true;
  f.ed.input.register_follow_up = CommandId::macro_replay;
  f.ed.input.awaiting_text_object = true;
  f.ed.input.text_object_inner = true;
  f.ed.input.awaiting_confirm = true;
  f.ed.input.confirm_command = CommandId::quit;
  f.ed.input.confirm_buffer = buffer->handle;
  f.ed.input.recording_macro = true;
  f.ed.input.replaying_macro = true;
  EditorSetRegister(&f.ed, 'a', Str8Lit("keep"), false);

  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 2.2f,
            (f32)text.y0 + 0.2f);

  CHECK(f.ed.input.pending == nullptr);
  CHECK(f.ed.input.pending_map == nullptr);
  CHECK_EQ(f.ed.input.pending_chord_count, 0);
  CHECK_EQ((u32)f.ed.input.awaiting_char_command, (u32)CommandId::None);
  CHECK(!f.ed.input.awaiting_register);
  CHECK_EQ((u32)f.ed.input.register_follow_up, (u32)CommandId::None);
  CHECK(!f.ed.input.awaiting_text_object);
  CHECK(!f.ed.input.awaiting_confirm);
  CHECK_EQ(view->vim.count, 0);
  CHECK(!view->vim.has_count);
  CHECK_EQ((u32)view->vim.pending_operator, (u32)OperatorKind::None);
  CHECK_EQ(view->vim.pending_register, 0);
  CHECK(f.ed.input.recording_macro);
  CHECK(f.ed.input.replaying_macro);
  CHECK_STR(EditorGetRegister(&f.ed, 'a').text, Str8Lit("keep"));

  Destroy(&f);
}

TEST(mouse_click_collapses_visual_and_temporary_visual_to_the_right_mode) {
  Fixture normal = MakeFixture("abcdef");
  RectS32 text = EditorPanelTextRect(&normal.ed, normal.ed.focused_panel);
  View *view = EditorFocusedView(&normal.ed);
  Buffer *buffer = EditorFocusedBuffer(&normal.ed);

  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 0;
  ViewSetCursor(view, buffer, 3);
  SendMouse(&normal, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 4.2f,
            (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Normal);
  CHECK_EQ(view->cursor, BufferOffsetFromColumn(buffer, 0, 4));

  Destroy(&normal);

  Fixture insert = MakeFixture("abcdef");
  text = EditorPanelTextRect(&insert.ed, insert.ed.focused_panel);
  view = EditorFocusedView(&insert.ed);
  buffer = EditorFocusedBuffer(&insert.ed);
  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 1;
  view->vim.mouse_visual_return_mode = VimMode::Insert;
  ViewSetCursor(view, buffer, 4);

  SendMouse(&insert, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 2.2f,
            (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Insert);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_EQ(view->cursor, BufferOffsetFromColumn(buffer, 0, 2));

  Destroy(&insert);
}

TEST(mouse_click_double_and_triple_select_words_brackets_and_lines) {
  Fixture bracket = MakeFixture("(alpha beta)\nzero");
  RectS32 text = EditorPanelTextRect(&bracket.ed, bracket.ed.focused_panel);
  View *view = EditorFocusedView(&bracket.ed);
  Buffer *buffer = EditorFocusedBuffer(&bracket.ed);

  SendMouse(&bracket, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 0.1f,
            (f32)text.y0 + 0.2f, 2);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);
  CHECK_EQ(Selection(view, buffer).min, 0);
  CHECK_EQ(Selection(view, buffer).max, 12);
  SendMouse(&bracket, MouseAction::Release, MouseButton::Left, (f32)text.x0 + 0.1f,
            (f32)text.y0 + 0.2f, 2);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);
  Destroy(&bracket);

  Fixture word = MakeFixture("zero beta gamma");
  text = EditorPanelTextRect(&word.ed, word.ed.focused_panel);
  view = EditorFocusedView(&word.ed);
  buffer = EditorFocusedBuffer(&word.ed);
  SendMouse(&word, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 6.2f,
            (f32)text.y0 + 0.2f, 2);
  CHECK_EQ(Selection(view, buffer).min, 5);
  CHECK_EQ(Selection(view, buffer).max, 9);
  Destroy(&word);

  Fixture line = MakeFixture("one\ntwo\nthree");
  text = EditorPanelTextRect(&line.ed, line.ed.focused_panel);
  view = EditorFocusedView(&line.ed);
  buffer = EditorFocusedBuffer(&line.ed);
  SendMouse(&line, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 1.2f,
            (f32)text.y0 + 1.2f, 3);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::VisualLine);
  CHECK_EQ(Selection(view, buffer).min, BufferOffsetFromLine(buffer, 1));
  CHECK_EQ(Selection(view, buffer).max, LineRangeWithNewline(&buffer->lines, &buffer->text, 1).max);
  Destroy(&line);
}

TEST(mouse_click_insert_and_replace_double_and_triple_persist_after_release) {
  Fixture insert = MakeFixture("alpha beta");
  RectS32 text = EditorPanelTextRect(&insert.ed, insert.ed.focused_panel);
  View *view = EditorFocusedView(&insert.ed);
  Buffer *buffer = EditorFocusedBuffer(&insert.ed);
  view->vim.mode = VimMode::Insert;

  SendMouse(&insert, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 1.2f,
            (f32)text.y0 + 0.2f, 2);
  SendMouse(&insert, MouseAction::Release, MouseButton::Left, (f32)text.x0 + 1.2f,
            (f32)text.y0 + 0.2f, 2);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Insert);
  CHECK_EQ(Selection(view, buffer).min, 0);
  CHECK_EQ(Selection(view, buffer).max, 5);
  Destroy(&insert);

  Fixture replace = MakeFixture("one\ntwo\nthree");
  text = EditorPanelTextRect(&replace.ed, replace.ed.focused_panel);
  view = EditorFocusedView(&replace.ed);
  view->vim.mode = VimMode::Replace;
  SendMouse(&replace, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 1.2f,
            (f32)text.y0 + 1.2f, 3);
  SendMouse(&replace, MouseAction::Release, MouseButton::Left, (f32)text.x0 + 1.2f,
            (f32)text.y0 + 1.2f, 3);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::VisualLine);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Replace);
  Destroy(&replace);
}

TEST(mouse_drag_left_waits_for_threshold_and_keeps_selection_after_release) {
  Fixture f = MakeFixture("abcdef");

  View *view = EditorFocusedView(&f.ed);
  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  RectS32 text = EditorPanelTextRect(&f.ed, f.ed.focused_panel);

  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 1.2f, (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Normal);

  SendMouse(&f, MouseAction::Drag, MouseButton::Left, (f32)text.x0 + 1.8f, (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Normal);

  SendMouse(&f, MouseAction::Drag, MouseButton::Left, (f32)text.x0 + 4.2f, (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);
  CHECK_EQ(view->vim.visual_anchor, BufferOffsetFromColumn(buffer, 0, 1));
  CHECK_EQ(Selection(view, buffer).min, 1);
  CHECK_EQ(Selection(view, buffer).max, 5);

  SendMouse(&f, MouseAction::Release, MouseButton::Left, (f32)text.x0 + 4.2f,
            (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)f.ed.mouse.capture.kind, (u32)MouseCaptureKind::None);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);

  Destroy(&f);
}

TEST(mouse_drag_left_keeps_press_anchor_when_dragging_backwards) {
  Fixture f = MakeFixture("abcdef");

  View *view = EditorFocusedView(&f.ed);
  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  RectS32 text = EditorPanelTextRect(&f.ed, f.ed.focused_panel);

  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 4.2f, (f32)text.y0 + 0.2f);
  SendMouse(&f, MouseAction::Drag, MouseButton::Left, (f32)text.x0 + 1.2f, (f32)text.y0 + 0.2f);

  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);
  CHECK_EQ(view->vim.visual_anchor, BufferOffsetFromColumn(buffer, 0, 4));
  CHECK_EQ(Selection(view, buffer).min, 1);
  CHECK_EQ(Selection(view, buffer).max, 5);

  Destroy(&f);
}

TEST(mouse_drag_cancel_restores_temporary_visual_and_global_cancel_restores_all_views) {
  Fixture single = MakeFixture("abcdef");
  View *view = EditorFocusedView(&single.ed);
  RectS32 text = EditorPanelTextRect(&single.ed, single.ed.focused_panel);
  view->vim.mode = VimMode::Insert;

  SendMouse(&single, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 0.2f,
            (f32)text.y0 + 0.2f);
  SendMouse(&single, MouseAction::Drag, MouseButton::Left, (f32)text.x0 + 3.2f,
            (f32)text.y0 + 0.2f);
  SendMouse(&single, MouseAction::Release, MouseButton::Left, (f32)text.x0 + 3.2f,
            (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);
  SendMouse(&single, MouseAction::Cancel, MouseButton::None, 0.0f, 0.0f);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Insert);
  CHECK_EQ((u32)view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  Destroy(&single);

  Fixture multi = MakeFixture("left\nright");
  Panel *right = EditorSplit(&multi.ed, Axis2::X);
  EditorLayout(&multi.ed);
  Panel *left = multi.ed.root_panel->first_child;
  RectS32 left_text = EditorPanelTextRect(&multi.ed, left);
  RectS32 right_text = EditorPanelTextRect(&multi.ed, right);
  left->view->vim.mode = VimMode::Insert;
  right->view->vim.mode = VimMode::Replace;

  EditorFocusPanel(&multi.ed, left);
  SendMouse(&multi, MouseAction::Press, MouseButton::Left, (f32)left_text.x0 + 0.2f,
            (f32)left_text.y0 + 0.2f);
  SendMouse(&multi, MouseAction::Drag, MouseButton::Left, (f32)left_text.x0 + 2.2f,
            (f32)left_text.y0 + 0.2f);
  SendMouse(&multi, MouseAction::Release, MouseButton::Left, (f32)left_text.x0 + 2.2f,
            (f32)left_text.y0 + 0.2f);

  EditorFocusPanel(&multi.ed, right);
  SendMouse(&multi, MouseAction::Press, MouseButton::Left, (f32)right_text.x0 + 0.2f,
            (f32)right_text.y0 + 0.2f);
  SendMouse(&multi, MouseAction::Drag, MouseButton::Left, (f32)right_text.x0 + 2.2f,
            (f32)right_text.y0 + 0.2f);
  SendMouse(&multi, MouseAction::Release, MouseButton::Left, (f32)right_text.x0 + 2.2f,
            (f32)right_text.y0 + 0.2f);

  CHECK_EQ((u32)left->view->vim.mode, (u32)VimMode::Visual);
  CHECK_EQ((u32)right->view->vim.mode, (u32)VimMode::Visual);
  SendMouse(&multi, MouseAction::Cancel, MouseButton::None, 0.0f, 0.0f);
  CHECK_EQ((u32)left->view->vim.mode, (u32)VimMode::Insert);
  CHECK_EQ((u32)right->view->vim.mode, (u32)VimMode::Replace);
  CHECK_EQ((u32)left->view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);
  CHECK_EQ((u32)right->view->vim.mouse_visual_return_mode, (u32)VimMode::Normal);

  Destroy(&multi);
}

TEST(mouse_drag_capture_stays_with_the_owning_view_across_focus_changes) {
  Fixture f = MakeFixture("abcdef");
  Panel *right = EditorSplit(&f.ed, Axis2::X);
  EditorLayout(&f.ed);
  Panel *left = f.ed.root_panel->first_child;
  RectS32 left_text = EditorPanelTextRect(&f.ed, left);
  Buffer *left_buffer = EditorBufferForView(&f.ed, left->view);

  EditorFocusPanel(&f.ed, left);
  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)left_text.x0 + 0.2f,
            (f32)left_text.y0 + 0.2f);
  EditorFocusPanel(&f.ed, right);
  SendMouse(&f, MouseAction::Drag, MouseButton::Left, (f32)left_text.x0 + 3.2f,
            (f32)left_text.y0 + 0.2f);

  CHECK(f.ed.focused_panel == right);
  CHECK_EQ((u32)left->view->vim.mode, (u32)VimMode::Visual);
  CHECK_EQ(Selection(left->view, left_buffer).min, 0);
  CHECK_EQ(Selection(left->view, left_buffer).max, 4);

  Destroy(&f);
}

TEST(mouse_click_right_selects_from_cursor_and_moves_the_nearest_endpoint) {
  Fixture fresh = MakeFixture("abcdef");
  View *view = EditorFocusedView(&fresh.ed);
  Buffer *buffer = EditorFocusedBuffer(&fresh.ed);
  RectS32 text = EditorPanelTextRect(&fresh.ed, fresh.ed.focused_panel);

  SendMouse(&fresh, MouseAction::Press, MouseButton::Right, (f32)text.x0 + 3.2f,
            (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);
  CHECK_EQ(view->vim.visual_anchor, 0);
  CHECK_EQ(Selection(view, buffer).min, 0);
  CHECK_EQ(Selection(view, buffer).max, 4);
  Destroy(&fresh);

  Fixture extend = MakeFixture("abcdef");
  view = EditorFocusedView(&extend.ed);
  buffer = EditorFocusedBuffer(&extend.ed);
  text = EditorPanelTextRect(&extend.ed, extend.ed.focused_panel);
  view->vim.mode = VimMode::Visual;
  view->vim.visual_anchor = 0;
  ViewSetCursor(view, buffer, 4);

  SendMouse(&extend, MouseAction::Press, MouseButton::Right, (f32)text.x0 + 2.2f,
            (f32)text.y0 + 0.2f);
  CHECK_EQ(view->vim.visual_anchor, 0);
  CHECK_EQ(view->cursor, 2);
  CHECK_EQ(Selection(view, buffer).min, 0);
  CHECK_EQ(Selection(view, buffer).max, 3);

  Destroy(&extend);
}

TEST(mouse_drag_cancels_safely_if_the_captured_view_changes_buffer) {
  Fixture f = MakeFixture("abcdef");
  RectS32 text = EditorPanelTextRect(&f.ed, f.ed.focused_panel);
  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 1.2f, (f32)text.y0 + 0.2f);

  BufferHandle other = BufferOpen(&f.ed.buffers, BufferKind::Scratch, Str8Lit("other"));
  EditorFocusedView(&f.ed)->buffer = other;

  SendMouse(&f, MouseAction::Drag, MouseButton::Left, (f32)text.x0 + 4.2f, (f32)text.y0 + 0.2f);
  CHECK_EQ((u32)f.ed.mouse.capture.kind, (u32)MouseCaptureKind::None);
  CHECK_EQ((u32)EditorFocusedView(&f.ed)->vim.mode, (u32)VimMode::Normal);

  Destroy(&f);
}

TEST(mouse_drag_clamps_and_autoscrolls_without_breaking_utf8_offsets) {
  Fixture f = MakeFixture("ééééé");
  f.ed.line_number_mode = LineNumberMode::Off;
  EditorSetScreen(&f.ed, RectS32{0, 0, 4, 5});

  View *view = EditorFocusedView(&f.ed);
  Buffer *buffer = EditorFocusedBuffer(&f.ed);
  RectS32 text = EditorPanelTextRect(&f.ed, f.ed.focused_panel);

  SendMouse(&f, MouseAction::Press, MouseButton::Left, (f32)text.x0 + 0.1f, (f32)text.y0 + 0.2f);
  SendMouse(&f, MouseAction::Drag, MouseButton::Left, (f32)text.x1 + 5.0f, (f32)text.y0 + 0.2f);

  CHECK_EQ(view->scroll_column, 1);
  CHECK_EQ(view->cursor, BufferOffsetFromColumn(buffer, 0, 4));
  CHECK_EQ((u32)view->vim.mode, (u32)VimMode::Visual);

  Destroy(&f);
}

TEST(mouse_click_and_drag_command_line_uses_the_command_view) {
  Fixture f = MakeFixture("buffer");
  Panel *focused_before = f.ed.focused_panel;
  CommandExec(&f.ed, CommandId::command_line_open);

  Buffer *command = EditorBufferForView(&f.ed, f.ed.command_view);
  BufferSetText(&f.ed, command, Str8C("abcdef"));
  f.ed.command_view->vim.mode = VimMode::Insert;

  f32 row = (f32)(f.ed.screen.y1 - 1) + 0.2f;
  SendMouse(&f, MouseAction::Press, MouseButton::Left, 4.2f, row);
  CHECK(f.ed.focused_panel == focused_before);
  CHECK(f.ed.command_view == EditorInputView(&f.ed));
  CHECK_EQ(f.ed.command_view->cursor, 3);

  SendMouse(&f, MouseAction::Drag, MouseButton::Left, 4.8f, row);
  CHECK_EQ((u32)f.ed.command_view->vim.mode, (u32)VimMode::Insert);
  SendMouse(&f, MouseAction::Drag, MouseButton::Left, 7.2f, row);
  CHECK_EQ((u32)f.ed.command_view->vim.mode, (u32)VimMode::Visual);
  CHECK_EQ(Selection(f.ed.command_view, command).min, 3);
  CHECK_EQ(Selection(f.ed.command_view, command).max, 6);

  Destroy(&f);
}
