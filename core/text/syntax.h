#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "text/token.h"

struct Buffer;

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

struct KeywordGroup {
  const String8 *words;
  u64 count;
  TokenKind kind;
};

struct LanguageDefinition {
  LanguageId id;
  String8 name;

  const String8 *extensions;
  u64 extension_count;

  const KeywordGroup *keyword_groups;
  u64 keyword_group_count;

  String8 line_comments[2];
  u64 line_comment_count;

  String8 block_comment_open;
  String8 block_comment_close;

  bool single_quote_is_character;
  bool triple_quoted_strings;
  bool backtick_strings;
  bool preprocessor_directives;
};

[[nodiscard]] const LanguageDefinition *SyntaxLanguageForPath(String8 path);

enum class SyntaxMode : u8 {
  Default = 0,
  BlockComment,
  TripleSingleString,
  TripleDoubleString,
  BacktickString,
};

struct SyntaxState {
  SyntaxMode mode;
};

struct SyntaxLineCache {
  SyntaxState incoming;
  SyntaxState outgoing;
  u64 first_token;
  u64 token_count;
};

struct SyntaxCache {
  const LanguageDefinition *language;
  Arena *arena;
  SyntaxLineCache *lines;
  u64 line_count;
  u64 line_capacity;
  u64 token_capacity;
  u64 lines_scanned_last_update;
};

void SyntaxAttach(Buffer *buffer, String8 path);
void SyntaxDestroy(SyntaxCache *cache);
void SyntaxRebuild(Buffer *buffer);

struct SyntaxEdit {
  u64 old_start_line;
  u64 old_end_line;
  u64 old_line_count;
  u64 old_size;
  SyntaxState start_state;
  u64 prefix_token_end;
  u64 old_token_count;
};

[[nodiscard]] SyntaxEdit SyntaxBeginEdit(const Buffer *buffer, RangeU64 old_range);
void SyntaxEndEdit(Buffer *buffer, SyntaxEdit edit);
