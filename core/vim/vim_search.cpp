#include "vim/vim_search.h"

bool SearchPatternIsCaseSensitive(String8 pattern) {
  for (u64 i = 0; i < pattern.size; i += 1) {
    if (CharIsUpper(pattern.str[i])) return true;
  }
  return false;
}

namespace {

StringMatch SearchFlags(String8 pattern) {
  return SearchPatternIsCaseSensitive(pattern) ? StringMatch::None : StringMatch::CaseInsensitive;
}

// The last match starting strictly before `limit`, or `text.size` for none.
u64 FindLastBefore(String8 text, String8 pattern, u64 limit, StringMatch flags) {
  u64 best = text.size;
  for (u64 at = Str8FindFirst(text, pattern, 0, flags); at < text.size && at < limit;
       at = Str8FindFirst(text, pattern, at + 1, flags)) {
    best = at;
  }
  return best;
}

}  // namespace

SearchHit BufferSearch(const Buffer *buffer, String8 pattern, u64 from, bool forward, bool wrap) {
  SearchHit miss = {0, false, false};
  if (pattern.size == 0) return miss;

  TempArena scratch = ScratchBegin();
  String8 text = BufferTextAll(scratch.arena, buffer);
  StringMatch flags = SearchFlags(pattern);

  SearchHit hit = miss;
  if (forward) {
    // Strictly after the cursor, so repeating the search always advances.
    u64 start = (from + 1 < text.size) ? from + 1 : text.size;
    u64 at = Str8FindFirst(text, pattern, start, flags);
    if (at < text.size) {
      hit = SearchHit{at, true, false};
    } else if (wrap) {
      at = Str8FindFirst(text, pattern, 0, flags);
      if (at < text.size) hit = SearchHit{at, true, true};
    }
  } else {
    u64 at = FindLastBefore(text, pattern, from, flags);
    if (at < text.size) {
      hit = SearchHit{at, true, false};
    } else if (wrap) {
      at = FindLastBefore(text, pattern, text.size, flags);
      if (at < text.size) hit = SearchHit{at, true, true};
    }
  }

  ScratchEnd(scratch);
  return hit;
}

u64 BufferSearchAll(const Buffer *buffer, String8 pattern, RangeU64 range, RangeU64 *out_hits,
                    u64 max_hits) {
  if (pattern.size == 0 || max_hits == 0) return 0;

  TempArena scratch = ScratchBegin();
  String8 text = BufferTextAll(scratch.arena, buffer);
  StringMatch flags = SearchFlags(pattern);

  u64 count = 0;
  u64 at = Str8FindFirst(text, pattern, Min(range.min, text.size), flags);
  while (at < text.size && at < range.max && count < max_hits) {
    out_hits[count] = RangeU64{at, at + pattern.size};
    count += 1;
    at = Str8FindFirst(text, pattern, at + 1, flags);
  }

  ScratchEnd(scratch);
  return count;
}

String8 BufferWordAtCursor(Arena *arena, const Buffer *buffer, u64 pos) {
  u64 size = BufferSize(buffer);
  if (pos >= size) return String8{nullptr, 0};

  TempArena scratch = ScratchBegin1(arena);
  String8 text = BufferTextAll(scratch.arena, buffer);

  // Vim scans forward to the first word character on the line, so `*` works
  // with the cursor sitting in the indentation before an identifier.
  u64 start = pos;
  while (start < text.size && text.str[start] != '\n' && !CharIsWord(text.str[start])) {
    start += 1;
  }
  if (start >= text.size || text.str[start] == '\n') {
    ScratchEnd(scratch);
    return String8{nullptr, 0};
  }

  while (start > 0 && CharIsWord(text.str[start - 1])) start -= 1;
  u64 end = start;
  while (end < text.size && CharIsWord(text.str[end])) end += 1;

  String8 word = PushStr8Copy(arena, String8{text.str + start, end - start});
  ScratchEnd(scratch);
  return word;
}
