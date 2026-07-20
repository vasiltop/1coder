#pragma once

#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_types.h"

// Non-owning view of UTF-8 bytes. Ownership is always an arena's job, which is
// why every function that produces new bytes takes the arena explicitly.
struct String8 {
  u8 *str;
  u64 size;
};

struct String8Node {
  String8Node *next;
  String8 string;
};

struct String8List {
  String8Node *first;
  String8Node *last;
  u64 node_count;
  u64 total_size;
};

#define Str8Lit(s) String8{(u8 *)(s), sizeof(s) - 1}
#define Str8LitComp(s) {(u8 *)(s), sizeof(s) - 1}

[[nodiscard]] inline String8 Str8(u8 *str, u64 size) { return String8{str, size}; }
[[nodiscard]] String8 Str8C(const char *cstr);
[[nodiscard]] String8 Str8Range(u8 *first, u8 *one_past_last);

enum class StringMatch : u8 {
  None = 0,
  CaseInsensitive = 1,
  RightSideSloppy = 2,  // b only needs to match a's prefix
};
ENUM_FLAG_OPS(StringMatch)

[[nodiscard]] bool Str8Match(String8 a, String8 b, StringMatch flags = StringMatch::None);

// Substring views. All clamp rather than assert, so callers can do arithmetic
// near the edges without guarding every index.
[[nodiscard]] String8 Str8Substr(String8 s, RangeU64 range);
[[nodiscard]] String8 Str8Prefix(String8 s, u64 size);
[[nodiscard]] String8 Str8Suffix(String8 s, u64 size);
[[nodiscard]] String8 Str8Skip(String8 s, u64 amount);
[[nodiscard]] String8 Str8Chop(String8 s, u64 amount);
[[nodiscard]] String8 Str8SkipChopWhitespace(String8 s);

[[nodiscard]] bool Str8StartsWith(String8 s, String8 prefix, StringMatch flags = StringMatch::None);
[[nodiscard]] bool Str8EndsWith(String8 s, String8 suffix, StringMatch flags = StringMatch::None);

// Returns index of first match, or s.size when absent.
[[nodiscard]] u64 Str8FindFirst(String8 s, String8 needle, u64 start = 0,
                                StringMatch flags = StringMatch::None);
[[nodiscard]] u64 Str8FindFirstChar(String8 s, u8 c, u64 start = 0);
[[nodiscard]] u64 Str8FindLastChar(String8 s, u8 c);

[[nodiscard]] String8 PushStr8Copy(Arena *arena, String8 s);
[[nodiscard]] String8 PushStr8Cat(Arena *arena, String8 a, String8 b);
#if defined(__GNUC__) || defined(__clang__)
#  define PrintfFormat(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#  define PrintfFormat(fmt_idx, args_idx)
#endif

[[nodiscard]] String8 PushStr8F(Arena *arena, const char *fmt, ...) PrintfFormat(2, 3);
// Null-terminated copy, for handing paths to OS APIs.
[[nodiscard]] const char *PushCStr(Arena *arena, String8 s);

void Str8ListPush(Arena *arena, String8List *list, String8 s);
[[nodiscard]] String8 Str8ListJoin(Arena *arena, String8List *list, String8 sep);
[[nodiscard]] String8List Str8Split(Arena *arena, String8 s, String8 splitters);
[[nodiscard]] String8List Str8SplitChar(Arena *arena, String8 s, u8 splitter);

// Path helpers, used by the file buffer and later by the explorer.
[[nodiscard]] String8 Str8PathBase(String8 path);       // "a/b/c.txt" -> "c.txt"
[[nodiscard]] String8 Str8PathDir(String8 path);        // "a/b/c.txt" -> "a/b"
// A path with no parent: "/" on POSIX, "C:/" on Windows. Note that "C:" is not
// one -- it names the current directory on that drive, not the drive's root.
[[nodiscard]] bool Str8PathIsRoot(String8 path);
[[nodiscard]] String8 Str8PathExt(String8 path);        // "a/b/c.txt" -> "txt"

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

struct DecodedCodepoint {
  u32 codepoint;
  u32 advance;  // bytes consumed; always >= 1 so decoding cannot stall
};

// Decodes at `offset`. Malformed bytes decode to U+FFFD with advance 1.
[[nodiscard]] DecodedCodepoint Utf8Decode(String8 s, u64 offset);
[[nodiscard]] DecodedCodepoint Utf8DecodePrev(String8 s, u64 offset);
// Writes up to 4 bytes, returns count written.
[[nodiscard]] u32 Utf8Encode(u8 *out, u32 codepoint);
[[nodiscard]] u32 Utf8EncodedSize(u32 codepoint);
[[nodiscard]] inline bool Utf8IsContinuation(u8 b) { return (b & 0xC0) == 0x80; }
// Number of codepoints, not bytes.
[[nodiscard]] u64 Utf8Length(String8 s);

// ---------------------------------------------------------------------------
// Character classes (ASCII), shared by vim motions and the command parser.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool CharIsSpace(u8 c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
[[nodiscard]] constexpr bool CharIsDigit(u8 c) { return c >= '0' && c <= '9'; }
[[nodiscard]] constexpr bool CharIsUpper(u8 c) { return c >= 'A' && c <= 'Z'; }
[[nodiscard]] constexpr bool CharIsLower(u8 c) { return c >= 'a' && c <= 'z'; }
[[nodiscard]] constexpr bool CharIsAlpha(u8 c) { return CharIsUpper(c) || CharIsLower(c); }
[[nodiscard]] constexpr bool CharIsAlnum(u8 c) { return CharIsAlpha(c) || CharIsDigit(c); }
// Vim's notion of a "word" character: alphanumeric, underscore, or non-ASCII.
[[nodiscard]] constexpr bool CharIsWord(u8 c) { return CharIsAlnum(c) || c == '_' || c >= 0x80; }
[[nodiscard]] constexpr u8 CharToLower(u8 c) { return CharIsUpper(c) ? (u8)(c + 32) : c; }
[[nodiscard]] constexpr u8 CharToUpper(u8 c) { return CharIsLower(c) ? (u8)(c - 32) : c; }
