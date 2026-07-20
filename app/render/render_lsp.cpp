#include "render/render_lsp.h"

#include "render/render_editor.h"

namespace {

constexpr f32 kHairline = 1.0f;
constexpr u64 kMaxCompletionRows = 12;
constexpr i32 kPopupPaddingCells = 1;
constexpr i32 kPopupTextMaxWidthCells = 72;
constexpr i32 kPopupDetailMaxCells = 24;
constexpr i32 kCompletionGapCells = 2;

[[nodiscard]] RectF32 TextRect(RectS32 text_cells, RectF32 content_rect, const RenderContext *ctx) {
  return RectF32{
      (f32)text_cells.x0 * ctx->cell_width,
      content_rect.y0,
      content_rect.x1,
      content_rect.y1,
  };
}

[[nodiscard]] f32 ColumnX(const RenderContext *ctx, RectF32 text_rect, u64 column,
                          u64 scroll_column) {
  u64 relative = (column > scroll_column) ? column - scroll_column : 0;
  return text_rect.x0 + (f32)relative * ctx->cell_width;
}

[[nodiscard]] u64 Utf8BytesForCells(String8 text, u64 cell_count) {
  u64 used = 0;
  u64 cells = 0;
  while (used < text.size && cells < cell_count) {
    DecodedCodepoint decoded = Utf8Decode(text, used);
    used += Max(decoded.advance, (u32)1);
    cells += 1;
  }
  return used;
}

[[nodiscard]] String8 TruncateToCells(Arena *arena, String8 text, i32 max_cells) {
  if (max_cells <= 0) return {};
  u64 cells = Utf8Length(text);
  if ((i32)cells <= max_cells) return text;
  if (max_cells <= 3) return PushStr8Copy(arena, Str8Prefix(text, Utf8BytesForCells(text, max_cells)));
  String8 prefix = Str8Prefix(text, Utf8BytesForCells(text, (u64)(max_cells - 3)));
  return PushStr8Cat(arena, prefix, Str8Lit("..."));
}

[[nodiscard]] Vec4F32 DiagnosticColor(const Theme *theme, LspDiagnosticSeverity severity) {
  switch (severity) {
    case LspDiagnosticSeverity::Error: return theme->diagnostic_error;
    case LspDiagnosticSeverity::Warning: return theme->diagnostic_warning;
    case LspDiagnosticSeverity::Information: return theme->diagnostic_information;
    case LspDiagnosticSeverity::Hint: return theme->diagnostic_hint;
  }
  return theme->diagnostic_hint;
}

[[nodiscard]] u8 DiagnosticMarker(LspDiagnosticSeverity severity) {
  switch (severity) {
    case LspDiagnosticSeverity::Error: return 'E';
    case LspDiagnosticSeverity::Warning: return 'W';
    case LspDiagnosticSeverity::Information: return 'I';
    case LspDiagnosticSeverity::Hint: return 'H';
  }
  return 0;
}

[[nodiscard]] u64 LineEndColumn(const Buffer *buffer, u64 line) {
  return BufferColumnFromOffset(buffer, BufferLineEnd(buffer, line));
}

void DrawPopupFrame(RenderContext *ctx, RectF32 rect) {
  DrawRect(ctx->draw, rect, ctx->theme.popup_background);
  DrawRect(ctx->draw, RectF32{rect.x0, rect.y0, rect.x1, rect.y0 + kHairline},
           ctx->theme.popup_border);
  DrawRect(ctx->draw, RectF32{rect.x0, rect.y1 - kHairline, rect.x1, rect.y1},
           ctx->theme.popup_border);
  DrawRect(ctx->draw, RectF32{rect.x0, rect.y0, rect.x0 + kHairline, rect.y1},
           ctx->theme.popup_border);
  DrawRect(ctx->draw, RectF32{rect.x1 - kHairline, rect.y0, rect.x1, rect.y1},
           ctx->theme.popup_border);
}

[[nodiscard]] RectF32 PlacePopup(RectF32 content_rect, f32 anchor_x, f32 anchor_row_top,
                                 f32 popup_width, f32 popup_height, f32 cell_height) {
  f32 x0 = anchor_x;
  if (x0 + popup_width > content_rect.x1) x0 = content_rect.x1 - popup_width;
  x0 = Max(content_rect.x0, x0);

  f32 y0 = anchor_row_top + cell_height;
  if (y0 + popup_height > content_rect.y1) y0 = anchor_row_top - popup_height;
  if (y0 < content_rect.y0) y0 = content_rect.y0;
  if (y0 + popup_height > content_rect.y1) y0 = content_rect.y1 - popup_height;
  y0 = Max(content_rect.y0, y0);

  return RectF32{x0, y0, x0 + popup_width, y0 + popup_height};
}

}  // namespace

void RenderLspDiagnostics(RenderContext *ctx, EditorLspUi *ui, Buffer *buffer, const View *view,
                          LspPositionEncoding position_encoding, RectF32 content_rect,
                          RectS32 text_cells, RangeU64 visible_lines, i32 gutter_columns) {
  if (ctx == nullptr || ui == nullptr || buffer == nullptr || view == nullptr) return;

  i32 columns = RectWidth(text_cells);
  if (columns <= 0 || RangeEmpty(visible_lines)) return;

  RectF32 text_rect = TextRect(text_cells, content_rect, ctx);

  u64 diagnostic_count = 0;
  const LspDiagnostic *diagnostics = EditorLspUiDiagnosticsForBuffer(ui, buffer, &diagnostic_count);
  if (diagnostics == nullptr || diagnostic_count == 0) return;

  if (gutter_columns > 0) {
    for (u64 line = visible_lines.min; line < visible_lines.max; line += 1) {
      LspDiagnosticSeverity severity = EditorLspUiHighestSeverityForLine(ui, buffer, line);
      u8 marker = DiagnosticMarker(severity);
      if (marker == 0) continue;
      u8 text[1] = {marker};
      f32 top = text_rect.y0 + (f32)(line - visible_lines.min) * ctx->cell_height;
      DrawText(ctx->draw, String8{text, 1}, content_rect.x0,
               top + ctx->atlas->ascent, DiagnosticColor(&ctx->theme, severity));
    }
  }

  u64 visible_columns_min = view->scroll_column;
  u64 visible_columns_max = view->scroll_column + (u64)columns;
  for (u64 i = 0; i < diagnostic_count; i += 1) {
    const LspDiagnostic &diagnostic = diagnostics[i];
    u64 last_line = diagnostic.range.end.line;
    bool through_newline =
        diagnostic.range.end.character == 0 && diagnostic.range.end.line > diagnostic.range.start.line;
    if (through_newline) last_line -= 1;
    if (diagnostic.range.start.line > last_line) continue;
    if (last_line < visible_lines.min || diagnostic.range.start.line >= visible_lines.max) continue;

    u64 start_line = Max((u64)diagnostic.range.start.line, visible_lines.min);
    u64 end_line = Min(last_line, visible_lines.max - 1);
    Vec4F32 color = DiagnosticColor(&ctx->theme, diagnostic.severity);

    u64 start_offset = LspOffsetFromPosition(buffer, diagnostic.range.start, position_encoding);
    u64 end_offset = through_newline ? 0 : LspOffsetFromPosition(buffer, diagnostic.range.end, position_encoding);

    for (u64 line = start_line; line <= end_line; line += 1) {
      u64 start_column = (line == diagnostic.range.start.line) ? BufferColumnFromOffset(buffer, start_offset) : 0;
      u64 end_column = 0;
      if (line < last_line || through_newline) {
        end_column = LineEndColumn(buffer, line);
      } else {
        end_column = BufferColumnFromOffset(buffer, end_offset);
      }
      if (end_column <= start_column) end_column = start_column + 1;

      u64 clipped_start = Max(start_column, visible_columns_min);
      u64 clipped_end = Min(end_column, visible_columns_max);
      if (clipped_end <= clipped_start) continue;

      f32 top = text_rect.y0 + (f32)(line - visible_lines.min) * ctx->cell_height;
      f32 x0 = ColumnX(ctx, text_rect, clipped_start, view->scroll_column);
      f32 x1 = ColumnX(ctx, text_rect, clipped_end, view->scroll_column);
      DrawRect(ctx->draw,
               RectF32{x0, top + ctx->cell_height - kHairline, Min(x1, text_rect.x1),
                       top + ctx->cell_height},
               color);
    }
  }
}

void RenderLspPopup(RenderContext *ctx, const EditorLspUiPopupView *popup, Buffer *buffer,
                    const View *view, RectF32 content_rect, RectS32 text_cells,
                    RangeU64 visible_lines) {
  if (ctx == nullptr || popup == nullptr || buffer == nullptr || view == nullptr) return;
  if (popup->kind == EditorLspUiPopupKind::None) return;
  if (!BufferHandleEqual(popup->anchor_buffer, buffer->handle)) return;

  i32 text_columns = RectWidth(text_cells);
  if (text_columns <= 0) return;

  RectF32 text_rect = TextRect(text_cells, content_rect, ctx);
  i32 content_columns = Max((i32)(RectWidthF(content_rect) / ctx->cell_width), 1);
  i32 content_rows = Max((i32)(RectHeightF(content_rect) / ctx->cell_height), 1);

  u64 anchor_line = BufferLineFromOffset(buffer, popup->anchor_offset);
  u64 anchor_column = BufferColumnFromOffset(buffer, popup->anchor_offset);
  f32 anchor_x = ColumnX(ctx, text_rect, anchor_column, view->scroll_column);

  i64 relative_line = (i64)anchor_line - (i64)visible_lines.min;
  if (relative_line < 0) relative_line = 0;
  if (relative_line >= content_rows) relative_line = content_rows - 1;
  f32 anchor_row_top = text_rect.y0 + (f32)relative_line * ctx->cell_height;

  TempArena scratch = ScratchBegin();
  if (popup->kind == EditorLspUiPopupKind::Completion) {
    u64 count = popup->completion.count;
    if (count == 0 || popup->completion.items == nullptr) {
      ScratchEnd(scratch);
      return;
    }

    i32 visible_rows = (i32)Min(count, kMaxCompletionRows);
    visible_rows = Min(visible_rows, Max(content_rows - 1, 1));

    u64 top_row = popup->completion.scroll;
    if (count > (u64)visible_rows) top_row = Min(top_row, count - (u64)visible_rows);
    if (popup->completion.selected < top_row) top_row = popup->completion.selected;
    if (popup->completion.selected >= top_row + (u64)visible_rows) {
      top_row = popup->completion.selected - (u64)visible_rows + 1;
    }

    i32 width_limit = Min(content_columns, kPopupTextMaxWidthCells);
    i32 inner_width = 1;
    for (u64 i = 0; i < count; i += 1) {
      const EditorLspUiCompletionItem &item = popup->completion.items[i];
      i32 label_cells = (i32)Min(Utf8Length(item.label), (u64)width_limit);
      i32 detail_cells = 0;
      if (item.detail.size > 0) {
        detail_cells = (i32)Min(Utf8Length(item.detail), (u64)kPopupDetailMaxCells);
      }
      i32 line_cells = label_cells;
      if (detail_cells > 0) line_cells += kCompletionGapCells + detail_cells;
      inner_width = Max(inner_width, line_cells);
    }
    i32 popup_width_cells = Min(content_columns, inner_width + 2 * kPopupPaddingCells);
    f32 popup_width = (f32)popup_width_cells * ctx->cell_width;
    f32 popup_height = (f32)(visible_rows + 2 * kPopupPaddingCells) * ctx->cell_height;
    RectF32 rect = PlacePopup(content_rect, anchor_x, anchor_row_top, popup_width, popup_height,
                              ctx->cell_height);

    DrawPopupFrame(ctx, rect);
    f32 row_x = rect.x0 + (f32)kPopupPaddingCells * ctx->cell_width;
    f32 row_right = rect.x1 - (f32)kPopupPaddingCells * ctx->cell_width;
    i32 row_width_cells = Max(popup_width_cells - 2 * kPopupPaddingCells, 1);
    for (i32 row = 0; row < visible_rows; row += 1) {
      u64 item_index = top_row + (u64)row;
      const EditorLspUiCompletionItem &item = popup->completion.items[item_index];
      f32 row_top = rect.y0 + (f32)(row + kPopupPaddingCells) * ctx->cell_height;
      if (item_index == popup->completion.selected) {
        DrawRect(ctx->draw, RectF32{row_x, row_top, row_right, row_top + ctx->cell_height},
                 ctx->theme.popup_selected);
      }

      String8 detail = item.detail;
      String8 label = item.label;
      i32 detail_cells = 0;
      if (detail.size > 0 && row_width_cells > 8) {
        detail = TruncateToCells(scratch.arena, detail, Min(kPopupDetailMaxCells, row_width_cells / 2));
        detail_cells = (i32)Utf8Length(detail);
      }

      i32 label_cells = row_width_cells;
      if (detail_cells > 0) label_cells -= detail_cells + kCompletionGapCells;
      if (label_cells < 1) {
        detail = {};
        detail_cells = 0;
        label_cells = row_width_cells;
      }
      label = TruncateToCells(scratch.arena, label, label_cells);

      f32 baseline = row_top + ctx->atlas->ascent;
      DrawText(ctx->draw, label, row_x, baseline, ctx->theme.popup_text);
      if (detail_cells > 0) {
        f32 detail_x = row_right - (f32)detail_cells * ctx->cell_width;
        DrawText(ctx->draw, detail, detail_x, baseline, ctx->theme.popup_detail);
      }
    }
  } else {
    u64 line_count = popup->text.line_count;
    if (line_count == 0 || popup->text.lines == nullptr) {
      ScratchEnd(scratch);
      return;
    }

    i32 line_limit = Min(Max(content_rows - 2 * kPopupPaddingCells, 1), (i32)line_count);
    i32 text_width_cells = 1;
    i32 max_text_width = Max(Min(content_columns - 2 * kPopupPaddingCells, kPopupTextMaxWidthCells), 1);
    for (u64 i = 0; i < line_count; i += 1) {
      text_width_cells = Max(text_width_cells,
                             (i32)Min(Utf8Length(popup->text.lines[i]), (u64)max_text_width));
    }
    i32 popup_width_cells = Min(content_columns, text_width_cells + 2 * kPopupPaddingCells);
    f32 popup_width = (f32)popup_width_cells * ctx->cell_width;
    f32 popup_height = (f32)(line_limit + 2 * kPopupPaddingCells) * ctx->cell_height;
    RectF32 rect = PlacePopup(content_rect, anchor_x, anchor_row_top, popup_width, popup_height,
                              ctx->cell_height);

    DrawPopupFrame(ctx, rect);
    f32 text_x = rect.x0 + (f32)kPopupPaddingCells * ctx->cell_width;
    for (i32 row = 0; row < line_limit; row += 1) {
      String8 line = TruncateToCells(scratch.arena, popup->text.lines[row],
                                     popup_width_cells - 2 * kPopupPaddingCells);
      f32 baseline = rect.y0 + (f32)(row + kPopupPaddingCells) * ctx->cell_height + ctx->atlas->ascent;
      DrawText(ctx->draw, line, text_x, baseline, ctx->theme.popup_text);
    }
  }
  ScratchEnd(scratch);
}
