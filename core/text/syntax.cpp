#include "text/syntax.h"

#include "editor/buffer.h"

// Full-buffer lexer. See syntax.h for the shape of the cache this fills in;
// this file is the scanner itself.
//
// The design is deliberately dumb: no lookahead beyond what a single language
// rule needs, no AST, no incremental re-lex (that is Task 3). Every rule reads
// straight from LanguageDefinition, so adding a language is a data change in
// syntax_languages.cpp, never a change here.

namespace {

constexpr u64 kSyntaxArenaReserve = MB(64);
constexpr u64 kSyntaxInitialLineCapacity = 256;
constexpr u64 kSyntaxInitialTokenCapacity = 256;

// ---------------------------------------------------------------------------
// Byte-level matching against buffer storage
//
// The gap buffer is not contiguous, so every comparison goes through
// BufferByteAt one byte at a time rather than taking a pointer into it.
// ---------------------------------------------------------------------------

// True when the bytes at [pos, pos + pattern.size) equal `pattern`, without
// reading past `end`.
[[nodiscard]] bool MatchAt(const Buffer *buffer, u64 pos, u64 end, String8 pattern) {
  if (pattern.size == 0 || pos + pattern.size > end) return false;
  for (u64 k = 0; k < pattern.size; k += 1) {
    if (BufferByteAt(buffer, pos + k) != pattern.str[k]) return false;
  }
  return true;
}

// True when [start, end) contains exactly `word`'s bytes -- used to compare a
// scanned identifier against a keyword table entry without ever materialising
// the identifier as a String8.
[[nodiscard]] bool IdentifierEquals(const Buffer *buffer, u64 start, u64 end, String8 word) {
  if (end - start != word.size) return false;
  for (u64 k = 0; k < word.size; k += 1) {
    if (BufferByteAt(buffer, start + k) != word.str[k]) return false;
  }
  return true;
}

// Searches [start, end) for `close`, honouring backslash escapes when
// requested (block comments do not support escaping their closer; quoted
// strings and backtick/triple strings do). On success `*out_end` is the
// offset just past the matched closer.
[[nodiscard]] bool FindClose(const Buffer *buffer, u64 start, u64 end, String8 close,
                              bool allow_escape, u64 *out_end) {
  u64 i = start;
  while (i < end) {
    if (MatchAt(buffer, i, end, close)) {
      *out_end = i + close.size;
      return true;
    }
    if (allow_escape && BufferByteAt(buffer, i) == '\\' && i + 1 < end) {
      i += 2;
      continue;
    }
    i += 1;
  }
  return false;
}

[[nodiscard]] bool IsPunctuationByte(u8 c) {
  switch (c) {
    case '(': case ')': case '[': case ']': case '{': case '}':
    case ',': case ';': case ':': case '.':
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool IsOperatorByte(u8 c) {
  switch (c) {
    case '+': case '-': case '*': case '/': case '%':
    case '=': case '<': case '>': case '!':
    case '&': case '|': case '^': case '~':
    case '?':
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Growth helpers
//
// Both arrays live in the cache's dedicated arena, so "growing" means pushing
// a bigger block and copying forward -- the arena is a bump allocator, not a
// realloc, so the previous (smaller) block is simply abandoned. Doubling
// bounds the total waste to the same order as the final size.
// ---------------------------------------------------------------------------

void EnsureLineCapacity(SyntaxCache *cache, u64 needed) {
  if (needed <= cache->line_capacity) return;

  u64 new_capacity = Max(needed, Max(cache->line_capacity * 2, kSyntaxInitialLineCapacity));
  SyntaxLineCache *lines = PushArrayNoZero(cache->arena, SyntaxLineCache, new_capacity);
  if (cache->line_count > 0) {
    for (u64 i = 0; i < cache->line_count; i += 1) lines[i] = cache->lines[i];
  }
  cache->lines = lines;
  cache->line_capacity = new_capacity;
}

void EnsureTokenCapacity(Buffer *buffer, u64 needed) {
  SyntaxCache *cache = &buffer->syntax;
  if (needed <= cache->token_capacity) return;

  u64 new_capacity = Max(needed, Max(cache->token_capacity * 2, kSyntaxInitialTokenCapacity));
  Token *tokens = PushArrayNoZero(cache->arena, Token, new_capacity);
  if (buffer->tokens.count > 0) {
    for (u64 i = 0; i < buffer->tokens.count; i += 1) tokens[i] = buffer->tokens.tokens[i];
  }
  buffer->tokens.tokens = tokens;
  cache->token_capacity = new_capacity;
}

// Appends a token, dropping empty ranges so the array stays the non-empty,
// non-Default set the renderer seam promises.
void AppendToken(Buffer *buffer, u64 *token_count, u64 start, u64 end, TokenKind kind) {
  if (end <= start) return;
  EnsureTokenCapacity(buffer, *token_count + 1);
  buffer->tokens.tokens[*token_count] = Token{start, end, kind};
  *token_count += 1;
}

// ---------------------------------------------------------------------------
// Identifier classification
// ---------------------------------------------------------------------------

[[nodiscard]] TokenKind ClassifyIdentifier(const Buffer *buffer, const LanguageDefinition *lang,
                                            u64 start, u64 end, u64 line_end) {
  for (u64 g = 0; g < lang->keyword_group_count; g += 1) {
    const KeywordGroup *group = &lang->keyword_groups[g];
    for (u64 w = 0; w < group->count; w += 1) {
      if (IdentifierEquals(buffer, start, end, group->words[w])) return group->kind;
    }
  }

  // Not a keyword: a call site is the identifier immediately (modulo
  // horizontal whitespace) followed by '('.
  u64 p = end;
  while (p < line_end && CharIsSpace(BufferByteAt(buffer, p))) p += 1;
  if (p < line_end && BufferByteAt(buffer, p) == '(') return TokenKind::Function;

  return TokenKind::Default;
}

// ---------------------------------------------------------------------------
// Number scanning
//
// Covers decimal integers/floats/exponents and 0x/0b/0o-prefixed integers,
// with '_' digit separators. Stops at the first byte that would not extend a
// sensible numeric literal, so a suffix like the `f` in `1.0f` or trailing
// prose like `123abc` is left for the identifier scanner (which will simply
// not classify it, leaving it Default) rather than folded into the number.
// ---------------------------------------------------------------------------

[[nodiscard]] u64 ScanNumber(const Buffer *buffer, u64 start, u64 end) {
  u64 i = start;
  u8 first = BufferByteAt(buffer, i);

  if (first == '0' && i + 1 < end) {
    u8 marker = CharToLower(BufferByteAt(buffer, i + 1));
    if (marker == 'x' || marker == 'b' || marker == 'o') {
      i += 2;
      while (i < end) {
        u8 c = BufferByteAt(buffer, i);
        if (CharIsAlnum(c) || c == '_') { i += 1; continue; }
        break;
      }
      return i;
    }
  }

  while (i < end) {
    u8 c = BufferByteAt(buffer, i);
    if (CharIsDigit(c) || c == '_') { i += 1; continue; }
    break;
  }

  if (i < end && BufferByteAt(buffer, i) == '.' && i + 1 < end &&
      CharIsDigit(BufferByteAt(buffer, i + 1))) {
    i += 1;
    while (i < end) {
      u8 c = BufferByteAt(buffer, i);
      if (CharIsDigit(c) || c == '_') { i += 1; continue; }
      break;
    }
  }

  if (i < end) {
    u8 c = BufferByteAt(buffer, i);
    if (c == 'e' || c == 'E') {
      u64 j = i + 1;
      if (j < end && (BufferByteAt(buffer, j) == '+' || BufferByteAt(buffer, j) == '-')) j += 1;
      if (j < end && CharIsDigit(BufferByteAt(buffer, j))) {
        j += 1;
        while (j < end) {
          u8 d = BufferByteAt(buffer, j);
          if (CharIsDigit(d) || d == '_') { j += 1; continue; }
          break;
        }
        i = j;
      }
    }
  }

  return i;
}

// ---------------------------------------------------------------------------
// Per-line scan
// ---------------------------------------------------------------------------

// Scans one line's bytes [range.min, range.max), which never include the
// line's newline (BufferLineRange excludes it), continuing whatever
// multiline construct `incoming` describes and returning the state the next
// line inherits.
[[nodiscard]] SyntaxState ScanLine(Buffer *buffer, const LanguageDefinition *lang, RangeU64 range,
                                    SyntaxState incoming, u64 *token_count) {
  u64 i = range.min;
  const u64 end = range.max;
  SyntaxMode mode = incoming.mode;

  // Continue a construct opened on a previous line before anything else --
  // it takes precedence over every other rule until it closes.
  if (mode == SyntaxMode::BlockComment) {
    u64 close_end = 0;
    if (FindClose(buffer, i, end, lang->block_comment_close, false, &close_end)) {
      AppendToken(buffer, token_count, i, close_end, TokenKind::Comment);
      i = close_end;
      mode = SyntaxMode::Default;
    } else {
      AppendToken(buffer, token_count, i, end, TokenKind::Comment);
      return SyntaxState{SyntaxMode::BlockComment};
    }
  } else if (mode == SyntaxMode::TripleSingleString || mode == SyntaxMode::TripleDoubleString) {
    String8 close = (mode == SyntaxMode::TripleSingleString) ? Str8Lit("'''") : Str8Lit("\"\"\"");
    u64 close_end = 0;
    if (FindClose(buffer, i, end, close, true, &close_end)) {
      AppendToken(buffer, token_count, i, close_end, TokenKind::String);
      i = close_end;
      mode = SyntaxMode::Default;
    } else {
      AppendToken(buffer, token_count, i, end, TokenKind::String);
      return SyntaxState{mode};
    }
  } else if (mode == SyntaxMode::BacktickString) {
    u64 close_end = 0;
    if (FindClose(buffer, i, end, Str8Lit("`"), true, &close_end)) {
      AppendToken(buffer, token_count, i, close_end, TokenKind::String);
      i = close_end;
      mode = SyntaxMode::Default;
    } else {
      AppendToken(buffer, token_count, i, end, TokenKind::String);
      return SyntaxState{SyntaxMode::BacktickString};
    }
  }

  // A leading '#' (modulo indentation) on an otherwise-fresh line is a whole-
  // line preprocessor directive for languages that have them (C/C++). This
  // check only ever runs when `mode` just resolved to Default above, matching
  // the "active multiline state beats everything" precedence.
  if (lang->preprocessor_directives && i < end) {
    u64 p = i;
    while (p < end && CharIsSpace(BufferByteAt(buffer, p))) p += 1;
    if (p < end && BufferByteAt(buffer, p) == '#') {
      AppendToken(buffer, token_count, p, end, TokenKind::Preprocessor);
      return SyntaxState{SyntaxMode::Default};
    }
  }

  while (i < end) {
    u8 c = BufferByteAt(buffer, i);

    // Block comment open.
    if (lang->block_comment_open.size > 0 && MatchAt(buffer, i, end, lang->block_comment_open)) {
      u64 close_end = 0;
      u64 search_start = i + lang->block_comment_open.size;
      if (FindClose(buffer, search_start, end, lang->block_comment_close, false, &close_end)) {
        AppendToken(buffer, token_count, i, close_end, TokenKind::Comment);
        i = close_end;
      } else {
        AppendToken(buffer, token_count, i, end, TokenKind::Comment);
        return SyntaxState{SyntaxMode::BlockComment};
      }
      continue;
    }

    // Line comments -- the rest of the line, no state carried.
    bool matched_line_comment = false;
    for (u64 lc = 0; lc < lang->line_comment_count; lc += 1) {
      String8 prefix = lang->line_comments[lc];
      if (prefix.size > 0 && MatchAt(buffer, i, end, prefix)) {
        AppendToken(buffer, token_count, i, end, TokenKind::Comment);
        i = end;
        matched_line_comment = true;
        break;
      }
    }
    if (matched_line_comment) break;

    // Triple-quoted strings (Python).
    if (lang->triple_quoted_strings) {
      bool is_single = MatchAt(buffer, i, end, Str8Lit("'''"));
      bool is_double = !is_single && MatchAt(buffer, i, end, Str8Lit("\"\"\""));
      if (is_single || is_double) {
        String8 close = is_single ? Str8Lit("'''") : Str8Lit("\"\"\"");
        u64 close_end = 0;
        if (FindClose(buffer, i + 3, end, close, true, &close_end)) {
          AppendToken(buffer, token_count, i, close_end, TokenKind::String);
          i = close_end;
        } else {
          AppendToken(buffer, token_count, i, end, TokenKind::String);
          SyntaxMode carried = is_single ? SyntaxMode::TripleSingleString : SyntaxMode::TripleDoubleString;
          return SyntaxState{carried};
        }
        continue;
      }
    }

    // Backtick strings (JS/TS/Go).
    if (lang->backtick_strings && c == '`') {
      u64 close_end = 0;
      if (FindClose(buffer, i + 1, end, Str8Lit("`"), true, &close_end)) {
        AppendToken(buffer, token_count, i, close_end, TokenKind::String);
        i = close_end;
      } else {
        AppendToken(buffer, token_count, i, end, TokenKind::String);
        return SyntaxState{SyntaxMode::BacktickString};
      }
      continue;
    }

    // Double-quoted strings. Unterminated stops at line end -- no state is
    // carried across lines for these, unlike block/triple/backtick.
    if (c == '"') {
      u64 close_end = 0;
      bool closed = FindClose(buffer, i + 1, end, Str8Lit("\""), true, &close_end);
      u64 token_end = closed ? close_end : end;
      AppendToken(buffer, token_count, i, token_end, TokenKind::String);
      i = token_end;
      continue;
    }

    // Single-quoted strings or characters, depending on the language.
    if (c == '\'') {
      u64 close_end = 0;
      bool closed = FindClose(buffer, i + 1, end, Str8Lit("'"), true, &close_end);
      u64 token_end = closed ? close_end : end;
      TokenKind kind = lang->single_quote_is_character ? TokenKind::Character : TokenKind::String;
      AppendToken(buffer, token_count, i, token_end, kind);
      i = token_end;
      continue;
    }

    // Numbers.
    if (CharIsDigit(c) || (c == '.' && i + 1 < end && CharIsDigit(BufferByteAt(buffer, i + 1)))) {
      u64 number_end = ScanNumber(buffer, i, end);
      AppendToken(buffer, token_count, i, number_end, TokenKind::Number);
      i = number_end;
      continue;
    }

    // Identifiers, keyword classification, and function-call detection.
    if (CharIsAlpha(c) || c == '_') {
      u64 ident_end = i + 1;
      while (ident_end < end) {
        u8 ic = BufferByteAt(buffer, ident_end);
        if (!CharIsAlnum(ic) && ic != '_') break;
        ident_end += 1;
      }

      TokenKind kind = ClassifyIdentifier(buffer, lang, i, ident_end, end);
      if (kind != TokenKind::Default) AppendToken(buffer, token_count, i, ident_end, kind);
      i = ident_end;
      continue;
    }

    // Operators: runs of adjacent operator bytes become one token.
    if (IsOperatorByte(c)) {
      u64 op_end = i + 1;
      while (op_end < end && IsOperatorByte(BufferByteAt(buffer, op_end))) op_end += 1;
      AppendToken(buffer, token_count, i, op_end, TokenKind::Operator);
      i = op_end;
      continue;
    }

    // Punctuation: one byte each.
    if (IsPunctuationByte(c)) {
      AppendToken(buffer, token_count, i, i + 1, TokenKind::Punctuation);
      i += 1;
      continue;
    }

    // Whitespace and anything unrecognised -- including UTF-8 continuation
    // and lead bytes, which stay Default and keep offsets byte-based rather
    // than being decoded here -- advance one byte at a time.
    i += 1;
  }

  return SyntaxState{SyntaxMode::Default};
}

}  // namespace

void SyntaxAttach(Buffer *buffer, String8 path) {
  const LanguageDefinition *lang = SyntaxLanguageForPath(path);
  SyntaxCache *cache = &buffer->syntax;

  // Reattaching the same resolved language changes nothing structural, so
  // callers -- e.g. re-opening or re-saving the same file -- do not pay for a
  // rebuild they do not need.
  if (cache->arena != nullptr && cache->language == lang) return;

  if (cache->arena == nullptr) {
    cache->arena = ArenaAlloc(kSyntaxArenaReserve);
  } else {
    // Changing language: drop everything derived from the old one. Clearing
    // (rather than releasing) keeps the already-committed pages, so repeated
    // attach/detach on the same buffer does not keep re-growing.
    ArenaClear(cache->arena);
  }

  cache->language = lang;
  cache->lines = nullptr;
  cache->line_count = 0;
  cache->line_capacity = 0;
  cache->token_capacity = 0;
  buffer->tokens = TokenArray{nullptr, 0};

  SyntaxRebuild(buffer);
}

void SyntaxDestroy(SyntaxCache *cache) {
  if (cache->arena != nullptr) ArenaRelease(cache->arena);
  *cache = SyntaxCache{};
}

void SyntaxRebuild(Buffer *buffer) {
  SyntaxCache *cache = &buffer->syntax;
  if (cache->arena == nullptr || cache->language == nullptr) return;

  // Every token and every line-cache entry is derived fresh below, so reclaim
  // the arena up front rather than letting each rebuild pile more geometric
  // growth atop the last one.
  ArenaClear(cache->arena);
  cache->lines = nullptr;
  cache->line_count = 0;
  cache->line_capacity = 0;
  cache->token_capacity = 0;
  buffer->tokens = TokenArray{nullptr, 0};

  u64 line_total = BufferLineCount(buffer);
  EnsureLineCapacity(cache, line_total);

  SyntaxState state = SyntaxState{SyntaxMode::Default};
  u64 token_count = 0;

  for (u64 line = 0; line < line_total; line += 1) {
    RangeU64 range = BufferLineRange(buffer, line);
    SyntaxState incoming = state;
    u64 first_token = token_count;

    state = ScanLine(buffer, cache->language, range, incoming, &token_count);

    cache->lines[line] = SyntaxLineCache{incoming, state, first_token, token_count - first_token};
  }

  cache->line_count = line_total;
  buffer->tokens.count = token_count;
}
