#pragma once

#include "base/base_math.h"
#include "editor/buffer.h"
#include "vim/vim_jumplist.h"
#include "vim/vim_state.h"

// A viewport onto a buffer: where the cursor is, what is scrolled into sight,
// and what mode this window is in. Text lives in the buffer; position lives
// here, which is what lets two windows show the same file independently.
//
// Scrolling is core state, not a rendering detail. Both axes are measured in
// cells -- lines down, codepoint columns across -- so the "keep the cursor on
// screen" logic is plain arithmetic that can be tested without a window. The
// renderer only multiplies these by glyph metrics.

// The most cursors a view will carry, placement marks included. A fixed inline
// array rather than an allocation, matching the jump list, so a View stays
// trivially resettable by `*view = View{}`.
inline constexpr u64 kMaxCursors = 128;

// One of a view's several cursors. The fields mirror the primary cursor's on
// the View, because fanning a command out works by swapping one of these into
// those fields and running the ordinary single-cursor code.
struct Cursor {
  u64 offset;
  u64 preferred_column;
  u64 anchor;  // visual anchor, as vim.visual_anchor is for the primary
};

struct View {
  BufferHandle buffer;

  // The primary cursor. A view with no secondary cursors is the ordinary case
  // and every command reads and writes these fields directly; multi-cursor is
  // built on top by swapping each secondary through them in turn.
  u64 cursor;  // byte offset

  // Column the cursor "wants". Moving down a short line and back onto a long
  // one returns to this column rather than staying where it was clamped, which
  // is what makes j/k feel right.
  u64 preferred_column;

  // Top-left of the visible region.
  u64 scroll_line;
  u64 scroll_column;

  VimState vim;

  // Secondary cursors. Empty in the single-cursor case, which is what lets
  // every existing command stay written against the primary alone.
  Cursor extras[kMaxCursors];
  u64 extra_count;

  // Positions marked while placing cursors, before they are confirmed. Kept
  // apart from `extras` so an abandoned placement leaves nothing behind, and so
  // the renderer can show marks in a shape that reads as "not live yet".
  u64 pending[kMaxCursors];
  u64 pending_count;
  bool placing;

  // Per-window jump list for <C-o> / <C-i>.
  JumpList jumps;
};

void ViewInit(View *view, BufferHandle buffer);

// Cursor movement. These clamp to the buffer and never land inside a multi-byte
// character.
void ViewSetCursor(View *view, const Buffer *buffer, u64 offset);
// For vertical motion, which must preserve the remembered column.
void ViewSetCursorKeepColumn(View *view, const Buffer *buffer, u64 offset);
void ViewSetCursorLineColumn(View *view, const Buffer *buffer, u64 line, u64 column);

[[nodiscard]] u64 ViewCursorLine(const View *view, const Buffer *buffer);
[[nodiscard]] u64 ViewCursorColumn(const View *view, const Buffer *buffer);

// In normal mode the cursor sits *on* a character rather than between two, so
// it may not rest on the newline at the end of a line. Insert and visual mode
// allow one past the end.
[[nodiscard]] u64 ViewClampCursorToMode(const View *view, const Buffer *buffer, u64 offset);

// Clamps into the buffer while always permitting the position just past a
// line's last character. Multi-cursor uses this rather than the mode-aware
// clamp: a cursor set may deliberately hold end-of-line positions, since
// putting one at the end of one line and one at the start of another is exactly
// what several cursors are for.
[[nodiscard]] u64 ViewClampCursorAllowLineEnd(const Buffer *buffer, u64 offset);

// The selected span in visual mode, empty otherwise. Linewise selection expands
// to whole lines.
[[nodiscard]] RangeU64 ViewSelection(const View *view, const Buffer *buffer);

// The same, for a cursor other than the primary. The mode is a property of the
// view, but the anchor is per-cursor, so several cursors in visual mode each
// select their own span.
[[nodiscard]] RangeU64 ViewSelectionFor(const View *view, const Buffer *buffer, u64 cursor,
                                        u64 anchor);

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

// Lines of context kept above and below the cursor where possible.
// Matches 'scrolloff' in the user's nvim config.
inline constexpr i32 kDefaultScrollOff = 4;
// Columns of context kept left and right of the cursor.
inline constexpr i32 kDefaultSideScrollOff = 8;

// Brings the cursor into view, moving as little as possible. Call after any
// cursor movement. `viewport` is the text area in cells, excluding chrome such
// as the status line.
void ViewScrollToCursor(View *view, const Buffer *buffer, i32 viewport_width,
                        i32 viewport_height, i32 scroll_off = kDefaultScrollOff,
                        i32 side_scroll_off = kDefaultSideScrollOff);

// Scrolls the view without moving the cursor, then drags the cursor along only
// if it would otherwise leave the screen -- vim's ctrl-e / ctrl-y behaviour.
void ViewScrollLines(View *view, const Buffer *buffer, i64 delta, i32 viewport_height);

// Scrolls horizontally. Negative moves left.
void ViewScrollColumns(View *view, const Buffer *buffer, i64 delta, i32 viewport_width);

// Moves the cursor by a screenful and scrolls to match: ctrl-d / ctrl-u.
void ViewScrollHalfPage(View *view, const Buffer *buffer, i64 direction, i32 viewport_height);
void ViewScrollFullPage(View *view, const Buffer *buffer, i64 direction, i32 viewport_height);

// Puts the cursor's line at the centre, top or bottom of the viewport: zz, zt, zb.
void ViewCenterOnCursor(View *view, const Buffer *buffer, i32 viewport_height);
void ViewCursorLineToTop(View *view, const Buffer *buffer, i32 viewport_height);
void ViewCursorLineToBottom(View *view, const Buffer *buffer, i32 viewport_height);

// Keeps scroll within the buffer's extent. Called after edits, which can shrink
// the buffer out from under a scrolled-down view.
void ViewClampScroll(View *view, const Buffer *buffer, i32 viewport_height);

// The line range the renderer should draw, already clipped to the buffer.
[[nodiscard]] RangeU64 ViewVisibleLines(const View *view, const Buffer *buffer,
                                        i32 viewport_height);

[[nodiscard]] bool ViewIsCursorVisible(const View *view, const Buffer *buffer,
                                       i32 viewport_width, i32 viewport_height);

// Longest line in the visible region, in codepoints -- what a horizontal
// scrollbar needs to size itself.
[[nodiscard]] u64 ViewMaxVisibleLineLength(const View *view, const Buffer *buffer,
                                           i32 viewport_height);
