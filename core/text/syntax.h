#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "text/token.h"

struct Buffer;

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

// ---------------------------------------------------------------------------
// Full-buffer lexer
//
// A minimal, data-driven scanner over LanguageDefinition metadata. It walks a
// buffer's bytes directly through BufferByteAt -- never copying the whole text
// into contiguous storage -- and fills the buffer's TokenArray plus a per-line
// cache of the lexer state carried across line boundaries.
//
// Only full rebuilds exist here; incremental re-lexing after a single edit
// (wiring into BufferReplace/undo/redo) is a later task.
// ---------------------------------------------------------------------------

// Multiline lexical constructs a line can start or end inside of. `Default`
// means no construct is active -- ordinary top-level text.
enum class SyntaxMode : u8 {
  Default = 0,
  BlockComment,        // inside /* ... */ (or the language's block comment)
  TripleSingleString,  // inside Python's '''...'''
  TripleDoubleString,  // inside Python's """..."""
  BacktickString,      // inside a JS/TS/Go `...` template/raw string
};

struct SyntaxState {
  SyntaxMode mode;
};

// Per-line bookkeeping: the state a line was entered/left in, plus the slice
// of the buffer's TokenArray produced while scanning it. `incoming` lets a
// future incremental re-lex start any line without rescanning from the top;
// `outgoing` is what the next line's `incoming` must be.
struct SyntaxLineCache {
  SyntaxState incoming;
  SyntaxState outgoing;
  u64 first_token;  // index into the buffer's TokenArray
  u64 token_count;
};

// Owns the arena backing both the line cache array and the buffer's token
// array (Buffer::tokens.tokens lives in `arena`, sized by `token_capacity`).
// A dedicated arena lets a full rebuild grow either array geometrically by
// simply pushing a bigger block and abandoning the old one -- bounded waste,
// no reallocation bookkeeping -- and lets the whole cache be dropped by
// releasing one arena.
struct SyntaxCache {
  const LanguageDefinition *language;
  Arena *arena;
  SyntaxLineCache *lines;
  u64 line_count;
  u64 line_capacity;
  u64 token_capacity;  // capacity backing Buffer::tokens.tokens
};

// Attaches syntax highlighting to `buffer` for `path`, allocating a dedicated
// arena on first use. Selects the language via SyntaxLanguageForPath and
// rebuilds the token cache. Reattaching with the same resolved language is a
// no-op -- callers do not need to guard against redundant attaches, e.g. on
// every save of an already-open file.
void SyntaxAttach(Buffer *buffer, String8 path);

// Releases the cache's arena and zeroes it. Safe to call on an unattached
// (zero-initialized) cache.
void SyntaxDestroy(SyntaxCache *cache);

// Full rescan of every line in the buffer, from Buffer::lines. No-op when the
// buffer has no syntax cache attached. Called after any whole-buffer text
// replacement (BufferSetText) so a highlighted buffer never shows stale
// tokens; incremental edits are Task 3.
void SyntaxRebuild(Buffer *buffer);
