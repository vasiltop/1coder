#pragma once

#include "base/base_string.h"
#include "base/base_types.h"
#include "text/token.h"

// Language identification and compile-time metadata for the minimal lexer.
//
// Each LanguageDefinition is a static, arena-free descriptor: it stores
// pointers into read-only segment data (string literals and keyword arrays)
// so it can be embedded in a buffer without any allocation.

enum class LanguageId : u8 {
  Fallback = 0,
  Cpp,
  JavaScript,
  TypeScript,
  Python,
  Rust,
  Go,
  Toml,
  Json,
};

// One logical group of reserved words that all map to the same token kind.
struct KeywordGroup {
  const String8 *words;
  u64 count;
  TokenKind kind;
};

struct LanguageDefinition {
  LanguageId id;
  String8 name;

  // File extensions that identify this language (without leading dot).
  const String8 *extensions;
  u64 extension_count;

  // Keyword tables (control-flow, types, constants …).
  const KeywordGroup *keyword_groups;
  u64 keyword_group_count;

  // Line-comment prefixes; up to two (e.g. "//" and "#").
  String8 line_comments[2];
  u64 line_comment_count;

  // Block-comment delimiters; empty strings mean "not supported".
  String8 block_comment_open;
  String8 block_comment_close;

  // Lexer behaviour flags.
  bool single_quote_is_character;  // C, C++, Rust
  bool triple_quoted_strings;      // Python
  bool backtick_strings;           // JS, TS, Go
  bool preprocessor_directives;    // C, C++
};

// Returns the best matching LanguageDefinition for the given path, or the
// Fallback definition when the extension is unknown or absent. Never returns
// null.
[[nodiscard]] const LanguageDefinition *SyntaxLanguageForPath(String8 path);
