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

// `current_count` is the number of live tokens *right now* -- during
// SyntaxRebuild that is the scan's running local counter, not
// `buffer->tokens.count` (which stays 0 until the whole rebuild finishes and
// is assigned once at the end). Copying from `buffer->tokens.count` instead
// would see 0 on every growth mid-rebuild and silently drop every token
// appended so far into uninitialized memory.
void EnsureTokenCapacity(Buffer *buffer, u64 current_count, u64 needed) {
  SyntaxCache *cache = &buffer->syntax;
  if (needed <= cache->token_capacity) return;

  u64 new_capacity = Max(needed, Max(cache->token_capacity * 2, kSyntaxInitialTokenCapacity));
  Token *tokens = PushArrayNoZero(cache->arena, Token, new_capacity);
  if (current_count > 0) {
    for (u64 i = 0; i < current_count; i += 1) tokens[i] = buffer->tokens.tokens[i];
  }
  buffer->tokens.tokens = tokens;
  cache->token_capacity = new_capacity;
}

// Appends a token, dropping empty ranges so the array stays the non-empty,
// non-Default set the renderer seam promises.
void AppendToken(Buffer *buffer, u64 *token_count, u64 start, u64 end, TokenKind kind) {
  if (end <= start) return;
  EnsureTokenCapacity(buffer, *token_count, *token_count + 1);
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
  const u64 line_start = range.min;
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
  // must only fire when `i` is still at the true line start: if a multiline
  // construct carried in from a previous line and closed mid-line above, `i`
  // has already moved past that closer, and whatever follows on the same
  // line is not line-leading even though `mode` just resolved to Default.
  if (lang->preprocessor_directives && i == line_start && i < end) {
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
  cache->lines_scanned_last_update = line_total;
  buffer->tokens.count = token_count;
}

// ---------------------------------------------------------------------------
// Incremental syntax update
// ---------------------------------------------------------------------------

SyntaxEdit SyntaxBeginEdit(const Buffer *buffer, RangeU64 old_range) {
  SyntaxEdit edit = {};
  const SyntaxCache *cache = &buffer->syntax;
  if (cache->arena == nullptr || cache->language == nullptr) return edit;

  u64 size = BufferSize(buffer);
  u64 start_off = Min(old_range.min, size);
  u64 end_off = Min(old_range.max, size);

  edit.old_size = size;
  edit.old_line_count = cache->line_count;
  edit.old_start_line = BufferLineFromOffset(buffer, start_off);
  edit.old_end_line = BufferLineFromOffset(buffer, end_off);

  // State entering the first changed line.
  edit.start_state = (edit.old_start_line == 0)
    ? SyntaxState{SyntaxMode::Default}
    : cache->lines[edit.old_start_line - 1].outgoing;

  // Token boundaries: prefix tokens are all tokens before old_start_line.
  edit.prefix_token_end = cache->lines[edit.old_start_line].first_token;

  // Suffix tokens start after old_end_line.
  if (edit.old_end_line + 1 < cache->line_count) {
    edit.suffix_token_start = cache->lines[edit.old_end_line + 1].first_token;
    edit.suffix_byte_start = BufferOffsetFromLine(buffer, edit.old_end_line + 1);
  } else {
    edit.suffix_token_start = buffer->tokens.count;
    edit.suffix_byte_start = size;
  }

  edit.old_token_count = buffer->tokens.count;

  return edit;
}

void SyntaxEndEdit(Buffer *buffer, SyntaxEdit edit, RangeU64 /*new_range*/) {
  SyntaxCache *cache = &buffer->syntax;
  if (cache->arena == nullptr || cache->language == nullptr) return;

  u64 new_size = BufferSize(buffer);
  u64 new_line_count = BufferLineCount(buffer);

  // Byte delta for shifting suffix tokens.
  i64 byte_delta = (i64)new_size - (i64)edit.old_size;

  // Line geometry.
  i64 line_delta = (i64)new_line_count - (i64)edit.old_line_count;
  u64 old_edited_lines = edit.old_end_line - edit.old_start_line + 1;
  u64 new_edited_lines = (u64)((i64)old_edited_lines + line_delta);

  u64 old_suffix_lines = (edit.old_end_line + 1 < edit.old_line_count)
    ? (edit.old_line_count - edit.old_end_line - 1) : 0;
  u64 total_new_lines = edit.old_start_line + new_edited_lines + old_suffix_lines;

  // Grow line cache if needed.
  EnsureLineCapacity(cache, total_new_lines);

  // Move suffix line-cache entries to their new positions.
  u64 old_suffix_idx = edit.old_end_line + 1;
  u64 new_suffix_idx = edit.old_start_line + new_edited_lines;

  if (old_suffix_lines > 0 && old_suffix_idx != new_suffix_idx) {
    if (new_suffix_idx > old_suffix_idx) {
      for (u64 i = old_suffix_lines; i > 0; i -= 1)
        cache->lines[new_suffix_idx + i - 1] = cache->lines[old_suffix_idx + i - 1];
    } else {
      for (u64 i = 0; i < old_suffix_lines; i += 1)
        cache->lines[new_suffix_idx + i] = cache->lines[old_suffix_idx + i];
    }
  }
  cache->line_count = total_new_lines;

  // --- Rescan lines, collecting tokens into scratch storage. ---
  // We scan from edit.old_start_line onward.  After all lines in the new
  // edited region are scanned, we check each suffix line: if the state we
  // carry into it equals its (already-moved) cached incoming state, we stop
  // (convergence) and retain the remaining suffix tokens with a byte shift.

  TempArena scratch = ScratchBegin(&cache->arena, 1);

  u64 rescan_cap = 256;
  Token *rescan_tokens = PushArrayNoZero(scratch.arena, Token, rescan_cap);
  u64 rescan_count = 0;

  SyntaxState state = edit.start_state;
  u64 lines_scanned = 0;
  u64 converge_line = total_new_lines; // line where we stopped (suffix kept from here)

  for (u64 line = edit.old_start_line; line < total_new_lines; line += 1) {
    // Check convergence: once we are past the edited region and our state
    // matches the cached incoming state of this (moved) suffix line, stop.
    if (line >= new_suffix_idx) {
      if (state.mode == cache->lines[line].incoming.mode) {
        converge_line = line;
        break;
      }
    }

    RangeU64 range = BufferLineRange(buffer, line);
    SyntaxState incoming = state;
    u64 first_tok = rescan_count;

    // Redirect buffer->tokens to our scratch array so ScanLine/AppendToken
    // works unmodified.
    Token *saved_tokens = buffer->tokens.tokens;
    u64 saved_count_val = buffer->tokens.count;
    u64 saved_cap = cache->token_capacity;

    buffer->tokens.tokens = rescan_tokens;
    cache->token_capacity = rescan_cap;

    u64 local_count = rescan_count;
    state = ScanLine(buffer, cache->language, range, incoming, &local_count);

    // Pick up growth if EnsureTokenCapacity triggered.
    rescan_tokens = buffer->tokens.tokens;
    rescan_cap = cache->token_capacity;
    rescan_count = local_count;

    buffer->tokens.tokens = saved_tokens;
    buffer->tokens.count = saved_count_val;
    cache->token_capacity = saved_cap;

    cache->lines[line] = SyntaxLineCache{incoming, state, 0, rescan_count - first_tok};
    lines_scanned += 1;
  }

  // --- Determine suffix tokens to retain ---
  // If we converged at `converge_line`, the suffix tokens start at the old
  // token index stored in that line's (moved) cache entry.  Otherwise there
  // are no suffix tokens to retain (we rescanned everything).
  u64 retained_suffix_token_start = edit.old_token_count; // sentinel: nothing
  u64 retained_suffix_count = 0;
  if (converge_line < total_new_lines) {
    retained_suffix_token_start = cache->lines[converge_line].first_token;
    retained_suffix_count = edit.old_token_count - retained_suffix_token_start;
  }

  // --- Splice token array: [prefix] [rescanned] [shifted suffix] ---
  u64 new_total_tokens = edit.prefix_token_end + rescan_count + retained_suffix_count;
  EnsureTokenCapacity(buffer, edit.prefix_token_end, new_total_tokens);

  // Shift and copy retained suffix tokens.  Source and destination may overlap
  // so copy in the safe direction.
  if (retained_suffix_count > 0) {
    u64 src = retained_suffix_token_start;
    u64 dst = edit.prefix_token_end + rescan_count;
    if (dst <= src) {
      for (u64 i = 0; i < retained_suffix_count; i += 1) {
        Token t = buffer->tokens.tokens[src + i];
        t.start = (u64)((i64)t.start + byte_delta);
        t.end   = (u64)((i64)t.end   + byte_delta);
        buffer->tokens.tokens[dst + i] = t;
      }
    } else {
      for (u64 i = retained_suffix_count; i > 0; i -= 1) {
        Token t = buffer->tokens.tokens[src + i - 1];
        t.start = (u64)((i64)t.start + byte_delta);
        t.end   = (u64)((i64)t.end   + byte_delta);
        buffer->tokens.tokens[dst + i - 1] = t;
      }
    }
  }

  // Copy rescanned tokens.
  for (u64 i = 0; i < rescan_count; i += 1) {
    buffer->tokens.tokens[edit.prefix_token_end + i] = rescan_tokens[i];
  }

  buffer->tokens.count = new_total_tokens;

  // --- Fix up first_token indices and update suffix incoming states. ---
  u64 running = 0;
  for (u64 line = 0; line < total_new_lines; line += 1) {
    cache->lines[line].first_token = running;
    running += cache->lines[line].token_count;
  }

  // If we converged, fix the incoming state of the convergence line to match
  // (it already should, but be explicit).
  if (converge_line < total_new_lines) {
    cache->lines[converge_line].incoming = state;
  }

  cache->lines_scanned_last_update = lines_scanned;
  ScratchEnd(scratch);
}
