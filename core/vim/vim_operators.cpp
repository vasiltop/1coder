#include "vim/vim_operators.h"

#include "editor/editor.h"

namespace {

// Expands a range to cover every line it touches, including the final newline.
[[nodiscard]] RangeU64 ExpandToLines(const Buffer *buffer, RangeU64 range) {
  u64 first = BufferLineFromOffset(buffer, range.min);
  u64 last = BufferLineFromOffset(buffer, (range.max > range.min) ? range.max - 1 : range.min);
  return RangeU64{BufferOffsetFromLine(buffer, first),
                  LineRangeWithNewline(&buffer->lines, &buffer->text, last).max};
}

}  // namespace

RangeU64 VimRangeFromMotion(const Buffer *buffer, u64 from, MotionResult motion) {
  RangeU64 range = RangeMake(from, motion.target);

  switch (motion.kind) {
    case MotionKind::Exclusive: {
      // Vim's exclusive-motion adjustment: an exclusive motion ending at the
      // start of a line instead ends at the end of the line before it. Without
      // this, `w` on the last word of a line steps onto the next line and so
      // `dw` and `yw` swallow the line break -- which then reappears as a
      // stray newline when the yank is pasted.
      if (range.max > range.min) {
        u64 line = BufferLineFromOffset(buffer, range.max);
        u64 line_start = BufferOffsetFromLine(buffer, line);

        if (range.max == line_start && line > 0) {
          u64 previous_end = BufferLineEnd(buffer, line - 1);
          if (previous_end > range.min) range.max = previous_end;
        } else {
          // When `w` lands partway into the next line (not at its very start),
          // clip the range to the end of the source line so `dw` never deletes
          // a newline or content from the following line.
          u64 from_line = BufferLineFromOffset(buffer, range.min);
          if (line > from_line) {
            u64 from_end = BufferLineEnd(buffer, from_line);
            range.max = (from_end > range.min) ? from_end : range.min;
          }
        }
      }
      break;
    }
    case MotionKind::Inclusive: {
      // The character at the far end is part of the span, so step past it,
      // clamped to the line boundary so inclusive motions cannot cross newlines.
      u64 next = BufferNextCodepoint(buffer, range.max);
      u64 line_nl = BufferLineEnd(buffer, BufferLineFromOffset(buffer, range.max));
      range.max = (next <= line_nl) ? next : line_nl;
      break;
    }
    case MotionKind::Linewise: {
      // Take the lines the two *endpoints* sit on, rather than expanding the
      // byte range. When the target lands exactly on a line start, the byte
      // before it belongs to the previous line, which would drop the
      // destination line entirely.
      u64 from_line = BufferLineFromOffset(buffer, from);
      u64 to_line = BufferLineFromOffset(buffer, motion.target);
      u64 first = Min(from_line, to_line);
      u64 last = Max(from_line, to_line);
      range = RangeU64{BufferOffsetFromLine(buffer, first),
                       LineRangeWithNewline(&buffer->lines, &buffer->text, last).max};
      break;
    }
  }
  return range;
}

void VimYankRange(Editor *ed, View *view, Buffer *buffer, RangeU64 range, bool linewise,
                  bool from_yank) {
  TempArena scratch = ScratchBegin();
  String8 text = BufferTextRange(scratch.arena, buffer, range);
  if (linewise && text.size == 0 && (from_yank || buffer->final_newline)) text = Str8Lit("\n");

  u8 name = RegisterNormalise(view->vim.pending_register);
  EditorSetRegister(ed, name, text, linewise);

  // Vim fills the unnamed register alongside a named one, so a bare `p` still
  // pastes what was just captured.
  if (name != kRegisterUnnamed) EditorSetRegister(ed, kRegisterUnnamed, text, linewise);

  // "0 survives deletes, so a yank can still be pasted after one.
  if (from_yank) EditorSetRegister(ed, kRegisterYank, text, linewise);

  ScratchEnd(scratch);
}

u64 VimApplyOperator(Editor *ed, View *view, Buffer *buffer, OperatorKind op, RangeU64 range,
                     bool linewise) {
  if (BufferIsReadOnly(buffer) && op != OperatorKind::Yank) return view->cursor;

  // Neovim leaves 'startofline' off, so a linewise delete keeps the column
  // rather than jumping to the first non-blank.
  u64 column = BufferColumnFromOffset(buffer, view->cursor);

  switch (op) {
    case OperatorKind::Yank: {
      VimYankRange(ed, view, buffer, range, linewise, true);
      return range.min;
    }

    case OperatorKind::Delete: {
      u64 size_before = BufferSize(buffer);
      BufferBeginEditGroup(buffer);
      VimYankRange(ed, view, buffer, range, linewise);

      u64 del_start_line = BufferLineFromOffset(buffer, range.min);
      u64 del_end_line   = (range.max > range.min)
                               ? BufferLineFromOffset(buffer, range.max - 1)
                               : del_start_line;
      bool cross_line    = linewise || del_end_line > del_start_line;

      BufferDelete(ed, buffer, range, view->cursor, range.min);

      if (BufferSize(buffer) == 0 && (cross_line || size_before > 0))
        BufferSetFinalNewline(buffer, !cross_line);

      BufferEndEditGroup(buffer);

      if (linewise) {
        u64 line = Min(BufferLineFromOffset(buffer, range.min), BufferLineCount(buffer) - 1);
        return BufferOffsetFromColumn(buffer, line, column);
      }
      return range.min;
    }

    case OperatorKind::Change: {
      VimYankRange(ed, view, buffer, range, linewise);

      if (linewise) {
        // `cc` empties the lines but keeps one to type on; leading whitespace
        // is preserved so the cursor enters insert at the current indent level.
        RangeU64 without_newline = range;
        u64 last_line = BufferLineFromOffset(buffer, (range.max > range.min) ? range.max - 1
                                                                            : range.min);
        without_newline.max = BufferLineEnd(buffer, last_line);

        u64 first_non_blank = without_newline.min;
        while (first_non_blank < without_newline.max) {
          u8 c = BufferByteAt(buffer, first_non_blank);
          if (c != ' ' && c != '\t') break;
          first_non_blank++;
        }

        RangeU64 to_delete = {first_non_blank, without_newline.max};
        BufferDelete(ed, buffer, to_delete, view->cursor, first_non_blank);
        return first_non_blank;
      }

      BufferDelete(ed, buffer, range, view->cursor, range.min);
      return range.min;
    }

    case OperatorKind::Indent:
    case OperatorKind::Dedent: {
      RangeU64 lines = ExpandToLines(buffer, range);
      return VimIndentLines(ed, view, buffer, lines, op == OperatorKind::Indent);
    }

    default:
      return view->cursor;
  }
}

u64 VimPaste(Editor *ed, View *view, Buffer *buffer, u64 pos, u64 count, bool after) {
  Register reg = EditorGetRegister(ed, RegisterNormalise(view->vim.pending_register));
  if (reg.text.size == 0 || BufferIsReadOnly(buffer)) return pos;

  // One undo step for the whole paste, even when a separator needs inserting.
  BufferBeginEditGroup(buffer);

  TempArena scratch = ScratchBegin();

  // Expand `count` copies now so the whole paste is one undoable edit.
  // Linewise pieces each need a trailing newline to stay on separate lines.
  String8 piece = reg.text;
  if (reg.linewise && piece.size > 0 && piece.str[piece.size - 1] != '\n') {
    piece = PushStr8Cat(scratch.arena, piece, Str8Lit("\n"));
  }
  String8List parts = {};
  for (u64 i = 0; i < count; i += 1) Str8ListPush(scratch.arena, &parts, piece);
  String8 text = Str8ListJoin(scratch.arena, &parts, String8{nullptr, 0});

  u64 cursor = pos;

  if (reg.linewise) {
    // Linewise content goes onto its own line, above or below the current one.
    u64 line = BufferLineFromOffset(buffer, pos);
    u64 at = after ? LineRangeWithNewline(&buffer->lines, &buffer->text, line).max
                   : BufferOffsetFromLine(buffer, line);

    // A final line with no trailing newline needs a separator, or the pasted
    // text would join onto it.
    if (after) {
      bool needs_sep = (at > 0 && BufferByteAt(buffer, at - 1) != '\n')
                    || (at == 0 && BufferSize(buffer) == 0);
      if (needs_sep) {
        BufferInsert(ed, buffer, at, Str8Lit("\n"), pos, at + 1);
        at += 1;
      }
    }

    // Strip trailing '\n' when appending at end of buffer; the save layer adds
    // one and paste-before keeps its own to represent the following blank line.
    if (after && at == BufferSize(buffer) && text.size > 0 && text.str[text.size - 1] == '\n') {
      text = Str8Chop(text, 1);
    }

    BufferInsert(ed, buffer, at, text, pos, at);
    cursor = at;

    // Land on the first non-blank character of the newly pasted line.
    u64 pasted_line = BufferLineFromOffset(buffer, cursor);
    u64 line_end = BufferLineEnd(buffer, pasted_line);
    while (cursor < line_end) {
      u8 c = BufferByteAt(buffer, cursor);
      if (c != ' ' && c != '\t') break;
      cursor += 1;
    }
  } else {
    // `p` pastes after the character under the cursor -- but on an empty line
    // there is no such character, and stepping forward would cross the newline
    // and land the text on the following line.
    u64 line_end = BufferLineEnd(buffer, BufferLineFromOffset(buffer, pos));
    u64 at = after ? Min(BufferNextCodepoint(buffer, pos), line_end) : pos;
    BufferInsert(ed, buffer, at, text, pos, at + text.size);

    // Cursor lands on the last codepoint for plain text; at the insertion
    // point when the text contains a newline.
    if (text.size == 0) {
      cursor = at;
    } else {
      bool has_newline = false;
      for (u64 k = 0; k < text.size; k += 1) {
        if (text.str[k] == '\n') { has_newline = true; break; }
      }
      if (!has_newline) {
        cursor = BufferPrevCodepoint(buffer, at + text.size);
      } else if (after && text.str[text.size - 1] == '\n') {
        cursor = at;
      } else {
        cursor = at;
      }
    }
  }

  ScratchEnd(scratch);
  BufferEndEditGroup(buffer);
  return cursor;
}

u64 VimIndentLines(Editor *ed, View *view, Buffer *buffer, RangeU64 lines, bool indent) {
  if (BufferIsReadOnly(buffer)) return view->cursor;

  u64 first = BufferLineFromOffset(buffer, lines.min);
  u64 last = BufferLineFromOffset(buffer, (lines.max > lines.min) ? lines.max - 1 : lines.min);

  // Capture cursor codepoint-column before any edits.
  u64 old_col = BufferColumnFromOffset(buffer, view->cursor);

  // <<: record cursor's end virtual-column before any edits to restore position after dedent.
  u64 old_end_vcol = 0;
  if (!indent) {
    u64 cur_line = BufferLineFromOffset(buffer, view->cursor);
    RangeU64 clr = BufferLineRange(buffer, cur_line);
    u64 vcol = 0;
    for (u64 p = clr.min; p < clr.max; p = BufferNextCodepoint(buffer, p)) {
      u8 c = BufferByteAt(buffer, p);
      vcol += (c == '\t') ? ((vcol / kTabStop + 1) * kTabStop - vcol) : 1;
      if (p == view->cursor) { old_end_vcol = vcol; break; }
    }
    if (old_end_vcol == 0) old_end_vcol = vcol + 1;
  }

  // For >>: count leading-whitespace codepoints on the first line so we can
  // compute min(old_col, old_first_nonblank) after the tab is prepended.
  u64 old_first_nonblank = 0;
  if (indent) {
    RangeU64 fr = BufferLineRange(buffer, first);
    for (u64 p = fr.min; p < fr.max; p++) {
      u8 c = BufferByteAt(buffer, p);
      if (c != ' ' && c != '\t') break;
      old_first_nonblank++;
    }
  }

  TempArena scratch = ScratchBegin();
  BufferBeginEditGroup(buffer);

  for (u64 i = last + 1; i > first; i -= 1) {
    u64 line = i - 1;
    RangeU64 range = BufferLineRange(buffer, line);

    if (indent) {
      if (RangeSize(range) == 0) continue;
      BufferInsert(ed, buffer, range.min, Str8Lit("\t"), view->cursor, view->cursor);
    } else {
      // Count visual columns of leading whitespace (tab stops at kTabStop).
      u64 vcols = 0;
      u64 ws_end = range.min;
      while (ws_end < range.max) {
        u8 c = BufferByteAt(buffer, ws_end);
        if (c == '\t') { vcols = (vcols / kTabStop + 1) * kTabStop; ws_end++; }
        else if (c == ' ') { vcols++; ws_end++; }
        else break;
      }
      u64 new_vcols = vcols > kTabStop ? vcols - kTabStop : 0;
      u64 new_tabs  = new_vcols / kTabStop;
      u64 new_spc   = new_vcols % kTabStop;
      u8 *prefix = PushArrayNoZero(scratch.arena, u8, new_tabs + new_spc + 1);
      for (u64 j = 0; j < new_tabs; j++) prefix[j] = '\t';
      for (u64 j = 0; j < new_spc;  j++) prefix[new_tabs + j] = ' ';
      BufferReplace(ed, buffer, RangeU64{range.min, ws_end},
                    String8{prefix, new_tabs + new_spc}, view->cursor, view->cursor);
    }
  }

  BufferEndEditGroup(buffer);
  ScratchEnd(scratch);

  if (indent) {
    // Cursor sits at min(old_col, old_first_nonblank) on the first line.
    return BufferOffsetFromColumn(buffer, first, Min(old_col, old_first_nonblank));
  }

  // Land on the last codepoint whose end virtual-column fits within old_end_vcol.
  {
    RangeU64 fr = BufferLineRange(buffer, first);
    u64 best = fr.min;
    u64 vcol = 0;
    for (u64 p = fr.min; p < fr.max; p = BufferNextCodepoint(buffer, p)) {
      u8 c = BufferByteAt(buffer, p);
      u64 w = (c == '\t') ? ((vcol / kTabStop + 1) * kTabStop - vcol) : 1;
      if (vcol + w > old_end_vcol) break;
      vcol += w;
      best = p;
    }
    return best;
  }
}

u64 VimJoinLines(Editor *ed, View *view, Buffer *buffer, u64 pos, u64 count) {
  if (BufferIsReadOnly(buffer)) return pos;

  BufferBeginEditGroup(buffer);

  u64 cursor = pos;
  // Neovim: bare J or 1J == 2J (one join); NJ joins N lines (N-1 joins).
  u64 joins = count <= 1 ? 1 : count - 1;

  for (u64 i = 0; i < joins; i += 1) {
    u64 line = BufferLineFromOffset(buffer, cursor);
    if (line + 1 >= BufferLineCount(buffer)) break;

    u64 end = BufferLineEnd(buffer, line);
    u64 next_start = BufferOffsetFromLine(buffer, line + 1);

    u64 content = next_start;
    RangeU64 next_range = BufferLineRange(buffer, line + 1);
    while (content < next_range.max && CharIsSpace(BufferByteAt(buffer, content))) {
      content = BufferNextCodepoint(buffer, content);
    }

    u64 line_start = BufferOffsetFromLine(buffer, line);
    bool ends_in_space = end > line_start && CharIsSpace(BufferByteAt(buffer, end - 1));
    bool need_space = end > line_start && content < next_range.max && !ends_in_space;
    String8 separator = need_space ? Str8Lit(" ") : String8{nullptr, 0};

    BufferReplace(ed, buffer, RangeU64{end, content}, separator, cursor, end);
    cursor = end;
  }

  BufferEndEditGroup(buffer);
  return cursor;
}
