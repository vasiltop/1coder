#include "render/render_editor.h"

namespace {

// Pixel rect of a cell rect.
[[nodiscard]] RectF32 CellsToPixels(const RenderContext *ctx, RectS32 cells) {
  return RectF32{
      (f32)cells.x0 * ctx->cell_width,
      (f32)cells.y0 * ctx->cell_height,
      (f32)cells.x1 * ctx->cell_width,
      (f32)cells.y1 * ctx->cell_height,
  };
}

// X of a column, given the view's horizontal scroll. Columns left of the scroll
// clamp to the left edge.
[[nodiscard]] f32 ColumnX(const RenderContext *ctx, RectF32 text_rect, u64 column,
                          u64 scroll_column) {
  u64 relative = (column > scroll_column) ? column - scroll_column : 0;
  return text_rect.x0 + (f32)relative * ctx->cell_width;
}

// Draws one line, a cell per codepoint, coloured by its syntax token.
// Everything left of the horizontal scroll is skipped outright and everything
// past the right edge stops the loop, so a very long line costs no more to draw
// than a short one.
void DrawBufferLine(RenderContext *ctx, const Buffer *buffer, u64 line, RectF32 text_rect,
                    f32 baseline_y, u64 scroll_column, i32 columns) {
  RangeU64 range = BufferLineRange(buffer, line);

  u64 column = 0;
  for (u64 p = range.min; p < range.max;) {
    DecodedCodepoint decoded = BufferDecodeAt(buffer, p);

    if (column >= scroll_column) {
      if (column - scroll_column >= (u64)columns) break;

      // Tabs occupy a cell and draw nothing.
      if (decoded.codepoint != '\t') {
        TokenKind kind = TokenKindAtOffset(&buffer->tokens, p);
        f32 x = ColumnX(ctx, text_rect, column, scroll_column);
        DrawGlyph(ctx->draw, decoded.codepoint, x, baseline_y,
                  ThemeColorForToken(&ctx->theme, kind));
      }
    }

    column += 1;
    p += Max(decoded.advance, (u32)1);
  }
}

void DrawSelectionOnLine(RenderContext *ctx, const Buffer *buffer, const View *view, u64 line,
                         RangeU64 selection, RectF32 text_rect, f32 top, i32 columns) {
  RangeU64 line_range = LineRangeWithNewline(&buffer->lines, &buffer->text, line);
  RangeU64 hit = RangeIntersect(selection, line_range);
  if (RangeEmpty(hit)) return;

  u64 line_end = BufferLineEnd(buffer, line);
  u64 start_column = BufferColumnFromOffset(buffer, hit.min);

  // A selection that runs through the line ending covers the rest of the row,
  // which is what makes linewise selection read as whole lines.
  bool through_newline = (hit.max > line_end);
  u64 end_column = through_newline ? view->scroll_column + (u64)columns
                                   : BufferColumnFromOffset(buffer, hit.max);

  if (end_column <= view->scroll_column) return;

  f32 x0 = ColumnX(ctx, text_rect, start_column, view->scroll_column);
  f32 x1 = ColumnX(ctx, text_rect, end_column, view->scroll_column);

  DrawRect(ctx->draw, RectF32{x0, top, Min(x1, text_rect.x1), top + ctx->cell_height},
           ctx->theme.selection);
}

// Draws the cursor in the cell whose top-left corner is (x, y). Shared by
// panels and the command window, so the shape always reflects the mode of
// whichever view owns it.
void DrawCursorCell(RenderContext *ctx, const Buffer *buffer, const View *view, f32 x, f32 y,
                    bool focused) {
  RectF32 cell = {x, y, x + ctx->cell_width, y + ctx->cell_height};

  if (!focused) {
    // An unfocused window shows where its cursor sits without competing with
    // the focused one, so it gets an outline rather than a solid block.
    constexpr f32 t = 1.0f;
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y0, cell.x1, cell.y0 + t}, ctx->theme.cursor);
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y1 - t, cell.x1, cell.y1}, ctx->theme.cursor);
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y0, cell.x0 + t, cell.y1}, ctx->theme.cursor);
    DrawRect(ctx->draw, RectF32{cell.x1 - t, cell.y0, cell.x1, cell.y1}, ctx->theme.cursor);
    return;
  }

  if (VimModeIsInsert(view->vim.mode)) {
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y0, cell.x0 + 2.0f, cell.y1}, ctx->theme.cursor);
    return;
  }

  // A block, with the covered character redrawn in the background colour so it
  // stays legible through it.
  DrawRect(ctx->draw, cell, ctx->theme.cursor);

  DecodedCodepoint under = BufferDecodeAt(buffer, view->cursor);
  if (under.codepoint != 0 && under.codepoint != '\n') {
    DrawGlyph(ctx->draw, under.codepoint, cell.x0, cell.y0 + ctx->atlas->ascent,
              ctx->theme.cursor_text);
  }
}

void DrawCursor(RenderContext *ctx, const Buffer *buffer, const View *view, RectF32 text_rect,
                RangeU64 visible, i32 columns, bool focused) {
  u64 cursor_line = ViewCursorLine(view, buffer);
  if (cursor_line < visible.min || cursor_line >= visible.max) return;

  u64 cursor_column = ViewCursorColumn(view, buffer);
  if (cursor_column < view->scroll_column ||
      cursor_column - view->scroll_column >= (u64)columns) {
    return;
  }

  f32 x = ColumnX(ctx, text_rect, cursor_column, view->scroll_column);
  f32 y = text_rect.y0 + (f32)(cursor_line - visible.min) * ctx->cell_height;
  DrawCursorCell(ctx, buffer, view, x, y, focused);
}

void DrawStatusLine(RenderContext *ctx, Editor *ed, const Buffer *buffer, const View *view,
                    RectF32 panel_rect, bool focused) {
  RectF32 rect = {panel_rect.x0, panel_rect.y1 - ctx->cell_height, panel_rect.x1, panel_rect.y1};

  DrawRect(ctx->draw, rect,
           focused ? ctx->theme.status_background : ctx->theme.status_background_inactive);
  DrawPushClip(ctx->draw, rect);

  Vec4F32 color = focused ? ctx->theme.status_text : ctx->theme.status_text_inactive;
  f32 baseline = rect.y0 + ctx->atlas->ascent;

  TempArena scratch = ScratchBegin();

  String8 name = (buffer->name.size > 0) ? buffer->name : Str8Lit("[no name]");
  String8 left = PushStr8F(scratch.arena, " %.*s%s%s", (int)name.size, (char *)name.str,
                           BufferIsDirty(buffer) ? " +" : "",
                           BufferIsReadOnly(buffer) ? " [ro]" : "");
  DrawText(ctx->draw, left, rect.x0, baseline, color);

  String8 mode = VimModeName(view->vim.mode);
  String8 right = PushStr8F(scratch.arena, "%.*s  %llu:%llu ", (int)mode.size, (char *)mode.str,
                            (unsigned long long)(ViewCursorLine(view, buffer) + 1),
                            (unsigned long long)(ViewCursorColumn(view, buffer) + 1));
  f32 right_x = rect.x1 - (f32)Utf8Length(right) * ctx->cell_width;
  DrawText(ctx->draw, right, right_x, baseline, color);

  ScratchEnd(scratch);
  DrawPopClip(ctx->draw);
}

void RenderPanel(RenderContext *ctx, Editor *ed, Panel *panel, bool focused) {
  View *view = panel->view;
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!view || !buffer) return;

  RectS32 text_cells = EditorPanelTextRect(ed, panel);
  RectF32 text_rect = CellsToPixels(ctx, text_cells);
  RectF32 panel_rect = CellsToPixels(ctx, panel->rect);

  i32 columns = RectWidth(text_cells);
  i32 rows = RectHeight(text_cells);
  if (columns <= 0 || rows <= 0) return;

  DrawPushClip(ctx->draw, text_rect);

  RangeU64 visible = ViewVisibleLines(view, buffer, rows);
  RangeU64 selection = ViewSelection(view, buffer);
  bool has_selection = VimModeIsVisual(view->vim.mode) && !RangeEmpty(selection);
  u64 cursor_line = ViewCursorLine(view, buffer);

  // Only the visible lines are touched, which is what keeps a large file as
  // cheap to draw as a small one.
  for (u64 line = visible.min; line < visible.max; line += 1) {
    f32 top = text_rect.y0 + (f32)(line - visible.min) * ctx->cell_height;

    if (focused && line == cursor_line) {
      DrawRect(ctx->draw, RectF32{text_rect.x0, top, text_rect.x1, top + ctx->cell_height},
               ctx->theme.current_line);
    }
    if (has_selection) {
      DrawSelectionOnLine(ctx, buffer, view, line, selection, text_rect, top, columns);
    }

    DrawBufferLine(ctx, buffer, line, text_rect, top + ctx->atlas->ascent, view->scroll_column,
                   columns);
  }

  // While the command window is open it holds the cursor, so the panel's own
  // is drawn as an outline rather than competing with a second block.
  DrawCursor(ctx, buffer, view, text_rect, visible, columns,
             focused && !ed->command_line_active);
  DrawPopClip(ctx->draw);

  DrawStatusLine(ctx, ed, buffer, view, panel_rect, focused);

  // A hairline on the left edge, so splits read as separate without a heavy
  // border.
  if (panel_rect.x0 > 0.0f) {
    DrawRect(ctx->draw,
             RectF32{panel_rect.x0, panel_rect.y0, panel_rect.x0 + 1.0f, panel_rect.y1},
             ctx->theme.split_border);
  }
}

void RenderPanelTree(RenderContext *ctx, Editor *ed, Panel *panel) {
  if (!panel) return;

  if (PanelIsLeaf(panel)) {
    RenderPanel(ctx, ed, panel, panel == ed->focused_panel);
    return;
  }
  for (Panel *child = panel->first_child; child; child = child->next) {
    RenderPanelTree(ctx, ed, child);
  }
}

// The bottom row: the command window when open, otherwise the last message or
// a part-typed chord.
void RenderCommandLine(RenderContext *ctx, Editor *ed, f32 pixel_width, f32 pixel_height) {
  // Starts where the panels stop, not one cell up from the bottom of the
  // window. The two are only the same when the window height divides exactly
  // by the cell height; otherwise the leftover pixels fall between them and the
  // command line appears to be two rows tall. Anchoring it to the panels puts
  // the remainder below everything, where it cannot show as a gap.
  f32 top = (f32)(ed->screen.y1 - 1) * ctx->cell_height;
  RectF32 rect = {0.0f, top, pixel_width, Max(pixel_height, top + ctx->cell_height)};
  DrawRect(ctx->draw, rect, ctx->theme.background);
  DrawPushClip(ctx->draw, rect);

  f32 baseline = rect.y0 + ctx->atlas->ascent;
  TempArena scratch = ScratchBegin();

  if (ed->command_line_active) {
    Buffer *command = BufferFromHandle(&ed->buffers, ed->command_buffer);
    View *view = ed->command_view;

    if (command && view) {
      // The ':' prompt occupies the first cell, so the text -- and the cursor
      // with it -- is shifted one column right.
      constexpr u64 kPromptColumns = 1;
      f32 text_x = ctx->cell_width * (f32)kPromptColumns;

      DrawText(ctx->draw, Str8Lit(":"), 0.0f, baseline, ctx->theme.text);
      DrawText(ctx->draw, BufferTextAll(scratch.arena, command), text_x, baseline,
               ctx->theme.text);

      // Same cursor as a panel: a block in normal mode, a bar in insert, with
      // the covered character redrawn through it.
      u64 column = BufferColumnFromOffset(command, view->cursor) + kPromptColumns;
      DrawCursorCell(ctx, command, view, (f32)column * ctx->cell_width, rect.y0, true);
    }
  } else if (ed->status_message.size > 0) {
    DrawText(ctx->draw, ed->status_message, 0.0f, baseline, ctx->theme.message);
  }

  // A part-typed chord, shown at the right, so a prefix never looks like a hang.
  if (!ed->command_line_active && ed->input.pending_chord_count > 0) {
    KeyChordSequence pending = {ed->input.pending_chords, ed->input.pending_chord_count};
    String8 text = KeyChordSequenceToString(scratch.arena, pending);
    f32 x = pixel_width - (f32)Utf8Length(text) * ctx->cell_width - ctx->cell_width;
    DrawText(ctx->draw, text, x, baseline, ctx->theme.message);
  }

  ScratchEnd(scratch);
  DrawPopClip(ctx->draw);
}

}  // namespace

void RenderContextInit(RenderContext *ctx, DrawList *draw, GlyphAtlas *atlas) {
  ctx->draw = draw;
  ctx->atlas = atlas;
  ctx->theme = ThemeDefault();
  // Cell size comes straight from the font, which is exactly why the core can
  // lay out in cells and stay free of font metrics.
  ctx->cell_width = atlas->advance;
  ctx->cell_height = atlas->line_height;
}

RectS32 RenderScreenCells(const RenderContext *ctx, i32 pixel_width, i32 pixel_height) {
  i32 columns = (ctx->cell_width > 0.0f) ? (i32)((f32)pixel_width / ctx->cell_width) : 1;
  i32 rows = (ctx->cell_height > 0.0f) ? (i32)((f32)pixel_height / ctx->cell_height) : 1;
  return RectS32{0, 0, Max(columns, 1), Max(rows, 1)};
}

void RenderEditor(RenderContext *ctx, Editor *ed, i32 pixel_width, i32 pixel_height) {
  DrawBegin(ctx->draw);

  DrawRect(ctx->draw, RectF32{0.0f, 0.0f, (f32)pixel_width, (f32)pixel_height},
           ctx->theme.background);

  RenderPanelTree(ctx, ed, ed->root_panel);
  RenderCommandLine(ctx, ed, (f32)pixel_width, (f32)pixel_height);

  DrawFlush(ctx->draw);
}
