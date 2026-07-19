#include "vim/vim_motions.h"

namespace {

// Vim's three character classes. A word boundary is any transition between
// them, which is what makes `w` stop between "foo" and "(".
enum class CharClass : u8 { Blank, Word, Punct };

[[nodiscard]] CharClass ClassOf(u8 c) {
  if (c == 0 || CharIsSpace(c)) return CharClass::Blank;
  if (CharIsWord(c)) return CharClass::Word;
  return CharClass::Punct;
}

// WORD motions collapse the distinction between word and punctuation, leaving
// only blank versus non-blank.
[[nodiscard]] CharClass ClassOfBig(u8 c) {
  return (c == 0 || CharIsSpace(c)) ? CharClass::Blank : CharClass::Word;
}

[[nodiscard]] MotionResult Ok(u64 target, MotionKind kind) {
  return MotionResult{target, kind, true};
}

[[nodiscard]] MotionResult Fail(u64 pos, MotionKind kind) {
  return MotionResult{pos, kind, false};
}

[[nodiscard]] bool IsNewline(const Buffer *b, u64 pos) {
  return BufferByteAt(b, pos) == '\n';
}

// One step of `w`: past the current run, then over any blanks.
[[nodiscard]] u64 StepWordForward(const Buffer *b, u64 pos, bool big) {
  u64 size = BufferSize(b);
  if (pos >= size) return size;

  auto classify = big ? ClassOfBig : ClassOf;
  CharClass start = classify(BufferByteAt(b, pos));

  if (start != CharClass::Blank) {
    while (pos < size && classify(BufferByteAt(b, pos)) == start) {
      pos = BufferNextCodepoint(b, pos);
    }
  }
  while (pos < size && classify(BufferByteAt(b, pos)) == CharClass::Blank) {
    pos = BufferNextCodepoint(b, pos);
  }
  return pos;
}

[[nodiscard]] u64 StepWordBackward(const Buffer *b, u64 pos, bool big) {
  if (pos == 0) return 0;

  auto classify = big ? ClassOfBig : ClassOf;

  pos = BufferPrevCodepoint(b, pos);
  while (pos > 0 && classify(BufferByteAt(b, pos)) == CharClass::Blank) {
    pos = BufferPrevCodepoint(b, pos);
  }
  if (classify(BufferByteAt(b, pos)) == CharClass::Blank) return pos;

  CharClass target = classify(BufferByteAt(b, pos));
  while (pos > 0) {
    u64 prev = BufferPrevCodepoint(b, pos);
    if (classify(BufferByteAt(b, prev)) != target) break;
    pos = prev;
  }
  return pos;
}

[[nodiscard]] u64 StepWordEnd(const Buffer *b, u64 pos, bool big) {
  u64 size = BufferSize(b);
  if (pos >= size) return size;

  auto classify = big ? ClassOfBig : ClassOf;

  // `e` always moves at least one character, so it does not stick on the end
  // of the word it starts on.
  pos = BufferNextCodepoint(b, pos);
  while (pos < size && classify(BufferByteAt(b, pos)) == CharClass::Blank) {
    pos = BufferNextCodepoint(b, pos);
  }
  if (pos >= size) return size;

  CharClass target = classify(BufferByteAt(b, pos));
  while (pos < size) {
    u64 next = BufferNextCodepoint(b, pos);
    if (next >= size || classify(BufferByteAt(b, next)) != target) break;
    pos = next;
  }
  return pos;
}

// Searches within the cursor's line only, as f/F/t/T do.
[[nodiscard]] u64 FindCharOnLine(const Buffer *b, u64 pos, u32 target, u64 count, bool forward,
                                 bool *found) {
  u64 line = BufferLineFromOffset(b, pos);
  RangeU64 range = BufferLineRange(b, line);
  *found = false;

  u64 at = pos;
  for (u64 i = 0; i < count; i += 1) {
    bool hit = false;
    if (forward) {
      for (u64 p = BufferNextCodepoint(b, at); p < range.max; p = BufferNextCodepoint(b, p)) {
        if (BufferByteAt(b, p) == (u8)target) {
          at = p;
          hit = true;
          break;
        }
      }
    } else {
      for (u64 p = at; p > range.min;) {
        p = BufferPrevCodepoint(b, p);
        if (BufferByteAt(b, p) == (u8)target) {
          at = p;
          hit = true;
          break;
        }
        if (p == range.min) break;
      }
    }
    if (!hit) return pos;
  }

  *found = true;
  return at;
}

[[nodiscard]] bool LineIsBlank(const Buffer *b, u64 line) {
  RangeU64 range = BufferLineRange(b, line);
  for (u64 p = range.min; p < range.max; p += 1) {
    if (!CharIsSpace(BufferByteAt(b, p))) return false;
  }
  return true;
}

}  // namespace

MotionResult MotionCharLeft(const Buffer *b, const View *v, u64 pos, u64 count, u32) {
  u64 line_start = BufferOffsetFromLine(b, BufferLineFromOffset(b, pos));
  u64 at = pos;
  // h stops at the start of the line rather than wrapping to the previous one.
  for (u64 i = 0; i < count && at > line_start; i += 1) at = BufferPrevCodepoint(b, at);
  return (at == pos) ? Fail(pos, MotionKind::Exclusive) : Ok(at, MotionKind::Exclusive);
}

MotionResult MotionCharRight(const Buffer *b, const View *v, u64 pos, u64 count, u32) {
  u64 line_end = BufferLineEnd(b, BufferLineFromOffset(b, pos));
  u64 at = pos;
  for (u64 i = 0; i < count && at < line_end; i += 1) at = BufferNextCodepoint(b, at);
  return (at == pos) ? Fail(pos, MotionKind::Exclusive) : Ok(at, MotionKind::Exclusive);
}

MotionResult MotionLineUp(const Buffer *b, const View *v, u64 pos, u64 count, u32) {
  u64 line = BufferLineFromOffset(b, pos);
  if (line == 0) return Fail(pos, MotionKind::Linewise);

  u64 target_line = (count > line) ? 0 : line - count;
  // Vertical motion aims at the remembered column, not the current one.
  return Ok(BufferOffsetFromColumn(b, target_line, v->preferred_column), MotionKind::Linewise);
}

MotionResult MotionLineDown(const Buffer *b, const View *v, u64 pos, u64 count, u32) {
  u64 line = BufferLineFromOffset(b, pos);
  u64 last = BufferLineCount(b) - 1;
  if (line >= last) return Fail(pos, MotionKind::Linewise);

  u64 target_line = Min(line + count, last);
  return Ok(BufferOffsetFromColumn(b, target_line, v->preferred_column), MotionKind::Linewise);
}

MotionResult MotionWordForward(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 at = pos;
  for (u64 i = 0; i < count; i += 1) at = StepWordForward(b, at, false);
  return (at == pos) ? Fail(pos, MotionKind::Exclusive) : Ok(at, MotionKind::Exclusive);
}

MotionResult MotionWordForwardBig(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 at = pos;
  for (u64 i = 0; i < count; i += 1) at = StepWordForward(b, at, true);
  return (at == pos) ? Fail(pos, MotionKind::Exclusive) : Ok(at, MotionKind::Exclusive);
}

MotionResult MotionWordBackward(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 at = pos;
  for (u64 i = 0; i < count; i += 1) at = StepWordBackward(b, at, false);
  return (at == pos) ? Fail(pos, MotionKind::Exclusive) : Ok(at, MotionKind::Exclusive);
}

MotionResult MotionWordBackwardBig(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 at = pos;
  for (u64 i = 0; i < count; i += 1) at = StepWordBackward(b, at, true);
  return (at == pos) ? Fail(pos, MotionKind::Exclusive) : Ok(at, MotionKind::Exclusive);
}

// `e` is inclusive: `de` takes the last character of the word with it.
MotionResult MotionWordEnd(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 at = pos;
  for (u64 i = 0; i < count; i += 1) at = StepWordEnd(b, at, false);
  return (at == pos) ? Fail(pos, MotionKind::Inclusive) : Ok(at, MotionKind::Inclusive);
}

MotionResult MotionWordEndBig(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 at = pos;
  for (u64 i = 0; i < count; i += 1) at = StepWordEnd(b, at, true);
  return (at == pos) ? Fail(pos, MotionKind::Inclusive) : Ok(at, MotionKind::Inclusive);
}

MotionResult MotionLineStart(const Buffer *b, const View *, u64 pos, u64, u32) {
  return Ok(BufferOffsetFromLine(b, BufferLineFromOffset(b, pos)), MotionKind::Exclusive);
}

MotionResult MotionLineFirstNonBlank(const Buffer *b, const View *, u64 pos, u64, u32) {
  RangeU64 range = BufferLineRange(b, BufferLineFromOffset(b, pos));
  u64 at = range.min;
  while (at < range.max && CharIsSpace(BufferByteAt(b, at))) at = BufferNextCodepoint(b, at);
  return Ok(at, MotionKind::Exclusive);
}

MotionResult MotionFirstNonBlankLinewise(const Buffer *b, const View *, u64 pos, u64 count,
                                         u32) {
  u64 line = BufferLineFromOffset(b, pos);
  // A count of one means this line, which is why the step is count - 1.
  u64 target = Min(line + (count > 0 ? count - 1 : 0), BufferLineCount(b) - 1);

  RangeU64 range = BufferLineRange(b, target);
  u64 at = range.min;
  while (at < range.max && CharIsSpace(BufferByteAt(b, at))) at = BufferNextCodepoint(b, at);

  return Ok(at, MotionKind::Linewise);
}

MotionResult MotionLineEnd(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 line = BufferLineFromOffset(b, pos);
  // A count on $ moves down that many lines first, as vim does.
  u64 target = Min(line + (count > 0 ? count - 1 : 0), BufferLineCount(b) - 1);
  return Ok(BufferLineEnd(b, target), MotionKind::Inclusive);
}

MotionResult MotionFileStart(const Buffer *b, const View *, u64, u64, u32) {
  return Ok(0, MotionKind::Linewise);
}

// G with a count goes to that line; without one, to the last.
MotionResult MotionFileEnd(const Buffer *b, const View *, u64, u64 count, u32 arg) {
  bool has_count = (arg != 0);
  u64 last = BufferLineCount(b) - 1;
  u64 line = has_count ? Min(count - 1, last) : last;
  return Ok(BufferOffsetFromLine(b, line), MotionKind::Linewise);
}

MotionResult MotionParagraphForward(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 line = BufferLineFromOffset(b, pos);
  u64 last = BufferLineCount(b) - 1;

  for (u64 i = 0; i < count; i += 1) {
    // Skip any blank run we are sitting in, then find the next one.
    while (line < last && LineIsBlank(b, line)) line += 1;
    while (line < last && !LineIsBlank(b, line)) line += 1;
  }
  return Ok(BufferOffsetFromLine(b, line), MotionKind::Exclusive);
}

MotionResult MotionParagraphBackward(const Buffer *b, const View *, u64 pos, u64 count, u32) {
  u64 line = BufferLineFromOffset(b, pos);

  for (u64 i = 0; i < count; i += 1) {
    while (line > 0 && LineIsBlank(b, line)) line -= 1;
    while (line > 0 && !LineIsBlank(b, line)) line -= 1;
  }
  return Ok(BufferOffsetFromLine(b, line), MotionKind::Exclusive);
}

MotionResult MotionFindCharForward(const Buffer *b, const View *, u64 pos, u64 count, u32 arg) {
  bool found = false;
  u64 at = FindCharOnLine(b, pos, arg, count, true, &found);
  // f is inclusive: `df,` removes the comma too.
  return found ? Ok(at, MotionKind::Inclusive) : Fail(pos, MotionKind::Inclusive);
}

MotionResult MotionFindCharBackward(const Buffer *b, const View *, u64 pos, u64 count, u32 arg) {
  bool found = false;
  u64 at = FindCharOnLine(b, pos, arg, count, false, &found);
  return found ? Ok(at, MotionKind::Exclusive) : Fail(pos, MotionKind::Exclusive);
}

MotionResult MotionTillCharForward(const Buffer *b, const View *, u64 pos, u64 count, u32 arg) {
  bool found = false;
  u64 at = FindCharOnLine(b, pos, arg, count, true, &found);
  if (!found) return Fail(pos, MotionKind::Inclusive);
  // t stops one short of the target.
  return Ok(BufferPrevCodepoint(b, at), MotionKind::Inclusive);
}

MotionResult MotionTillCharBackward(const Buffer *b, const View *, u64 pos, u64 count, u32 arg) {
  bool found = false;
  u64 at = FindCharOnLine(b, pos, arg, count, false, &found);
  if (!found) return Fail(pos, MotionKind::Exclusive);
  return Ok(BufferNextCodepoint(b, at), MotionKind::Exclusive);
}

MotionResult MotionMatchingBracket(const Buffer *b, const View *, u64 pos, u64, u32) {
  constexpr const char *kOpen = "([{";
  constexpr const char *kClose = ")]}";

  u64 size = BufferSize(b);

  // Vim scans forward on the line for a bracket if the cursor is not on one.
  u64 line_end = BufferLineEnd(b, BufferLineFromOffset(b, pos));
  u64 at = pos;
  u8 c = 0;
  i32 which = -1;
  bool opening = false;

  while (at < line_end) {
    c = BufferByteAt(b, at);
    for (i32 i = 0; i < 3; i += 1) {
      if (c == (u8)kOpen[i]) { which = i; opening = true; break; }
      if (c == (u8)kClose[i]) { which = i; opening = false; break; }
    }
    if (which >= 0) break;
    at = BufferNextCodepoint(b, at);
  }
  if (which < 0) return Fail(pos, MotionKind::Inclusive);

  u8 open = (u8)kOpen[which];
  u8 close = (u8)kClose[which];

  i64 depth = 0;
  if (opening) {
    for (u64 p = at; p < size; p = BufferNextCodepoint(b, p)) {
      u8 ch = BufferByteAt(b, p);
      if (ch == open) depth += 1;
      else if (ch == close) {
        depth -= 1;
        if (depth == 0) return Ok(p, MotionKind::Inclusive);
      }
    }
  } else {
    for (u64 p = at;; ) {
      u8 ch = BufferByteAt(b, p);
      if (ch == close) depth += 1;
      else if (ch == open) {
        depth -= 1;
        if (depth == 0) return Ok(p, MotionKind::Inclusive);
      }
      if (p == 0) break;
      p = BufferPrevCodepoint(b, p);
    }
  }

  return Fail(pos, MotionKind::Inclusive);
}

// ---------------------------------------------------------------------------
// Text objects
// ---------------------------------------------------------------------------

TextObjectResult TextObjectWord(const Buffer *b, u64 pos, u64 count, bool inner, bool big) {
  u64 size = BufferSize(b);
  if (size == 0) return TextObjectResult{RangeU64{0, 0}, false, false};

  auto classify = big ? ClassOfBig : ClassOf;
  CharClass start_class = classify(BufferByteAt(b, pos));

  u64 start = pos;
  while (start > 0) {
    u64 prev = BufferPrevCodepoint(b, start);
    if (classify(BufferByteAt(b, prev)) != start_class) break;
    start = prev;
  }

  u64 end = pos;
  while (end < size && classify(BufferByteAt(b, end)) == start_class) {
    end = BufferNextCodepoint(b, end);
  }

  // Repeating extends over following runs, so 2iw covers a word and the space.
  for (u64 i = 1; i < count && end < size; i += 1) {
    CharClass next_class = classify(BufferByteAt(b, end));
    while (end < size && classify(BufferByteAt(b, end)) == next_class) {
      end = BufferNextCodepoint(b, end);
    }
  }

  if (!inner) {
    // `aw` takes the trailing whitespace, or the leading run if there is none.
    u64 after = end;
    while (after < size && classify(BufferByteAt(b, after)) == CharClass::Blank &&
           !IsNewline(b, after)) {
      after = BufferNextCodepoint(b, after);
    }
    if (after != end) {
      end = after;
    } else {
      while (start > 0) {
        u64 prev = BufferPrevCodepoint(b, start);
        if (classify(BufferByteAt(b, prev)) != CharClass::Blank || IsNewline(b, prev)) break;
        start = prev;
      }
    }
  }

  return TextObjectResult{RangeU64{start, end}, false, true};
}

TextObjectResult TextObjectQuoted(const Buffer *b, u64 pos, u8 quote, bool inner) {
  u64 line = BufferLineFromOffset(b, pos);
  RangeU64 line_range = BufferLineRange(b, line);

  // Quotes are matched within the line, pairing them left to right, so the
  // cursor's position decides which pair it falls inside.
  u64 open = line_range.max;
  u64 close = line_range.max;

  for (u64 p = line_range.min; p < line_range.max; p = BufferNextCodepoint(b, p)) {
    if (BufferByteAt(b, p) != quote) continue;

    u64 next = line_range.max;
    for (u64 q = BufferNextCodepoint(b, p); q < line_range.max; q = BufferNextCodepoint(b, q)) {
      if (BufferByteAt(b, q) == quote) { next = q; break; }
    }
    if (next == line_range.max) break;

    if (pos >= p && pos <= next) { open = p; close = next; break; }
    p = next;
  }

  if (open == line_range.max) return TextObjectResult{RangeU64{pos, pos}, false, false};

  RangeU64 range = inner ? RangeU64{BufferNextCodepoint(b, open), close}
                         : RangeU64{open, BufferNextCodepoint(b, close)};
  return TextObjectResult{range, false, true};
}

TextObjectResult TextObjectDelimited(const Buffer *b, u64 pos, u8 open_ch, u8 close_ch,
                                     bool inner) {
  u64 size = BufferSize(b);

  // Scan outwards for the innermost enclosing pair.
  i64 depth = 0;
  u64 open = size;
  for (u64 p = pos;; ) {
    u8 c = BufferByteAt(b, p);
    if (c == close_ch && p != pos) depth += 1;
    else if (c == open_ch) {
      if (depth == 0) { open = p; break; }
      depth -= 1;
    }
    if (p == 0) break;
    p = BufferPrevCodepoint(b, p);
  }
  if (open == size) return TextObjectResult{RangeU64{pos, pos}, false, false};

  depth = 0;
  u64 close = size;
  for (u64 p = open; p < size; p = BufferNextCodepoint(b, p)) {
    u8 c = BufferByteAt(b, p);
    if (c == open_ch) depth += 1;
    else if (c == close_ch) {
      depth -= 1;
      if (depth == 0) { close = p; break; }
    }
  }
  if (close == size) return TextObjectResult{RangeU64{pos, pos}, false, false};

  RangeU64 range = inner ? RangeU64{BufferNextCodepoint(b, open), close}
                         : RangeU64{open, BufferNextCodepoint(b, close)};
  return TextObjectResult{range, false, true};
}

TextObjectResult TextObjectParagraph(const Buffer *b, u64 pos, bool inner) {
  u64 line = BufferLineFromOffset(b, pos);
  u64 last = BufferLineCount(b) - 1;
  bool blank = LineIsBlank(b, line);

  u64 first_line = line;
  while (first_line > 0 && LineIsBlank(b, first_line - 1) == blank) first_line -= 1;

  u64 last_line = line;
  while (last_line < last && LineIsBlank(b, last_line + 1) == blank) last_line += 1;

  if (!inner) {
    // `ap` also takes the blank run that follows.
    while (last_line < last && LineIsBlank(b, last_line + 1) != blank) last_line += 1;
  }

  RangeU64 range = RangeU64{BufferOffsetFromLine(b, first_line),
                            LineRangeWithNewline(&b->lines, &b->text, last_line).max};
  return TextObjectResult{range, true, true};
}
