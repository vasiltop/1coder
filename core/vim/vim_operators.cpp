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
        }
      }
      break;
    }
    case MotionKind::Inclusive:
      // The character at the far end is part of the span, so step past it.
      range.max = Min(BufferNextCodepoint(buffer, range.max), BufferSize(buffer));
      break;
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
  if (op != OperatorKind::Yank && BufferEditBlocked(buffer, range)) return view->cursor;

  // Neovim leaves 'startofline' off, so a linewise delete keeps the column
  // rather than jumping to the first non-blank.
  u64 column = BufferColumnFromOffset(buffer, view->cursor);

  switch (op) {
    case OperatorKind::Yank: {
      VimYankRange(ed, view, buffer, range, linewise, true);
      // Yank leaves the cursor at the start of the yanked span.
      return range.min;
    }

    case OperatorKind::Delete: {
      VimYankRange(ed, view, buffer, range, linewise);
      BufferDelete(ed, buffer, range, view->cursor, range.min);

      if (linewise) {
        // The cursor lands on whichever line moved up into the deleted one,
        // keeping its column.
        u64 line = Min(BufferLineFromOffset(buffer, range.min), BufferLineCount(buffer) - 1);
        return BufferOffsetFromColumn(buffer, line, column);
      }
      return range.min;
    }

    case OperatorKind::Change: {
      VimYankRange(ed, view, buffer, range, linewise);

      if (linewise) {
        // `cc` empties the lines but keeps one to type on, rather than
        // removing them outright as `dd` would.
        RangeU64 without_newline = range;
        u64 last_line = BufferLineFromOffset(buffer, (range.max > range.min) ? range.max - 1
                                                                            : range.min);
        without_newline.max = BufferLineEnd(buffer, last_line);
        BufferDelete(ed, buffer, without_newline, view->cursor, without_newline.min);
        return without_newline.min;
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
  if (reg.text.size == 0) return pos;

  TempArena scratch = ScratchBegin();

  // Repeat the register's contents `count` times before inserting, so 3p is a
  // single undoable edit rather than three.
  String8List parts = {};
  for (u64 i = 0; i < count; i += 1) Str8ListPush(scratch.arena, &parts, reg.text);
  String8 text = Str8ListJoin(scratch.arena, &parts, String8{nullptr, 0});

  // Linewise content has to end in a newline or it would run into the line it
  // lands next to. `yy` on a final line with no newline of its own produces
  // exactly that case.
  if (reg.linewise && text.size > 0 && text.str[text.size - 1] != '\n') {
    text = PushStr8Cat(scratch.arena, text, Str8Lit("\n"));
  }

  u64 cursor = pos;

  if (reg.linewise) {
    // Linewise content goes onto its own line, above or below the current one.
    u64 line = BufferLineFromOffset(buffer, pos);
    u64 at = after ? LineRangeWithNewline(&buffer->lines, &buffer->text, line).max
                   : BufferOffsetFromLine(buffer, line);

    if (BufferEditBlocked(buffer, RangeU64{at, at})) {
      ScratchEnd(scratch);
      return pos;
    }

    // A final line with no newline of its own needs one adding first, or the
    // pasted text would join onto it.
    if (after && at > 0 && BufferByteAt(buffer, at - 1) != '\n') {
      BufferInsert(ed, buffer, at, Str8Lit("\n"), pos, at + 1);
      at += 1;
    }

    BufferInsert(ed, buffer, at, text, pos, at);
    cursor = at;
  } else {
    // `p` pastes after the character under the cursor -- but on an empty line
    // there is no such character, and stepping forward would cross the newline
    // and land the text on the following line.
    u64 line_end = BufferLineEnd(buffer, BufferLineFromOffset(buffer, pos));
    u64 at = after ? Min(BufferNextCodepoint(buffer, pos), line_end) : pos;
    if (BufferEditBlocked(buffer, RangeU64{at, at})) {
      ScratchEnd(scratch);
      return pos;
    }
    // Characterwise paste leaves the cursor on the last pasted character.
    BufferInsert(ed, buffer, at, text, pos, at + text.size);
    cursor = (text.size > 0) ? at + text.size - 1 : at;
  }

  ScratchEnd(scratch);
  return cursor;
}

u64 VimIndentLines(Editor *ed, View *view, Buffer *buffer, RangeU64 lines, bool indent) {
  if (BufferEditBlocked(buffer, lines)) return view->cursor;

  u64 column = BufferColumnFromOffset(buffer, view->cursor);
  u64 first = BufferLineFromOffset(buffer, lines.min);
  u64 last = BufferLineFromOffset(buffer, (lines.max > lines.min) ? lines.max - 1 : lines.min);

  TempArena scratch = ScratchBegin();
  String8 shift = PushStr8F(scratch.arena, "%*s", (int)kShiftWidth, "");

  // One undo step for the whole operation, however many lines it spans.
  BufferBeginEditGroup(buffer);

  // Work bottom-up so each edit cannot shift the offsets of lines not yet
  // visited.
  for (u64 i = last + 1; i > first; i -= 1) {
    u64 line = i - 1;
    RangeU64 range = BufferLineRange(buffer, line);

    if (indent) {
      if (RangeSize(range) == 0) continue;  // leave blank lines alone
      BufferInsert(ed, buffer, range.min, shift, view->cursor, view->cursor);
    } else {
      // Remove up to one shift width of leading whitespace.
      u64 removed = 0;
      u64 p = range.min;
      while (removed < kShiftWidth && p < range.max) {
        u8 c = BufferByteAt(buffer, p);
        if (c == ' ') { p += 1; removed += 1; }
        else if (c == '\t') { p += 1; removed = kShiftWidth; }
        else break;
      }
      if (p > range.min) {
        BufferDelete(ed, buffer, RangeU64{range.min, p}, view->cursor, view->cursor);
      }
    }
  }

  BufferEndEditGroup(buffer);
  ScratchEnd(scratch);

  // Keep the column, matching neovim's 'nostartofline'.
  return BufferOffsetFromColumn(buffer, first, column);
}

u64 VimJoinLines(Editor *ed, View *view, Buffer *buffer, u64 pos, u64 count) {
  BufferBeginEditGroup(buffer);

  u64 cursor = pos;
  u64 joins = Max(count, (u64)1);

  for (u64 i = 0; i < joins; i += 1) {
    u64 line = BufferLineFromOffset(buffer, cursor);
    if (line + 1 >= BufferLineCount(buffer)) break;

    u64 end = BufferLineEnd(buffer, line);
    u64 next_start = BufferOffsetFromLine(buffer, line + 1);

    // Swallow the newline plus the next line's leading whitespace, and put a
    // single space in their place.
    u64 content = next_start;
    RangeU64 next_range = BufferLineRange(buffer, line + 1);
    while (content < next_range.max && CharIsSpace(BufferByteAt(buffer, content))) {
      content = BufferNextCodepoint(buffer, content);
    }

    RangeU64 replace = RangeU64{end, content};
    if (BufferEditBlocked(buffer, replace)) break;

    // No separator is added when the line is empty or already ends in one.
    bool need_space = (end > BufferOffsetFromLine(buffer, line)) && (content < next_range.max);
    String8 separator = need_space ? Str8Lit(" ") : String8{nullptr, 0};

    BufferReplace(ed, buffer, replace, separator, cursor, end);
    cursor = end;
  }

  BufferEndEditGroup(buffer);
  return cursor;
}
