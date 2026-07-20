#pragma once

#include "editor/buffer.h"
#include "editor/view.h"
#include "vim/vim_motions.h"
#include "vim/vim_state.h"

struct Editor;

// Operators act on a range. Since motions produce ranges and text objects
// produce ranges, one implementation of each operator serves every one of them.

// Turns a motion's outcome into the span an operator should act on. Inclusive
// motions extend one character past the target; linewise ones expand to cover
// whole lines.
[[nodiscard]] RangeU64 VimRangeFromMotion(const Buffer *buffer, u64 from, MotionResult motion);

// Whether a motion's result should be treated as whole lines.
[[nodiscard]] inline bool VimMotionIsLinewise(MotionResult motion) {
  return motion.kind == MotionKind::Linewise;
}

// Applies an operator, recording the affected text into a register and leaving
// the cursor where vim would. Returns the offset the cursor should move to.
u64 VimApplyOperator(Editor *ed, View *view, Buffer *buffer, OperatorKind op, RangeU64 range,
                     bool linewise);

// Captures a span into whichever register `view` has selected -- including the
// system clipboard via "+ or "*. `from_yank` distinguishes a real yank from the
// capture a delete performs, because vim's "0 holds the last yank only, which
// is what makes `"0p` still work after something has been deleted.
void VimYankRange(Editor *ed, View *view, Buffer *buffer, RangeU64 range, bool linewise,
                  bool from_yank = false);

// Pastes the unnamed register. Linewise content lands on its own line, which is
// why the register remembers how it was captured.
u64 VimPaste(Editor *ed, View *view, Buffer *buffer, u64 pos, u64 count, bool after);
u64 VimPasteRegister(Editor *ed, View *view, Buffer *buffer, Register reg, u64 pos, u64 count,
                     bool after);
u64 VimInsertRegister(Editor *ed, View *view, Buffer *buffer, u64 pos, Register reg);
u64 VimReplaceVisual(Editor *ed, View *view, Buffer *buffer, Register replacement);

// Clean Neovim defaults (-u NONE): noexpandtab, tabstop=8, shiftwidth=8.
// Indent inserts one tab; dedent removes one tabstop worth of visual columns.
inline constexpr u64 kTabStop = 8;
// Used for insert-mode <Tab> (spaces per tab press, separate from kTabStop).
inline constexpr u64 kShiftWidth = 2;
u64 VimIndentLines(Editor *ed, View *view, Buffer *buffer, RangeU64 lines, bool indent);

// Joins the line below onto the current one, collapsing the whitespace between
// them to a single space as vim does.
u64 VimJoinLines(Editor *ed, View *view, Buffer *buffer, u64 pos, u64 count);
