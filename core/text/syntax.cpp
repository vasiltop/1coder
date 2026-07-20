#include "text/syntax.h"

#include "editor/buffer.h"

namespace {

constexpr u64 kSyntaxArenaReserve = MB(64);
constexpr u64 kSyntaxInitialLineCapacity = 256;
constexpr u64 kSyntaxInitialTokenCapacity = 256;

[[nodiscard]] bool MatchAt(const Buffer *buffer, u64 pos, u64 end, String8 pattern) {
  if (pattern.size == 0 || pos + pattern.size > end) return false;
  for (u64 k = 0; k < pattern.size; k += 1) {
    if (BufferByteAt(buffer, pos + k) != pattern.str[k]) return false;
  }
  return true;
}

[[nodiscard]] bool IdentifierEquals(const Buffer *buffer, u64 start, u64 end, String8 word) {
  if (end - start != word.size) return false;
  for (u64 k = 0; k < word.size; k += 1) {
    if (BufferByteAt(buffer, start + k) != word.str[k]) return false;
  }
  return true;
}

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

// Rebuild tracks live tokens locally, so callers pass the count to preserve.
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

void AppendToken(Buffer *buffer, u64 *token_count, u64 start, u64 end, TokenKind kind) {
  if (end <= start) return;
  EnsureTokenCapacity(buffer, *token_count, *token_count + 1);
  buffer->tokens.tokens[*token_count] = Token{start, end, kind};
  *token_count += 1;
}

[[nodiscard]] TokenKind ClassifyIdentifier(const Buffer *buffer, const LanguageDefinition *lang,
                                            u64 start, u64 end, u64 line_end) {
  for (u64 g = 0; g < lang->keyword_group_count; g += 1) {
    const KeywordGroup *group = &lang->keyword_groups[g];
    for (u64 w = 0; w < group->count; w += 1) {
      if (IdentifierEquals(buffer, start, end, group->words[w])) return group->kind;
    }
  }

  u64 p = end;
  while (p < line_end && CharIsSpace(BufferByteAt(buffer, p))) p += 1;
  if (p < line_end && BufferByteAt(buffer, p) == '(') return TokenKind::Function;

  return TokenKind::Default;
}

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

[[nodiscard]] SyntaxState ScanLine(Buffer *buffer, const LanguageDefinition *lang, RangeU64 range,
                                    SyntaxState incoming, u64 *token_count) {
  u64 i = range.min;
  const u64 line_start = range.min;
  const u64 end = range.max;
  SyntaxMode mode = incoming.mode;

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

  // A directive is valid only before any other token on the physical line.
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

    if (c == '"') {
      u64 close_end = 0;
      bool closed = FindClose(buffer, i + 1, end, Str8Lit("\""), true, &close_end);
      u64 token_end = closed ? close_end : end;
      AppendToken(buffer, token_count, i, token_end, TokenKind::String);
      i = token_end;
      continue;
    }

    if (c == '\'') {
      u64 close_end = 0;
      bool closed = FindClose(buffer, i + 1, end, Str8Lit("'"), true, &close_end);
      u64 token_end = closed ? close_end : end;
      TokenKind kind = lang->single_quote_is_character ? TokenKind::Character : TokenKind::String;
      AppendToken(buffer, token_count, i, token_end, kind);
      i = token_end;
      continue;
    }

    if (CharIsDigit(c) || (c == '.' && i + 1 < end && CharIsDigit(BufferByteAt(buffer, i + 1)))) {
      u64 number_end = ScanNumber(buffer, i, end);
      AppendToken(buffer, token_count, i, number_end, TokenKind::Number);
      i = number_end;
      continue;
    }

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

    if (IsOperatorByte(c)) {
      u64 op_end = i + 1;
      while (op_end < end && IsOperatorByte(BufferByteAt(buffer, op_end))) op_end += 1;
      AppendToken(buffer, token_count, i, op_end, TokenKind::Operator);
      i = op_end;
      continue;
    }

    if (IsPunctuationByte(c)) {
      AppendToken(buffer, token_count, i, i + 1, TokenKind::Punctuation);
      i += 1;
      continue;
    }

    i += 1;
  }

  return SyntaxState{SyntaxMode::Default};
}

}  // namespace

void SyntaxAttach(Buffer *buffer, String8 path) {
  const LanguageDefinition *lang = SyntaxLanguageForPath(path);
  SyntaxCache *cache = &buffer->syntax;

  if (cache->arena != nullptr && cache->language == lang) return;

  if (cache->arena == nullptr) {
    cache->arena = ArenaAlloc(kSyntaxArenaReserve);
  } else {
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

  edit.start_state = (edit.old_start_line == 0)
    ? SyntaxState{SyntaxMode::Default}
    : cache->lines[edit.old_start_line - 1].outgoing;

  edit.prefix_token_end = cache->lines[edit.old_start_line].first_token;

  edit.old_token_count = buffer->tokens.count;

  return edit;
}

void SyntaxEndEdit(Buffer *buffer, SyntaxEdit edit) {
  SyntaxCache *cache = &buffer->syntax;
  if (cache->arena == nullptr || cache->language == nullptr) return;

  u64 new_size = BufferSize(buffer);
  u64 new_line_count = BufferLineCount(buffer);

  i64 byte_delta = (i64)new_size - (i64)edit.old_size;

  i64 line_delta = (i64)new_line_count - (i64)edit.old_line_count;
  u64 old_edited_lines = edit.old_end_line - edit.old_start_line + 1;
  u64 new_edited_lines = (u64)((i64)old_edited_lines + line_delta);

  u64 old_suffix_lines = (edit.old_end_line + 1 < edit.old_line_count)
    ? (edit.old_line_count - edit.old_end_line - 1) : 0;
  u64 total_new_lines = edit.old_start_line + new_edited_lines + old_suffix_lines;

  EnsureLineCapacity(cache, total_new_lines);

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

  TempArena scratch = ScratchBegin(&cache->arena, 1);

  u64 rescan_cap = 256;
  Token *rescan_tokens = PushArrayNoZero(scratch.arena, Token, rescan_cap);
  u64 rescan_count = 0;

  SyntaxState state = edit.start_state;
  u64 lines_scanned = 0;
  u64 converge_line = total_new_lines;

  for (u64 line = edit.old_start_line; line < total_new_lines; line += 1) {
    if (line >= new_suffix_idx) {
      if (state.mode == cache->lines[line].incoming.mode) {
        converge_line = line;
        break;
      }
    }

    RangeU64 range = BufferLineRange(buffer, line);
    SyntaxState incoming = state;
    u64 first_tok = rescan_count;

    // Reuse ScanLine with scratch token storage.
    Token *saved_tokens = buffer->tokens.tokens;
    u64 saved_count_val = buffer->tokens.count;
    u64 saved_cap = cache->token_capacity;

    buffer->tokens.tokens = rescan_tokens;
    cache->token_capacity = rescan_cap;

    u64 local_count = rescan_count;
    state = ScanLine(buffer, cache->language, range, incoming, &local_count);

    rescan_tokens = buffer->tokens.tokens;
    rescan_cap = cache->token_capacity;
    rescan_count = local_count;

    buffer->tokens.tokens = saved_tokens;
    buffer->tokens.count = saved_count_val;
    cache->token_capacity = saved_cap;

    cache->lines[line] = SyntaxLineCache{incoming, state, 0, rescan_count - first_tok};
    lines_scanned += 1;
  }

  u64 retained_suffix_token_start = edit.old_token_count;
  u64 retained_suffix_count = 0;
  if (converge_line < total_new_lines) {
    retained_suffix_token_start = cache->lines[converge_line].first_token;
    retained_suffix_count = edit.old_token_count - retained_suffix_token_start;
  }

  u64 new_total_tokens = edit.prefix_token_end + rescan_count + retained_suffix_count;

  // The retained suffix is still read from old indices after growth.
  EnsureTokenCapacity(buffer, edit.old_token_count, new_total_tokens);

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

  for (u64 i = 0; i < rescan_count; i += 1) {
    buffer->tokens.tokens[edit.prefix_token_end + i] = rescan_tokens[i];
  }

  buffer->tokens.count = new_total_tokens;

  u64 running = 0;
  for (u64 line = 0; line < total_new_lines; line += 1) {
    cache->lines[line].first_token = running;
    running += cache->lines[line].token_count;
  }

  if (converge_line < total_new_lines) {
    cache->lines[converge_line].incoming = state;
  }

  cache->lines_scanned_last_update = lines_scanned;
  ScratchEnd(scratch);
}
