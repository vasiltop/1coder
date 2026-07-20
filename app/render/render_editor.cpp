#include "render/render_editor.h"
#include "render/render_metrics.h"

#include "buffers/buf_image.h"
#include "editor/lsp.h"
#include "render/render_lsp.h"
#include "vim/vim_search.h"

namespace {

// A cap on highlighted matches per frame. Only the visible lines are scanned,
// so this is reached only by a pattern matching almost every character, where
// the highlight has stopped being useful anyway.
constexpr u64 kMaxVisibleSearchMatches = 512;

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

// Draws one line's number in the gutter, right-aligned against the blank column
// that separates it from the text. What the number *is* -- absolute or a
// distance from the cursor -- is core's answer, not the renderer's.
void DrawLineNumber(RenderContext *ctx, const Editor *ed, const View *view, const Buffer *buffer,
                    u64 line, f32 gutter_x, f32 baseline_y, i32 gutter, bool is_cursor_line) {
  TempArena scratch = ScratchBegin();

  u64 label = EditorLineNumberLabel(ed, view, buffer, line);
  String8 text = PushStr8F(scratch.arena, "%llu", (unsigned long long)label);

  // The last gutter column is deliberately left empty, so the digits end one
  // cell short of the text.
  f32 right_edge = gutter_x + (f32)(gutter - 1) * ctx->cell_width;
  f32 x = right_edge - (f32)Utf8Length(text) * ctx->cell_width;
  // A number too wide for the gutter would run into the text; clamp instead.
  if (x < gutter_x) x = gutter_x;

  DrawText(ctx->draw, text, x, baseline_y,
           is_cursor_line ? ctx->theme.line_number_current : ctx->theme.line_number);

  ScratchEnd(scratch);
}

// Draws one line, a cell per codepoint, coloured by its syntax token.
// Everything left of the horizontal scroll is skipped outright and everything
// past the right edge stops the loop, so a very long line costs no more to draw
// than a short one.
void DrawBufferLine(RenderContext *ctx, const Buffer *buffer, u64 line, RectF32 text_rect,
                    f32 baseline_y, u64 scroll_column, i32 columns) {
  RangeU64 range = BufferLineRange(buffer, line);

  const TokenArray *tokens = &buffer->tokens;
  u64 token_index = TokenIndexAtOffset(tokens, range.min);

  u64 column = 0;
  for (u64 p = range.min; p < range.max;) {
    DecodedCodepoint decoded = BufferDecodeAt(buffer, p);

    if (column >= scroll_column) {
      if (column - scroll_column >= (u64)columns) break;

      // Tabs occupy a cell and draw nothing.
      if (decoded.codepoint != '\t') {
        while (token_index < tokens->count && tokens->tokens[token_index].end <= p) {
          token_index += 1;
        }

        TokenKind kind = TokenKind::Default;
        if (token_index < tokens->count) {
          const Token *token = &tokens->tokens[token_index];
          if (token->start <= p && p < token->end) kind = token->kind;
        }

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
                         RangeU64 selection, RectF32 text_rect, f32 top, i32 columns,
                         Vec4F32 color) {
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

  DrawRect(ctx->draw, RectF32{x0, top, Min(x1, text_rect.x1), top + ctx->cell_height}, color);
}

// Draws the cursor in the cell whose top-left corner is (x, y). Shared by
// panels and the command window, so the shape always reflects the mode of
// whichever view owns it. `offset` says which character the block covers --
// passed rather than read off the view, so a view with several cursors can
// draw each of them through here.
void DrawCursorCell(RenderContext *ctx, const Buffer *buffer, const View *view, u64 offset, f32 x,
                    f32 y, bool focused, Vec4F32 color) {
  RectF32 cell = {x, y, x + ctx->cell_width, y + ctx->cell_height};

  if (!focused) {
    // An unfocused window shows where its cursor sits without competing with
    // the focused one, so it gets an outline rather than a solid block.
    constexpr f32 t = 1.0f;
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y0, cell.x1, cell.y0 + t}, color);
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y1 - t, cell.x1, cell.y1}, color);
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y0, cell.x0 + t, cell.y1}, color);
    DrawRect(ctx->draw, RectF32{cell.x1 - t, cell.y0, cell.x1, cell.y1}, color);
    return;
  }

  if (VimModeIsInsert(view->vim.mode)) {
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y0, cell.x0 + 2.0f, cell.y1}, color);
    return;
  }

  // A block, with the covered character redrawn in the background colour so it
  // stays legible through it.
  DrawRect(ctx->draw, cell, color);

  DecodedCodepoint under = BufferDecodeAt(buffer, offset);
  if (under.codepoint != 0 && under.codepoint != '\n') {
    DrawGlyph(ctx->draw, under.codepoint, cell.x0, cell.y0 + ctx->atlas->ascent,
              ctx->theme.cursor_text);
  }
}

// Where an offset lands on screen, or false when it is scrolled out of sight.
[[nodiscard]] bool CursorCellOrigin(RenderContext *ctx, const Buffer *buffer, const View *view,
                                    u64 offset, RectF32 text_rect, RangeU64 visible, i32 columns,
                                    f32 *out_x, f32 *out_y) {
  u64 line = BufferLineFromOffset(buffer, offset);
  if (line < visible.min || line >= visible.max) return false;

  u64 column = BufferColumnFromOffset(buffer, offset);
  if (column < view->scroll_column || column - view->scroll_column >= (u64)columns) return false;

  *out_x = ColumnX(ctx, text_rect, column, view->scroll_column);
  *out_y = text_rect.y0 + (f32)(line - visible.min) * ctx->cell_height;
  return true;
}

void DrawCursor(RenderContext *ctx, const Buffer *buffer, const View *view, RectF32 text_rect,
                RangeU64 visible, i32 columns, bool focused) {
  f32 x = 0.0f;
  f32 y = 0.0f;

  // Positions marked but not yet confirmed get an underline rather than a
  // block: they are where cursors *would* go, and must not be mistaken for
  // cursors that are already there.
  for (u64 i = 0; i < view->pending_count; i += 1) {
    if (!CursorCellOrigin(ctx, buffer, view, view->pending[i], text_rect, visible, columns, &x, &y))
      continue;
    RectF32 cell = {x, y, x + ctx->cell_width, y + ctx->cell_height};
    DrawRect(ctx->draw, RectF32{cell.x0, cell.y1 - 2.0f, cell.x1, cell.y1},
             ctx->theme.cursor_pending);
  }

  // Secondaries first, primary last, so the primary wins wherever they overlap.
  for (u64 i = 0; i < view->extra_count; i += 1) {
    if (!CursorCellOrigin(ctx, buffer, view, view->extras[i].offset, text_rect, visible, columns,
                          &x, &y))
      continue;
    DrawCursorCell(ctx, buffer, view, view->extras[i].offset, x, y, focused,
                   ctx->theme.cursor_secondary);
  }

  if (CursorCellOrigin(ctx, buffer, view, view->cursor, text_rect, visible, columns, &x, &y)) {
    DrawCursorCell(ctx, buffer, view, view->cursor, x, y, focused, ctx->theme.cursor);
  }
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

  // How many cursors are live, or that marks are being placed -- both are
  // states the user has to be able to see they are in before they type.
  String8 cursors = Str8Lit("");
  if (view->placing) {
    cursors = PushStr8F(scratch.arena, "PLACE %llu  ", (unsigned long long)view->pending_count);
  } else if (view->extra_count > 0) {
    cursors = PushStr8F(scratch.arena, "%llu cursors  ",
                        (unsigned long long)(view->extra_count + 1));
  }

  String8 mode = VimModeName(view->vim.mode);
  String8 right = PushStr8F(scratch.arena, "%.*s%.*s  %llu:%llu ", (int)cursors.size,
                            (char *)cursors.str, (int)mode.size, (char *)mode.str,
                            (unsigned long long)(ViewCursorLine(view, buffer) + 1),
                            (unsigned long long)(ViewCursorColumn(view, buffer) + 1));
  f32 right_x = rect.x1 - (f32)Utf8Length(right) * ctx->cell_width;
  DrawText(ctx->draw, right, right_x, baseline, color);

  ScratchEnd(scratch);
  DrawPopClip(ctx->draw);
}

// An image buffer's text is a metadata summary, and it is drawn inside a frame
// sized to the image's aspect ratio. No pixels are decoded yet -- core has no
// decoder and this layer has no second texture -- so the frame is what conveys
// the shape while the summary conveys the rest.
//
// When decoding lands it replaces the fill here and nothing else moves: the
// rect is already the right one to draw into.
void DrawImagePlaceholder(RenderContext *ctx, const ImageInfo *info, RectF32 text_rect) {
  f32 available_w = text_rect.x1 - text_rect.x0;
  f32 available_h = text_rect.y1 - text_rect.y0;
  if (available_w <= 0.0f || available_h <= 0.0f) return;

  // Leave a margin so the frame reads as an object in the panel rather than as
  // a border on the panel.
  f32 max_w = available_w * 0.8f;
  f32 max_h = available_h * 0.8f;

  f32 width = max_w;
  f32 height = max_h;
  if (info->width != 0 && info->height != 0) {
    f32 aspect = (f32)info->width / (f32)info->height;
    height = Min(max_h, max_w / aspect);
    width = height * aspect;
  }

  f32 cx = (text_rect.x0 + text_rect.x1) * 0.5f;
  f32 cy = (text_rect.y0 + text_rect.y1) * 0.5f;
  RectF32 frame = {cx - width * 0.5f, cy - height * 0.5f, cx + width * 0.5f, cy + height * 0.5f};

  DrawRect(ctx->draw, frame, ctx->theme.current_line);

  // Four hairlines rather than an outline primitive, which the draw list has no
  // need for anywhere else.
  f32 t = 1.0f;
  DrawRect(ctx->draw, RectF32{frame.x0, frame.y0, frame.x1, frame.y0 + t},
           ctx->theme.split_border);
  DrawRect(ctx->draw, RectF32{frame.x0, frame.y1 - t, frame.x1, frame.y1},
           ctx->theme.split_border);
  DrawRect(ctx->draw, RectF32{frame.x0, frame.y0, frame.x0 + t, frame.y1},
           ctx->theme.split_border);
  DrawRect(ctx->draw, RectF32{frame.x1 - t, frame.y0, frame.x1, frame.y1},
           ctx->theme.split_border);
}

void RenderPanel(RenderContext *ctx, Editor *ed, Panel *panel, bool focused) {
  View *view = panel->view;
  Buffer *buffer = EditorBufferForView(ed, view);
  if (!view || !buffer) return;

  EditorLspUi *lsp_ui = nullptr;
  LspPositionEncoding lsp_position_encoding = LspPositionEncoding::Utf16;
  if (buffer->kind == BufferKind::File) {
    lsp_ui = ed->lsp_ui;
    EditorLspBufferInfo lsp_info = {};
    if (EditorLspGetBufferInfo(ed, buffer, &lsp_info)) {
      lsp_position_encoding = lsp_info.position_encoding;
    }
  }

  RectS32 text_cells = EditorPanelTextRect(ed, panel);
  RectF32 panel_rect = CellsToPixels(ctx, panel->rect);

  // A window height is rarely an exact multiple of the cell height, and those
  // few leftover pixels have to land somewhere. Panels along the bottom stretch
  // to meet the command line, which puts the remainder inside the text area --
  // where unused space is ordinary and invisible -- rather than leaving a strip
  // of background above or below the command line.
  if (panel->rect.y1 >= ed->screen.y1 - 1) {
    panel_rect.y1 = ctx->panel_bottom;
  }

  // Derived from the panel rather than converted separately, so the stretch
  // above carries through and the status line stays exactly one cell tall.
  RectF32 content_rect = panel_rect;
  content_rect.y1 = panel_rect.y1 - ctx->cell_height;

  // The gutter is taken off the left in pixels here and in cells in
  // EditorPanelTextRect. Both have to move together: `columns` comes from the
  // cell rect, so narrowing only one of them would shrink the column count
  // while glyphs still started at the panel edge, quietly clipping the right.
  i32 gutter = EditorGutterWidth(ed, panel);
  RectF32 text_rect = content_rect;
  text_rect.x0 = content_rect.x0 + (f32)gutter * ctx->cell_width;

  i32 columns = RectWidth(text_cells);
  i32 rows = RectHeight(text_cells);
  if (columns <= 0 || rows <= 0) return;

  DrawPushClip(ctx->draw, content_rect);

  // The frame goes down before the text, which keeps drawing where it always
  // does. Centring the summary inside the frame would mean offsetting the text
  // origin, and the cursor and selection rects are all derived from text_rect
  // -- they would drift away from the glyphs.
  const ImageInfo *image = ImageBufferInfo(buffer);
  if (image) DrawImagePlaceholder(ctx, image, text_rect);

  RangeU64 visible = ViewVisibleLines(view, buffer, rows);
  RangeU64 selection = ViewSelection(view, buffer);
  bool has_selection = VimModeIsVisual(view->vim.mode) && !RangeEmpty(selection);
  u64 cursor_line = ViewCursorLine(view, buffer);

  // Search matches, gathered once for the visible span rather than per line.
  // Only what is on screen is scanned, so highlighting costs the same on a
  // large file as on a small one.
  RangeU64 matches[kMaxVisibleSearchMatches];
  u64 match_count = 0;
  if (ed->search_highlight && ed->search_pattern.size > 0) {
    RangeU64 span = {BufferOffsetFromLine(buffer, visible.min),
                     BufferLineEnd(buffer, (visible.max > 0) ? visible.max - 1 : 0)};
    match_count = BufferSearchAll(buffer, ed->search_pattern, span, matches,
                                  kMaxVisibleSearchMatches);
  }

  // Only the visible lines are touched, which is what keeps a large file as
  // cheap to draw as a small one.
  for (u64 line = visible.min; line < visible.max; line += 1) {
    f32 top = text_rect.y0 + (f32)(line - visible.min) * ctx->cell_height;

    // The wash starts at the gutter, not the text, so the current line reads as
    // one band -- which is what vim's 'cursorline' does.
    if (focused && line == cursor_line) {
      DrawRect(ctx->draw, RectF32{content_rect.x0, top, text_rect.x1, top + ctx->cell_height},
               ctx->theme.current_line);
    }

    if (gutter > 0) {
      DrawLineNumber(ctx, ed, view, buffer, line, content_rect.x0,
                     top + ctx->atlas->ascent, gutter, line == cursor_line);
    }
    // Matches go under the selection, so a selected match still reads as
    // selected.
    for (u64 i = 0; i < match_count; i += 1) {
      DrawSelectionOnLine(ctx, buffer, view, line, matches[i], text_rect, top, columns,
                          ctx->theme.search_match);
    }
    if (has_selection) {
      DrawSelectionOnLine(ctx, buffer, view, line, selection, text_rect, top, columns,
                          ctx->theme.selection);
    }

    DrawBufferLine(ctx, buffer, line, text_rect, top + ctx->atlas->ascent, view->scroll_column,
                   columns);
  }

  if (lsp_ui != nullptr) {
    RenderLspDiagnostics(ctx, lsp_ui, buffer, view, lsp_position_encoding, content_rect,
                        text_cells, visible, gutter);
  }

  // While the command window is open it holds the cursor, so the panel's own
  // is drawn as an outline rather than competing with a second block.
  DrawCursor(ctx, buffer, view, text_rect, visible, columns, focused && !ed->command_line_active);

  if (focused && lsp_ui != nullptr) {
    if (const EditorLspUiPopupView *popup = EditorLspUiPopup(lsp_ui)) {
      RenderLspPopup(ctx, popup, buffer, view, content_rect, text_cells, visible);
    }
  }
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
  RectF32 rect = {0.0f, pixel_height - ctx->cell_height, pixel_width, pixel_height};
  DrawRect(ctx->draw, rect, ctx->theme.background);
  DrawPushClip(ctx->draw, rect);

  f32 baseline = rect.y0 + ctx->atlas->ascent;
  TempArena scratch = ScratchBegin();

  if (ed->command_line_active) {
    Buffer *command = BufferFromHandle(&ed->buffers, ed->command_buffer);
    View *view = ed->command_view;

    if (command && view) {
      u8 prompt = ed->command_line_prompt ? ed->command_line_prompt : (u8)':';
      String8 prompt_text = String8{&prompt, 1};
      if (Str8Match(ed->command_line_purpose, Str8Lit("rename"))) {
        prompt_text = Str8Lit("Rename: ");
      }
      u64 prompt_columns = Utf8Length(prompt_text);
      f32 text_x = ctx->cell_width * (f32)prompt_columns;

      DrawText(ctx->draw, prompt_text, 0.0f, baseline, ctx->theme.text);
      DrawText(ctx->draw, BufferTextAll(scratch.arena, command), text_x, baseline,
               ctx->theme.text);

      // Same cursor as a panel: a block in normal mode, a bar in insert, with
      // the covered character redrawn through it.
      u64 column = BufferColumnFromOffset(command, view->cursor) + prompt_columns;
      DrawCursorCell(ctx, command, view, view->cursor, (f32)column * ctx->cell_width, rect.y0, true,
                     ctx->theme.cursor);
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
  // advance is already normalised at the atlas boundary; line_height is
  // fractional and is normalised here.  Using the struct makes the single
  // normalisation point and verbatim pass-through explicit.
  RenderCellMetrics metrics = RenderCellMetricsFromAtlas(atlas->advance, atlas->line_height);
  ctx->cell_width = metrics.cell_width;
  ctx->cell_height = metrics.cell_height;
}

RectS32 RenderScreenCells(const RenderContext *ctx, i32 pixel_width, i32 pixel_height) {
  i32 columns = (ctx->cell_width > 0.0f) ? (i32)((f32)pixel_width / ctx->cell_width) : 1;
  i32 rows = (ctx->cell_height > 0.0f) ? (i32)((f32)pixel_height / ctx->cell_height) : 1;
  return RectS32{0, 0, Max(columns, 1), Max(rows, 1)};
}

void RenderEditor(RenderContext *ctx, Editor *ed, i32 pixel_width, i32 pixel_height) {
  // Where the panel area ends and the command line begins.
  ctx->panel_bottom = (f32)pixel_height - ctx->cell_height;

  DrawBegin(ctx->draw);

  DrawRect(ctx->draw, RectF32{0.0f, 0.0f, (f32)pixel_width, (f32)pixel_height},
           ctx->theme.background);

  RenderPanelTree(ctx, ed, ed->root_panel);
  RenderCommandLine(ctx, ed, (f32)pixel_width, (f32)pixel_height);

  DrawFlush(ctx->draw);
}
