#include "editor/editor.h"

#include "buffers/buf_image.h"
#include "vim/vim_motions.h"

#include <math.h>

namespace {

[[nodiscard]] PanelBoundary InvalidBoundary(Axis2 axis = Axis2::X) {
  return PanelBoundary{nullptr, nullptr, nullptr, axis, false};
}

[[nodiscard]] MouseHit MakeHit(MouseHitKind kind, Panel *panel, View *view, Buffer *buffer) {
  MouseHit hit = {};
  hit.kind = kind;
  hit.panel = panel;
  hit.view = view;
  hit.buffer = buffer;
  hit.buffer_handle = buffer ? buffer->handle : BufferHandleZero();
  hit.boundary = InvalidBoundary();
  return hit;
}

[[nodiscard]] MouseHit OutsideHit() { return MakeHit(MouseHitKind::Outside, nullptr, nullptr, nullptr); }

[[nodiscard]] MouseCapture MakeCapture(MouseCaptureKind kind, const MouseHit &hit, MouseButton button) {
  MouseCapture capture = {};
  capture.kind = kind;
  capture.panel = hit.panel;
  capture.view = hit.view;
  capture.buffer = hit.buffer;
  capture.buffer_handle = hit.buffer_handle;
  capture.boundary = hit.boundary;
  capture.button = button;
  return capture;
}

[[nodiscard]] bool ScreenContains(const Editor *ed, const MouseEvent &event) {
  return ed && event.grid_x >= (f32)ed->screen.x0 && event.grid_x < (f32)ed->screen.x1 &&
         event.grid_y >= (f32)ed->screen.y0 && event.grid_y < (f32)ed->screen.y1;
}

[[nodiscard]] u64 ClampBufferLine(const Buffer *buffer, u64 line) {
  u64 count = BufferLineCount(buffer);
  return (count > 0) ? Min(line, count - 1) : 0;
}

[[nodiscard]] RectS32 PanelContentRect(Editor *ed, Panel *panel) {
  RectS32 rect = panel->rect;
  rect.y1 = Max(rect.y1 - 1, rect.y0);
  return rect;
}

[[nodiscard]] RectS32 CommandLineRect(const Editor *ed) {
  return RectS32{ed->screen.x0, ed->screen.y1 - 1, ed->screen.x1, ed->screen.y1};
}

[[nodiscard]] MouseHit CommandLineHit(Editor *ed, const MouseEvent &event) {
  View *view = ed->command_view;
  Buffer *buffer = EditorBufferForView(ed, view);
  MouseHit hit = MakeHit(MouseHitKind::CommandLine, nullptr, view, buffer);
  if (!buffer) return hit;

  i32 column = Max(event.x - ed->screen.x0 - 1, 0);
  u64 scrolled = view ? view->scroll_column + (u64)column : (u64)column;
  hit.offset = BufferOffsetFromColumn(buffer, 0, scrolled);
  hit.has_offset = true;
  return hit;
}

[[nodiscard]] MouseHit VerticalBoundaryHit(Editor *ed, const MouseEvent &event) {
  if (!ed || !ed->root_panel) return OutsideHit();

  i32 boundary_x = (i32)floorf(event.grid_x + 0.5f);
  if (fabsf(event.grid_x - (f32)boundary_x) > 0.125f) return OutsideHit();

  PanelBoundary boundary = PanelBoundaryAt(ed->root_panel, boundary_x, event.y, Axis2::X);
  if (!boundary.valid) return OutsideHit();

  Panel *panel = PanelFromPoint(ed->root_panel, boundary_x, event.y);
  if (!panel || !panel->view) return OutsideHit();

  Buffer *buffer = EditorBufferForView(ed, panel->view);
  MouseHit hit = MakeHit(MouseHitKind::VerticalBoundary, panel, panel->view, buffer);
  hit.boundary = boundary;
  return hit;
}

[[nodiscard]] MouseHit PanelContentHit(Editor *ed, Panel *panel, const MouseEvent &event) {
  if (!ed || !panel || !panel->view) return OutsideHit();

  Buffer *buffer = EditorBufferForView(ed, panel->view);
  if (!buffer) return OutsideHit();

  RectS32 text = EditorPanelTextRect(ed, panel);
  u64 line = ClampBufferLine(buffer, panel->view->scroll_line + (u64)(event.y - text.y0));

  if (event.x < text.x0) {
    MouseHit hit = MakeHit(MouseHitKind::Gutter, panel, panel->view, buffer);
    hit.offset = BufferOffsetFromLine(buffer, line);
    hit.has_offset = true;
    return hit;
  }

  if (ImageBufferInfo(buffer)) return MakeHit(MouseHitKind::Image, panel, panel->view, buffer);

  MouseHit hit = MakeHit(MouseHitKind::Text, panel, panel->view, buffer);
  u64 column = panel->view->scroll_column + (u64)(event.x - text.x0);
  hit.offset = BufferOffsetFromColumn(buffer, line, column);
  hit.has_offset = true;
  return hit;
}

void ClearCapture(MouseState *mouse) {
  mouse->capture = MouseCapture{};
  mouse->pointer_anchor = MouseHit{};
  mouse->selection_anchor = MouseHit{};
  mouse->latest_hit = MouseHit{};
  mouse->selection_kind = MouseSelectionKind::None;
}

[[nodiscard]] bool PanelTreeContains(Panel *root, const Panel *target) {
  if (!root || !target) return false;
  if (root == target) return true;
  for (Panel *child = root->first_child; child; child = child->next) {
    if (PanelTreeContains(child, target)) return true;
  }
  return false;
}

template <typename Fn>
void ForEachLiveView(Panel *panel, Fn &&fn) {
  if (!panel) return;
  if (PanelIsLeaf(panel)) {
    if (panel->view) fn(panel->view);
    return;
  }
  for (Panel *child = panel->first_child; child; child = child->next) {
    ForEachLiveView(child, fn);
  }
}

void RestoreTemporaryVisual(View *view, Buffer *buffer) {
  if (!view || !buffer || !VimHasMouseVisualReturnMode(&view->vim)) return;
  view->vim.mode = VimConsumeVisualExitMode(&view->vim, VimMode::Normal);
  ViewSetCursor(view, buffer, view->cursor);
}

void RestoreAllTemporaryVisuals(Editor *ed) {
  ForEachLiveView(ed->root_panel, [&](View *view) {
    Buffer *buffer = EditorBufferForView(ed, view);
    RestoreTemporaryVisual(view, buffer);
  });
  Buffer *command = EditorBufferForView(ed, ed->command_view);
  RestoreTemporaryVisual(ed->command_view, command);
}

void ScrollViewToOwnCursor(Editor *ed, Panel *panel, View *view, Buffer *buffer) {
  if (!ed || !view || !buffer) return;
  if (panel) {
    ViewScrollToCursor(view, buffer, EditorPanelTextWidth(ed, panel), EditorPanelTextHeight(ed, panel));
  } else {
    i32 width = Max(ed->screen.x1 - ed->screen.x0 - 1, 1);
    ViewScrollToCursor(view, buffer, width, 1);
  }
}

[[nodiscard]] bool ResolveCapturedTarget(Editor *ed, const MouseCapture &capture, Panel **out_panel,
                                         View **out_view, Buffer **out_buffer) {
  *out_panel = nullptr;
  *out_view = nullptr;
  *out_buffer = nullptr;

  if (capture.kind == MouseCaptureKind::Panel) {
    if (!capture.panel || !capture.view || !PanelTreeContains(ed->root_panel, capture.panel) ||
        capture.panel->view != capture.view) {
      return false;
    }
    if (!BufferHandleEqual(capture.view->buffer, capture.buffer_handle)) return false;

    Buffer *buffer = BufferFromHandle(&ed->buffers, capture.buffer_handle);
    if (!buffer) return false;

    *out_panel = capture.panel;
    *out_view = capture.view;
    *out_buffer = buffer;
    return true;
  }

  if (capture.kind == MouseCaptureKind::CommandLine) {
    if (!capture.view || capture.view != ed->command_view) return false;
    if (!BufferHandleEqual(capture.view->buffer, capture.buffer_handle)) return false;

    Buffer *buffer = BufferFromHandle(&ed->buffers, capture.buffer_handle);
    if (!buffer) return false;

    *out_view = capture.view;
    *out_buffer = buffer;
    return true;
  }

  return false;
}

[[nodiscard]] i32 ClampCoord(i32 value, i32 min_v, i32 max_v) {
  if (max_v < min_v) return min_v;
  return Min(Max(value, min_v), max_v);
}

[[nodiscard]] MouseHit ResolvePanelDragHit(Editor *ed, Panel *panel, const MouseEvent &event,
                                           View *view, Buffer *buffer) {
  RectS32 text = EditorPanelTextRect(ed, panel);
  RectS32 content = PanelContentRect(ed, panel);

  if (event.y < text.y0) ViewScrollLines(view, buffer, -1, EditorPanelTextHeight(ed, panel));
  if (event.y >= text.y1) ViewScrollLines(view, buffer, 1, EditorPanelTextHeight(ed, panel));
  if (event.x < text.x0 && event.x < content.x0) ViewScrollColumns(view, buffer, -1, EditorPanelTextWidth(ed, panel));
  if (event.x >= text.x1) ViewScrollColumns(view, buffer, 1, EditorPanelTextWidth(ed, panel));

  MouseEvent clamped = event;
  clamped.x = ClampCoord(event.x, content.x0, content.x1 - 1);
  clamped.y = ClampCoord(event.y, text.y0, text.y1 - 1);
  clamped.grid_x = (f32)clamped.x;
  clamped.grid_y = (f32)clamped.y;
  return PanelContentHit(ed, panel, clamped);
}

[[nodiscard]] MouseHit ResolveCommandDragHit(Editor *ed, const MouseEvent &event, View *view,
                                             Buffer *buffer) {
  if (event.x < ed->screen.x0) {
    ViewScrollColumns(view, buffer, -1, Max(ed->screen.x1 - ed->screen.x0 - 1, 1));
  }
  if (event.x >= ed->screen.x1) {
    ViewScrollColumns(view, buffer, 1, Max(ed->screen.x1 - ed->screen.x0 - 1, 1));
  }

  MouseEvent clamped = event;
  RectS32 rect = CommandLineRect(ed);
  clamped.x = ClampCoord(event.x, rect.x0, rect.x1 - 1);
  clamped.y = rect.y0;
  clamped.grid_x = (f32)clamped.x;
  clamped.grid_y = (f32)clamped.y;
  return CommandLineHit(ed, clamped);
}

void MaybeStoreMouseReturnMode(View *view, VimMode source_mode) {
  if (!VimHasMouseVisualReturnMode(&view->vim) &&
      (source_mode == VimMode::Insert || source_mode == VimMode::Replace)) {
    VimSetMouseVisualReturnMode(&view->vim, source_mode);
  }
}

void CollapseSelection(View *view, Buffer *buffer) {
  view->vim.mode = VimConsumeVisualExitMode(&view->vim, VimMode::Normal);
  ViewSetCursor(view, buffer, view->cursor);
}

void ApplyCharacterSelection(View *view, Buffer *buffer, u64 anchor, u64 cursor, VimMode source_mode) {
  MaybeStoreMouseReturnMode(view, source_mode);
  view->vim.visual_anchor = anchor;
  view->vim.mode = VimMode::Visual;
  ViewSetCursor(view, buffer, cursor);
}

[[nodiscard]] RangeU64 WordRangeAt(const Buffer *buffer, u64 offset) {
  u64 size = BufferSize(buffer);
  if (size == 0) return RangeU64{0, 0};

  u64 pos = Min(offset, size - 1);
  TextObjectResult word = TextObjectWord(buffer, pos, 1, true, false);
  return word.valid ? word.range : RangeU64{pos, pos};
}

void ApplyWordSelection(View *view, Buffer *buffer, u64 anchor_seed, u64 target_seed,
                        VimMode source_mode) {
  MaybeStoreMouseReturnMode(view, source_mode);

  RangeU64 anchor = WordRangeAt(buffer, anchor_seed);
  RangeU64 target = WordRangeAt(buffer, target_seed);
  RangeU64 range = RangeU64{Min(anchor.min, target.min), Max(anchor.max, target.max)};

  view->vim.visual_anchor = range.min;
  view->vim.mode = VimMode::Visual;
  ViewSetCursor(view, buffer, (range.max > range.min) ? BufferPrevCodepoint(buffer, range.max) : range.min);
}

void ApplyLineSelection(View *view, Buffer *buffer, u64 anchor_offset, u64 target_offset,
                        VimMode source_mode) {
  MaybeStoreMouseReturnMode(view, source_mode);

  u64 anchor_line = BufferLineFromOffset(buffer, anchor_offset);
  u64 target_line = BufferLineFromOffset(buffer, target_offset);

  view->vim.visual_anchor = BufferOffsetFromLine(buffer, anchor_line);
  view->vim.mode = VimMode::VisualLine;
  ViewSetCursor(view, buffer, BufferOffsetFromLine(buffer, target_line));
}

[[nodiscard]] bool BracketSelectionRange(const Buffer *buffer, u64 offset, RangeU64 *out_range) {
  if (!buffer || offset >= BufferSize(buffer)) return false;

  u8 ch = BufferByteAt(buffer, offset);
  if (!(ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}')) return false;

  MotionResult motion = MotionMatchingBracket(buffer, nullptr, offset, 1, 0);
  if (!motion.valid) return false;

  u64 first = Min(offset, motion.target);
  u64 last = Max(offset, motion.target);
  out_range->min = first;
  out_range->max = Min(BufferNextCodepoint(buffer, last), BufferSize(buffer));
  return true;
}

void ApplyRangeSelection(View *view, Buffer *buffer, RangeU64 range, VimMode source_mode) {
  MaybeStoreMouseReturnMode(view, source_mode);
  view->vim.visual_anchor = range.min;
  view->vim.mode = VimMode::Visual;
  ViewSetCursor(view, buffer, (range.max > range.min) ? BufferPrevCodepoint(buffer, range.max) : range.min);
}

void UpdateSelectionFromHit(Editor *ed, View *view, Buffer *buffer, const MouseHit &hit) {
  VimMode source_mode = view->vim.mode;
  switch (ed->mouse.selection_kind) {
    case MouseSelectionKind::Character:
      ApplyCharacterSelection(view, buffer, ed->mouse.selection_anchor.offset, hit.offset, source_mode);
      break;
    case MouseSelectionKind::Word:
      ApplyWordSelection(view, buffer, ed->mouse.selection_anchor.offset, hit.offset, source_mode);
      break;
    case MouseSelectionKind::Line:
      ApplyLineSelection(view, buffer, ed->mouse.selection_anchor.offset, hit.offset, source_mode);
      break;
    default:
      break;
  }
}

void BeginPanelCapture(Editor *ed, const MouseHit &hit, MouseButton button, MouseSelectionKind kind) {
  ed->mouse.capture = MakeCapture(MouseCaptureKind::Panel, hit, button);
  ed->mouse.pointer_anchor = hit;
  ed->mouse.selection_anchor = hit;
  ed->mouse.latest_hit = hit;
  ed->mouse.selection_kind = kind;
}

void BeginCommandCapture(Editor *ed, const MouseHit &hit) {
  ed->mouse.capture = MakeCapture(MouseCaptureKind::CommandLine, hit, MouseButton::Left);
  ed->mouse.pointer_anchor = hit;
  ed->mouse.selection_anchor = hit;
  ed->mouse.latest_hit = hit;
  ed->mouse.selection_kind = MouseSelectionKind::Character;
}

void HandleLeftPanelPress(Editor *ed, MouseHit hit, const MouseEvent &event) {
  EditorFocusPanel(ed, hit.panel);
  View *view = hit.view;
  Buffer *buffer = hit.buffer;
  if (!view || !buffer) return;

  if (hit.kind == MouseHitKind::Image || hit.kind == MouseHitKind::StatusLine) {
    ClearCapture(&ed->mouse);
    return;
  }
  if (hit.kind != MouseHitKind::Text && hit.kind != MouseHitKind::Gutter) {
    ClearCapture(&ed->mouse);
    return;
  }

  if (VimModeIsVisual(view->vim.mode)) CollapseSelection(view, buffer);

  u64 cursor = ViewClampCursorToMode(view, buffer, hit.offset);
  ViewSetCursor(view, buffer, cursor);
  ScrollViewToOwnCursor(ed, hit.panel, view, buffer);

  MouseSelectionKind kind =
      (event.click_count >= 3) ? MouseSelectionKind::Line
                               : (event.click_count == 2 ? MouseSelectionKind::Word
                                                         : MouseSelectionKind::Character);
  BeginPanelCapture(ed, hit, MouseButton::Left, kind);

  if (kind == MouseSelectionKind::Character) return;

  VimMode source_mode = view->vim.mode;
  if (kind == MouseSelectionKind::Word) {
    RangeU64 range = {};
    if (BracketSelectionRange(buffer, hit.offset, &range)) {
      ApplyRangeSelection(view, buffer, range, source_mode);
    } else {
      ApplyWordSelection(view, buffer, hit.offset, hit.offset, source_mode);
    }
  } else {
    ApplyLineSelection(view, buffer, hit.offset, hit.offset, source_mode);
  }
  ScrollViewToOwnCursor(ed, hit.panel, view, buffer);
}

void HandleRightPanelPress(Editor *ed, MouseHit hit) {
  EditorFocusPanel(ed, hit.panel);
  View *view = hit.view;
  Buffer *buffer = hit.buffer;
  if (!view || !buffer) return;

  if (hit.kind == MouseHitKind::Image || hit.kind == MouseHitKind::StatusLine) {
    ClearCapture(&ed->mouse);
    return;
  }
  if (hit.kind != MouseHitKind::Text && hit.kind != MouseHitKind::Gutter) {
    ClearCapture(&ed->mouse);
    return;
  }

  BeginPanelCapture(ed, hit, MouseButton::Right, MouseSelectionKind::Character);

  if (VimModeIsVisual(view->vim.mode)) {
    u64 anchor = view->vim.visual_anchor;
    u64 cursor = view->cursor;
    u64 dist_anchor = (anchor > hit.offset) ? anchor - hit.offset : hit.offset - anchor;
    u64 dist_cursor = (cursor > hit.offset) ? cursor - hit.offset : hit.offset - cursor;

    if (view->vim.mode == VimMode::VisualLine) ed->mouse.selection_kind = MouseSelectionKind::Line;
    if (dist_anchor < dist_cursor) {
      ed->mouse.selection_anchor.offset = cursor;
    } else {
      ed->mouse.selection_anchor.offset = anchor;
    }
    ed->mouse.selection_anchor.has_offset = true;
    UpdateSelectionFromHit(ed, view, buffer, hit);
  } else {
    ed->mouse.selection_anchor.offset = view->cursor;
    ed->mouse.selection_anchor.has_offset = true;
    ApplyCharacterSelection(view, buffer, view->cursor, hit.offset, view->vim.mode);
  }
  ScrollViewToOwnCursor(ed, hit.panel, view, buffer);
}

void HandleCommandLinePress(Editor *ed, MouseHit hit) {
  View *view = hit.view;
  Buffer *buffer = hit.buffer;
  if (!view || !buffer || hit.kind != MouseHitKind::CommandLine) {
    ClearCapture(&ed->mouse);
    return;
  }

  if (VimModeIsVisual(view->vim.mode)) CollapseSelection(view, buffer);

  ViewSetCursor(view, buffer, hit.offset);
  ScrollViewToOwnCursor(ed, nullptr, view, buffer);
  BeginCommandCapture(ed, hit);
}

}  // namespace

MouseHit EditorMouseHitTest(Editor *ed, const MouseEvent &event) {
  if (!ScreenContains(ed, event)) return OutsideHit();

  i32 command_row = ed->screen.y1 - 1;
  if (event.y == command_row) {
    if (ed->command_line_active) return CommandLineHit(ed, event);
    return OutsideHit();
  }

  MouseHit boundary_hit = VerticalBoundaryHit(ed, event);
  if (boundary_hit.kind == MouseHitKind::VerticalBoundary) return boundary_hit;

  Panel *panel = PanelFromPoint(ed->root_panel, event.x, event.y);
  if (!panel || !panel->view) return OutsideHit();

  Buffer *buffer = EditorBufferForView(ed, panel->view);
  if (!buffer) return OutsideHit();

  if (event.y == panel->rect.y1 - 1) {
    MouseHit hit = MakeHit(MouseHitKind::StatusLine, panel, panel->view, buffer);
    hit.boundary = PanelBoundaryAt(ed->root_panel, event.x, panel->rect.y1, Axis2::Y);
    return hit;
  }

  RectS32 text = EditorPanelTextRect(ed, panel);
  if (!RectContains(text, event.x, event.y) && event.x < text.x0) {
    if (event.y >= text.y0 && event.y < text.y1) return PanelContentHit(ed, panel, event);
    return OutsideHit();
  }
  if (RectContains(text, event.x, event.y)) return PanelContentHit(ed, panel, event);

  return OutsideHit();
}

void EditorProcessMouse(Editor *ed, const MouseEvent &event) {
  if (!ed) return;

  MouseState *mouse = &ed->mouse;

  if (event.action == MouseAction::Cancel) {
    ClearCapture(mouse);
    RestoreAllTemporaryVisuals(ed);
    return;
  }

  if (event.action == MouseAction::Release) {
    if (mouse->capture.kind != MouseCaptureKind::None && mouse->capture.button == event.button) {
      ClearCapture(mouse);
    }
    return;
  }

  if (event.action == MouseAction::Drag) {
    if (mouse->capture.kind == MouseCaptureKind::None || mouse->capture.button != event.button) return;

    Panel *panel = nullptr;
    View *view = nullptr;
    Buffer *buffer = nullptr;
    if (!ResolveCapturedTarget(ed, mouse->capture, &panel, &view, &buffer)) {
      ClearCapture(mouse);
      return;
    }

    MouseHit hit = (mouse->capture.kind == MouseCaptureKind::Panel)
                       ? ResolvePanelDragHit(ed, panel, event, view, buffer)
                       : ResolveCommandDragHit(ed, event, view, buffer);
    if (!hit.has_offset) return;

    mouse->latest_hit = hit;
    if (mouse->selection_kind == MouseSelectionKind::Character &&
        !VimModeIsVisual(view->vim.mode) &&
        hit.offset == mouse->pointer_anchor.offset) {
      return;
    }

    UpdateSelectionFromHit(ed, view, buffer, hit);
    return;
  }

  if (event.action != MouseAction::Press) return;

  EditorCancelPendingInput(ed);
  MouseHit hit = EditorMouseHitTest(ed, event);
  mouse->latest_hit = hit;

  if (hit.kind == MouseHitKind::CommandLine) {
    if (event.button == MouseButton::Left) HandleCommandLinePress(ed, hit);
    return;
  }

  if (hit.kind == MouseHitKind::Outside || hit.kind == MouseHitKind::VerticalBoundary) {
    ClearCapture(mouse);
    return;
  }

  if (event.button == MouseButton::Left) {
    HandleLeftPanelPress(ed, hit, event);
  } else if (event.button == MouseButton::Right) {
    HandleRightPanelPress(ed, hit);
  } else if (hit.panel) {
    EditorFocusPanel(ed, hit.panel);
    ClearCapture(mouse);
  }
}
