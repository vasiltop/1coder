#include "text/token.h"

u64 TokenIndexAtOffset(const TokenArray *tokens, u64 offset) {
  u64 lo = 0, hi = tokens->count;
  while (lo < hi) {
    u64 mid = lo + (hi - lo) / 2;
    if (tokens->tokens[mid].end <= offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

TokenKind TokenKindAtOffset(const TokenArray *tokens, u64 offset) {
  u64 index = TokenIndexAtOffset(tokens, offset);
  if (index >= tokens->count) return TokenKind::Default;

  // The token found ends after `offset`, but gaps between tokens are legal.
  const Token *token = &tokens->tokens[index];
  if (offset < token->start) return TokenKind::Default;
  return token->kind;
}
