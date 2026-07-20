#include "editor/editor.h"

#include "buffers/buf_image.h"

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
  hit.boundary = InvalidBoundary();
  return hit;
}

[[nodiscard]] MouseHit OutsideHit() { return MakeHit(MouseHitKind::Outside, nullptr, nullptr, nullptr); }

[[nodiscard]] bool ScreenContains(const Editor *ed, const MouseEvent &event) {
  return ed && event.grid_x >= (f32)ed->screen.x0 && event.grid_x < (f32)ed->screen.x1 &&
         event.grid_y >= (f32)ed->screen.y0 && event.grid_y < (f32)ed->screen.y1;
}

[[nodiscard]] u64 ClampBufferLine(const Buffer *buffer, u64 line) {
  u64 count = BufferLineCount(buffer);
  return (count > 0) ? Min(line, count - 1) : 0;
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

void EditorProcessMouse(Editor *ed, const MouseEvent &event) {}
