#pragma once

#include "editor/buffer.h"
#include "editor/view.h"
#include "vim/vim_state.h"

// Motions are pure functions of (buffer, position, count) returning a target
// offset and how the span should be treated. Because they know nothing about
// operators, every motion composes with every operator for free -- adding a
// motion makes it immediately available to d, c, y, > and <.

using MotionProc = MotionResult (*)(const Buffer *buffer, const View *view, u64 pos, u64 count,
                                    u32 argument);

// `argument` carries the target character for f/F/t/T and is ignored otherwise.

MotionResult MotionCharLeft(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionCharRight(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionLineUp(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionLineDown(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);

MotionResult MotionWordForward(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionWordBackward(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionWordEnd(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
// WORD variants: whitespace-delimited rather than punctuation-aware.
MotionResult MotionWordForwardBig(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionWordBackwardBig(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionWordEndBig(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);

MotionResult MotionLineStart(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionLineFirstNonBlank(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionLineEnd(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);

MotionResult MotionFileStart(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionFileEnd(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);

MotionResult MotionParagraphForward(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionParagraphBackward(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);

MotionResult MotionFindCharForward(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionFindCharBackward(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionTillCharForward(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);
MotionResult MotionTillCharBackward(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);

MotionResult MotionMatchingBracket(const Buffer *b, const View *v, u64 pos, u64 count, u32 arg);

// ---------------------------------------------------------------------------
// Text objects
//
// A parallel family that yields a range directly rather than a target, so the
// same operator machinery consumes both.
// ---------------------------------------------------------------------------

struct TextObjectResult {
  RangeU64 range;
  bool linewise;
  bool valid;
};

// `inner` selects iw/i" style (contents only) versus aw/a" (contents plus
// surrounding whitespace or delimiters).
TextObjectResult TextObjectWord(const Buffer *b, u64 pos, u64 count, bool inner, bool big);
TextObjectResult TextObjectQuoted(const Buffer *b, u64 pos, u8 quote, bool inner);
TextObjectResult TextObjectDelimited(const Buffer *b, u64 pos, u8 open, u8 close, bool inner);
TextObjectResult TextObjectParagraph(const Buffer *b, u64 pos, bool inner);
