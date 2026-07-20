#include "base/base_string.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

String8 Str8C(const char *cstr) {
  if (!cstr) return String8{nullptr, 0};
  return String8{(u8 *)cstr, (u64)strlen(cstr)};
}

String8 Str8Range(u8 *first, u8 *one_past_last) {
  Assert(one_past_last >= first);
  return String8{first, (u64)(one_past_last - first)};
}

bool Str8Match(String8 a, String8 b, StringMatch flags) {
  bool sloppy = HasFlag(flags, StringMatch::RightSideSloppy);
  if (a.size != b.size && !sloppy) return false;
  if (sloppy && b.size > a.size) return false;

  u64 n = Min(a.size, b.size);
  bool ci = HasFlag(flags, StringMatch::CaseInsensitive);
  for (u64 i = 0; i < n; i += 1) {
    u8 ca = a.str[i], cb = b.str[i];
    if (ci) {
      ca = CharToLower(ca);
      cb = CharToLower(cb);
    }
    if (ca != cb) return false;
  }
  return true;
}

String8 Str8Substr(String8 s, RangeU64 range) {
  u64 min = Min(range.min, s.size);
  u64 max = Clamp(min, range.max, s.size);
  return String8{s.str + min, max - min};
}

String8 Str8Prefix(String8 s, u64 size) { return String8{s.str, Min(size, s.size)}; }

String8 Str8Suffix(String8 s, u64 size) {
  u64 n = Min(size, s.size);
  return String8{s.str + (s.size - n), n};
}

String8 Str8Skip(String8 s, u64 amount) {
  u64 n = Min(amount, s.size);
  return String8{s.str + n, s.size - n};
}

String8 Str8Chop(String8 s, u64 amount) {
  u64 n = Min(amount, s.size);
  return String8{s.str, s.size - n};
}

String8 Str8SkipChopWhitespace(String8 s) {
  u64 start = 0;
  while (start < s.size && CharIsSpace(s.str[start])) start += 1;
  u64 end = s.size;
  while (end > start && CharIsSpace(s.str[end - 1])) end -= 1;
  return String8{s.str + start, end - start};
}

bool Str8StartsWith(String8 s, String8 prefix, StringMatch flags) {
  return Str8Match(s, prefix, flags | StringMatch::RightSideSloppy);
}

bool Str8EndsWith(String8 s, String8 suffix, StringMatch flags) {
  if (suffix.size > s.size) return false;
  return Str8Match(Str8Suffix(s, suffix.size), suffix, flags);
}

u64 Str8FindFirst(String8 s, String8 needle, u64 start, StringMatch flags) {
  if (needle.size == 0 || needle.size > s.size) return s.size;
  for (u64 i = start; i + needle.size <= s.size; i += 1) {
    if (Str8Match(String8{s.str + i, needle.size}, needle, flags)) return i;
  }
  return s.size;
}

u64 Str8FindFirstChar(String8 s, u8 c, u64 start) {
  for (u64 i = start; i < s.size; i += 1) {
    if (s.str[i] == c) return i;
  }
  return s.size;
}

u64 Str8FindLastChar(String8 s, u8 c) {
  for (u64 i = s.size; i > 0; i -= 1) {
    if (s.str[i - 1] == c) return i - 1;
  }
  return s.size;
}

String8 PushStr8Copy(Arena *arena, String8 s) {
  // Always null-terminate the copy without counting the terminator in size, so
  // an arena-owned String8 can be handed to a C API after a cast.
  u8 *dst = PushArrayNoZero(arena, u8, s.size + 1);
  if (s.size) memcpy(dst, s.str, s.size);
  dst[s.size] = 0;
  return String8{dst, s.size};
}

String8 PushStr8Cat(Arena *arena, String8 a, String8 b) {
  u8 *dst = PushArrayNoZero(arena, u8, a.size + b.size + 1);
  if (a.size) memcpy(dst, a.str, a.size);
  if (b.size) memcpy(dst + a.size, b.str, b.size);
  dst[a.size + b.size] = 0;
  return String8{dst, a.size + b.size};
}

String8 PushStr8F(Arena *arena, const char *fmt, ...) {
  va_list args, args_copy;
  va_start(args, fmt);
  va_copy(args_copy, args);

  int needed = vsnprintf(nullptr, 0, fmt, args);
  va_end(args);

  if (needed < 0) {
    va_end(args_copy);
    return String8{nullptr, 0};
  }

  u8 *dst = PushArrayNoZero(arena, u8, (u64)needed + 1);
  vsnprintf((char *)dst, (size_t)needed + 1, fmt, args_copy);
  va_end(args_copy);

  return String8{dst, (u64)needed};
}

const char *PushCStr(Arena *arena, String8 s) {
  return (const char *)PushStr8Copy(arena, s).str;
}

void Str8ListPush(Arena *arena, String8List *list, String8 s) {
  String8Node *node = PushStruct(arena, String8Node);
  node->string = s;
  if (list->last) {
    list->last->next = node;
    list->last = node;
  } else {
    list->first = list->last = node;
  }
  list->node_count += 1;
  list->total_size += s.size;
}

String8 Str8ListJoin(Arena *arena, String8List *list, String8 sep) {
  if (list->node_count == 0) return String8{nullptr, 0};

  u64 total = list->total_size + sep.size * (list->node_count - 1);
  u8 *dst = PushArrayNoZero(arena, u8, total + 1);

  u64 off = 0;
  bool first = true;
  for (String8Node *n = list->first; n; n = n->next) {
    // Keyed off "is this the first node", not off the output position: an empty
    // leading element leaves the offset at zero, which would swallow the
    // separator that should follow it.
    if (!first && sep.size) {
      memcpy(dst + off, sep.str, sep.size);
      off += sep.size;
    }
    first = false;
    if (n->string.size) memcpy(dst + off, n->string.str, n->string.size);
    off += n->string.size;
  }
  dst[total] = 0;
  return String8{dst, total};
}

String8List Str8Split(Arena *arena, String8 s, String8 splitters) {
  String8List list = {};
  u64 start = 0;
  for (u64 i = 0; i <= s.size; i += 1) {
    bool is_split = false;
    if (i < s.size) {
      for (u64 j = 0; j < splitters.size; j += 1) {
        if (s.str[i] == splitters.str[j]) {
          is_split = true;
          break;
        }
      }
    }
    // Empty runs are dropped, so "a//b" splits into two parts, not three.
    if (is_split || i == s.size) {
      if (i > start) Str8ListPush(arena, &list, String8{s.str + start, i - start});
      start = i + 1;
    }
  }
  return list;
}

String8List Str8SplitChar(Arena *arena, String8 s, u8 splitter) {
  return Str8Split(arena, s, String8{&splitter, 1});
}

String8 Str8PathBase(String8 path) {
  u64 slash = Str8FindLastChar(path, '/');
#if defined(_WIN32)
  u64 back = Str8FindLastChar(path, '\\');
  if (back != path.size && (slash == path.size || back > slash)) slash = back;
#endif
  return (slash == path.size) ? path : Str8Skip(path, slash + 1);
}

String8 Str8PathDir(String8 path) {
  u64 slash = Str8FindLastChar(path, '/');
#if defined(_WIN32)
  u64 back = Str8FindLastChar(path, '\\');
  if (back != path.size && (slash == path.size || back > slash)) slash = back;
#endif
  if (slash == path.size) return String8{nullptr, 0};

#if defined(_WIN32)
  // "C:/" is a drive root and has no parent, so it answers empty the way "/"
  // does. The prefix would otherwise be "C:", which names the current directory
  // on that drive rather than its root -- and the explorer would treat it as
  // one more level to walk up into.
  if (slash == 2 && path.str[1] == ':') return String8{nullptr, 0};
#endif

  return Str8Prefix(path, slash);
}

String8 Str8PathExt(String8 path) {
  String8 base = Str8PathBase(path);
  u64 dot = Str8FindLastChar(base, '.');
  // A leading dot is a hidden file, not an extension.
  if (dot == base.size || dot == 0) return String8{nullptr, 0};
  return Str8Skip(base, dot + 1);
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

namespace {

constexpr u32 kReplacement = 0xFFFD;

// Bytes in the sequence introduced by each lead byte; 0 marks an invalid lead.
constexpr u8 kUtf8ClassFromLead[32] = {
    1, 1, 1, 1, 1, 1, 1, 1,  // 0xxxxxxx
    1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0,  // 10xxxxxx - continuation, invalid as lead
    2, 2, 2, 2,              // 110xxxxx
    3, 3,                    // 1110xxxx
    4,                       // 11110xxx
    0,                       // 11111xxx
};

}  // namespace

DecodedCodepoint Utf8Decode(String8 s, u64 offset) {
  if (offset >= s.size) return DecodedCodepoint{0, 1};

  u8 lead = s.str[offset];
  u8 len = kUtf8ClassFromLead[lead >> 3];
  u64 remaining = s.size - offset;

  if (len == 0 || len > remaining) return DecodedCodepoint{kReplacement, 1};

  u32 cp = 0;
  switch (len) {
    case 1: return DecodedCodepoint{lead, 1};
    case 2: cp = lead & 0x1F; break;
    case 3: cp = lead & 0x0F; break;
    case 4: cp = lead & 0x07; break;
    default: Unreachable();
  }

  for (u8 i = 1; i < len; i += 1) {
    u8 b = s.str[offset + i];
    if (!Utf8IsContinuation(b)) return DecodedCodepoint{kReplacement, 1};
    cp = (cp << 6) | (b & 0x3F);
  }

  // Reject overlong encodings, surrogates and out-of-range values; accepting
  // them would let two byte sequences denote the same character.
  bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                  (len == 4 && cp < 0x10000);
  if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return DecodedCodepoint{kReplacement, 1};
  }

  return DecodedCodepoint{cp, len};
}

DecodedCodepoint Utf8DecodePrev(String8 s, u64 offset) {
  if (offset == 0) return DecodedCodepoint{0, 0};

  u64 start = offset - 1;
  u64 limit = (offset >= 4) ? offset - 4 : 0;
  while (start > limit && Utf8IsContinuation(s.str[start])) start -= 1;

  DecodedCodepoint d = Utf8Decode(s, start);
  // If the scan landed somewhere that doesn't decode up to `offset`, treat the
  // single preceding byte as malformed rather than reporting a bogus advance.
  if (start + d.advance != offset) return DecodedCodepoint{kReplacement, 1};
  return d;
}

u32 Utf8EncodedSize(u32 codepoint) {
  if (codepoint < 0x80) return 1;
  if (codepoint < 0x800) return 2;
  if (codepoint < 0x10000) return 3;
  if (codepoint <= 0x10FFFF) return 4;
  return 3;  // encodes as U+FFFD
}

u32 Utf8Encode(u8 *out, u32 codepoint) {
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    codepoint = kReplacement;
  }

  if (codepoint < 0x80) {
    out[0] = (u8)codepoint;
    return 1;
  }
  if (codepoint < 0x800) {
    out[0] = (u8)(0xC0 | (codepoint >> 6));
    out[1] = (u8)(0x80 | (codepoint & 0x3F));
    return 2;
  }
  if (codepoint < 0x10000) {
    out[0] = (u8)(0xE0 | (codepoint >> 12));
    out[1] = (u8)(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = (u8)(0x80 | (codepoint & 0x3F));
    return 3;
  }
  out[0] = (u8)(0xF0 | (codepoint >> 18));
  out[1] = (u8)(0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = (u8)(0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = (u8)(0x80 | (codepoint & 0x3F));
  return 4;
}

u64 Utf8Length(String8 s) {
  u64 count = 0;
  for (u64 i = 0; i < s.size;) {
    i += Utf8Decode(s, i).advance;
    count += 1;
  }
  return count;
}
