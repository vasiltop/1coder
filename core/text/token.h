#pragma once

#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_types.h"

// Syntax highlighting seam.
//
// A buffer carries a token array; the renderer colours glyphs by looking each
// token's kind up in the theme. Nothing on either side knows who produced the
// tokens, so tree-sitter can be dropped in later by filling this array from a
// buffer's on_edit hook without the renderer changing at all.
//
// Until then the array stays empty, which every consumer must read as "all
// text is TokenKind::Default".

enum class TokenKind : u8 {
  Default = 0,
  Keyword,
  Identifier,
  Type,
  Function,
  String,
  Character,
  Number,
  Comment,
  Operator,
  Punctuation,
  Preprocessor,
  Constant,
  Error,
  COUNT
};

struct Token {
  u64 start;  // byte offset, inclusive
  u64 end;    // byte offset, exclusive
  TokenKind kind;
};

struct TokenArray {
  Token *tokens;
  u64 count;
};

// Kind covering `offset`, or Default when no token does. Tokens are kept sorted
// by start offset, so this is a binary search.
[[nodiscard]] TokenKind TokenKindAtOffset(const TokenArray *tokens, u64 offset);

// Index of the first token ending after `offset`, for walking a visible line's
// tokens without rescanning from the top of the file.
[[nodiscard]] u64 TokenIndexAtOffset(const TokenArray *tokens, u64 offset);
