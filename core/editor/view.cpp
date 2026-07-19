#include "editor/view.h"

namespace {

// Number of codepoints on a line.
[[nodiscard]] u64 LineColumnCount(const Buffer *buffer, u64 line) {
  RangeU64 range = BufferLineRange(buffer, line);
  u64 columns = 0;
  for (u64 p = range.min; p < range.max;) {
    p = BufferNextCodepoint(buffer, p);
    columns += 1;
  }
  return columns;
}

}  // namespace

void ViewInit(View *view, BufferHandle buffer) {
  *view = View{};
  view->buffer = buffer;
  VimStateReset(&view->vim);
}

u64 ViewClampCursorToMode(const View *view, const Buffer *buffer, u64 offset) {
  u64 size = BufferSize(buffer);
  u64 clamped = Min(offset, size);

  u64 line = BufferLineFromOffset(buffer, clamped);
  u64 line_start = BufferOffsetFromLine(buffer, line);
  u64 line_end = BufferLineEnd(buffer, line);

  // Normal mode rests *on* a character, so it cannot sit past the last one.
  // Insert and visual mode may sit one past, where new text would land -- and
  // so may a buffer that opts out of modal editing, which behaves as a plain
  // text field and would otherwise refuse to place the cursor after what you
  // just typed.
  bool allow_past_end = VimModeIsInsert(view->vim.mode) || VimModeIsVisual(view->vim.mode) ||
                        HasFlag(buffer->flags, BufferFlags::NoVim);
  if (!allow_past_end && clamped >= line_end && line_end > line_start) {
    return BufferPrevCodepoint(buffer, line_end);
  }
  return Min(clamped, line_end);
}

void ViewSetCursor(View *view, const Buffer *buffer, u64 offset) {
  view->cursor = ViewClampCursorToMode(view, buffer, offset);
  view->preferred_column = BufferColumnFromOffset(buffer, view->cursor);
}

void ViewSetCursorKeepColumn(View *view, const Buffer *buffer, u64 offset) {
  // Vertical motion must not overwrite the remembered column, or moving through
  // a short line would permanently forget where the cursor wanted to be.
  view->cursor = ViewClampCursorToMode(view, buffer, offset);
}

void ViewSetCursorLineColumn(View *view, const Buffer *buffer, u64 line, u64 column) {
  u64 clamped_line = Min(line, BufferLineCount(buffer) - 1);
  u64 offset = BufferOffsetFromColumn(buffer, clamped_line, column);
  view->cursor = ViewClampCursorToMode(view, buffer, offset);
}

u64 ViewCursorLine(const View *view, const Buffer *buffer) {
  return BufferLineFromOffset(buffer, view->cursor);
}

u64 ViewCursorColumn(const View *view, const Buffer *buffer) {
  return BufferColumnFromOffset(buffer, view->cursor);
}

RangeU64 ViewSelection(const View *view, const Buffer *buffer) {
  if (!VimModeIsVisual(view->vim.mode)) return RangeU64{view->cursor, view->cursor};

  RangeU64 range = RangeMake(view->vim.visual_anchor, view->cursor);

  if (view->vim.mode == VimMode::VisualLine) {
    // Linewise selection covers whole lines regardless of where in them the
    // anchor and cursor sit.
    u64 first = BufferLineFromOffset(buffer, range.min);
    u64 last = BufferLineFromOffset(buffer, range.max);
    return RangeU64{BufferOffsetFromLine(buffer, first),
                    LineRangeWithNewline(&buffer->lines, &buffer->text, last).max};
  }

  // Characterwise visual mode includes the character under the cursor.
  return RangeU64{range.min, Min(BufferNextCodepoint(buffer, range.max), BufferSize(buffer))};
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

void ViewClampScroll(View *view, const Buffer *buffer, i32 viewport_height) {
  u64 line_count = BufferLineCount(buffer);

  // Allow scrolling until the last line reaches the top of the viewport, but no
  // further, so an edit that shrinks the buffer cannot strand the view in
  // empty space.
  u64 max_scroll = (line_count > 0) ? line_count - 1 : 0;
  if (viewport_height > 0 && line_count > (u64)viewport_height) {
    max_scroll = line_count - (u64)viewport_height;
  } else if (line_count <= (u64)viewport_height) {
    max_scroll = 0;
  }

  view->scroll_line = Min(view->scroll_line, max_scroll);
}

void ViewScrollToCursor(View *view, const Buffer *buffer, i32 viewport_width,
                        i32 viewport_height, i32 scroll_off, i32 side_scroll_off) {
  if (viewport_height <= 0 || viewport_width <= 0) return;

  u64 line_count = BufferLineCount(buffer);
  u64 cursor_line = ViewCursorLine(view, buffer);
  u64 cursor_column = ViewCursorColumn(view, buffer);

  // --- vertical ---
  // A viewport too short to hold the margins on both sides would oscillate, so
  // shrink the margin to fit.
  i32 margin = Min(scroll_off, (viewport_height - 1) / 2);
  margin = Max(margin, 0);

  u64 top_limit = (cursor_line > (u64)margin) ? cursor_line - (u64)margin : 0;
  if (view->scroll_line > top_limit) view->scroll_line = top_limit;

  u64 bottom_line = view->scroll_line + (u64)viewport_height - 1;
  u64 bottom_limit = cursor_line + (u64)margin;
  // Near the end of the buffer there are no lines left to keep as margin, so
  // stop demanding them rather than scrolling past the end.
  if (bottom_limit >= line_count) bottom_limit = (line_count > 0) ? line_count - 1 : 0;
  if (bottom_limit > bottom_line) {
    view->scroll_line = bottom_limit - (u64)viewport_height + 1;
  }

  ViewClampScroll(view, buffer, viewport_height);

  // --- horizontal ---
  i32 side_margin = Min(side_scroll_off, (viewport_width - 1) / 2);
  side_margin = Max(side_margin, 0);

  u64 left_limit = (cursor_column > (u64)side_margin) ? cursor_column - (u64)side_margin : 0;
  if (view->scroll_column > left_limit) view->scroll_column = left_limit;

  u64 right_column = view->scroll_column + (u64)viewport_width - 1;
  u64 right_limit = cursor_column + (u64)side_margin;
  if (right_limit > right_column) {
    view->scroll_column = right_limit - (u64)viewport_width + 1;
  }
}

void ViewScrollLines(View *view, const Buffer *buffer, i64 delta, i32 viewport_height) {
  if (viewport_height <= 0) return;

  i64 target = (i64)view->scroll_line + delta;
  view->scroll_line = (target > 0) ? (u64)target : 0;
  ViewClampScroll(view, buffer, viewport_height);

  // The cursor stays put unless the scroll would leave it off screen, in which
  // case it is dragged to the nearest visible line.
  u64 cursor_line = ViewCursorLine(view, buffer);
  u64 top = view->scroll_line;
  u64 bottom = view->scroll_line + (u64)viewport_height - 1;

  if (cursor_line < top) {
    ViewSetCursorLineColumn(view, buffer, top, view->preferred_column);
  } else if (cursor_line > bottom) {
    ViewSetCursorLineColumn(view, buffer, bottom, view->preferred_column);
  }
}

void ViewScrollColumns(View *view, const Buffer *buffer, i64 delta, i32 viewport_width) {
  i64 target = (i64)view->scroll_column + delta;
  view->scroll_column = (target > 0) ? (u64)target : 0;

  // Keep the cursor within the horizontal window, matching the vertical case.
  if (viewport_width > 0) {
    u64 cursor_column = ViewCursorColumn(view, buffer);
    u64 left = view->scroll_column;
    u64 right = view->scroll_column + (u64)viewport_width - 1;
    u64 line = ViewCursorLine(view, buffer);

    if (cursor_column < left) {
      ViewSetCursorLineColumn(view, buffer, line, left);
      view->preferred_column = left;
    } else if (cursor_column > right) {
      ViewSetCursorLineColumn(view, buffer, line, right);
      view->preferred_column = right;
    }
  }
}

void ViewScrollHalfPage(View *view, const Buffer *buffer, i64 direction, i32 viewport_height) {
  if (viewport_height <= 0) return;
  i64 amount = Max((i64)viewport_height / 2, (i64)1) * ((direction >= 0) ? 1 : -1);

  // ctrl-d/ctrl-u move the cursor with the view, unlike ctrl-e/ctrl-y.
  u64 cursor_line = ViewCursorLine(view, buffer);
  i64 target_line = (i64)cursor_line + amount;
  u64 line_count = BufferLineCount(buffer);
  u64 clamped = (target_line < 0) ? 0 : Min((u64)target_line, line_count - 1);

  ViewSetCursorLineColumn(view, buffer, clamped, view->preferred_column);

  i64 scroll_target = (i64)view->scroll_line + amount;
  view->scroll_line = (scroll_target > 0) ? (u64)scroll_target : 0;
  ViewClampScroll(view, buffer, viewport_height);
  ViewScrollToCursor(view, buffer, 1 << 20, viewport_height);
}

void ViewScrollFullPage(View *view, const Buffer *buffer, i64 direction, i32 viewport_height) {
  if (viewport_height <= 0) return;
  // Vim keeps two lines of overlap between pages for continuity.
  i64 amount = Max((i64)viewport_height - 2, (i64)1) * ((direction >= 0) ? 1 : -1);

  u64 cursor_line = ViewCursorLine(view, buffer);
  i64 target_line = (i64)cursor_line + amount;
  u64 line_count = BufferLineCount(buffer);
  u64 clamped = (target_line < 0) ? 0 : Min((u64)target_line, line_count - 1);

  ViewSetCursorLineColumn(view, buffer, clamped, view->preferred_column);

  i64 scroll_target = (i64)view->scroll_line + amount;
  view->scroll_line = (scroll_target > 0) ? (u64)scroll_target : 0;
  ViewClampScroll(view, buffer, viewport_height);
  ViewScrollToCursor(view, buffer, 1 << 20, viewport_height);
}

void ViewCenterOnCursor(View *view, const Buffer *buffer, i32 viewport_height) {
  if (viewport_height <= 0) return;
  u64 cursor_line = ViewCursorLine(view, buffer);
  u64 half = (u64)viewport_height / 2;
  view->scroll_line = (cursor_line > half) ? cursor_line - half : 0;
  ViewClampScroll(view, buffer, viewport_height);
}

void ViewCursorLineToTop(View *view, const Buffer *buffer, i32 viewport_height) {
  view->scroll_line = ViewCursorLine(view, buffer);
  ViewClampScroll(view, buffer, viewport_height);
}

void ViewCursorLineToBottom(View *view, const Buffer *buffer, i32 viewport_height) {
  if (viewport_height <= 0) return;
  u64 cursor_line = ViewCursorLine(view, buffer);
  u64 height = (u64)viewport_height - 1;
  view->scroll_line = (cursor_line > height) ? cursor_line - height : 0;
  ViewClampScroll(view, buffer, viewport_height);
}

RangeU64 ViewVisibleLines(const View *view, const Buffer *buffer, i32 viewport_height) {
  u64 line_count = BufferLineCount(buffer);
  u64 first = Min(view->scroll_line, line_count);
  u64 last = (viewport_height > 0) ? first + (u64)viewport_height : first;
  return RangeU64{first, Min(last, line_count)};
}

bool ViewIsCursorVisible(const View *view, const Buffer *buffer, i32 viewport_width,
                         i32 viewport_height) {
  if (viewport_height <= 0 || viewport_width <= 0) return false;

  u64 line = ViewCursorLine(view, buffer);
  u64 column = ViewCursorColumn(view, buffer);

  bool vertical = line >= view->scroll_line &&
                  line < view->scroll_line + (u64)viewport_height;
  bool horizontal = column >= view->scroll_column &&
                    column < view->scroll_column + (u64)viewport_width;
  return vertical && horizontal;
}

u64 ViewMaxVisibleLineLength(const View *view, const Buffer *buffer, i32 viewport_height) {
  RangeU64 lines = ViewVisibleLines(view, buffer, viewport_height);
  u64 longest = 0;
  for (u64 line = lines.min; line < lines.max; line += 1) {
    longest = Max(longest, LineColumnCount(buffer, line));
  }
  return longest;
}
