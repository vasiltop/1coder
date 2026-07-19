#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_string.h"
#include "test.h"

#include <string.h>

TEST(arena_push_and_pop) {
  Arena *arena = ArenaAlloc(MB(1));
  CHECK(arena != nullptr);

  u64 start = ArenaPos(arena);

  u8 *a = PushArray(arena, u8, 16);
  CHECK(a != nullptr);
  CHECK(ArenaPos(arena) >= start + 16);

  // PushArray zeroes; PushArrayNoZero does not promise anything.
  for (u64 i = 0; i < 16; i += 1) CHECK_EQ(a[i], 0);

  u64 mid = ArenaPos(arena);
  u8 *b = PushArray(arena, u8, 4096);
  CHECK(b != nullptr);
  CHECK(b != a);

  ArenaPopTo(arena, mid);
  CHECK_EQ(ArenaPos(arena), mid);

  // Popping makes the space reusable.
  u8 *c = PushArray(arena, u8, 4096);
  CHECK_EQ((u64)c, (u64)b);

  ArenaClear(arena);
  CHECK_EQ(ArenaPos(arena), kArenaHeaderSize);

  ArenaRelease(arena);
}

TEST(arena_alignment) {
  Arena *arena = ArenaAlloc(MB(1));

  // Deliberately misalign, then confirm the next aligned push is honoured.
  (void)ArenaPush(arena, 1, 1);
  void *p16 = ArenaPush(arena, 8, 16);
  CHECK_EQ((u64)p16 % 16, 0);

  (void)ArenaPush(arena, 1, 1);
  void *p64 = ArenaPush(arena, 8, 64);
  CHECK_EQ((u64)p64 % 64, 0);

  ArenaRelease(arena);
}

TEST(arena_commits_past_granularity) {
  Arena *arena = ArenaAlloc(MB(8));

  // Cross several commit boundaries and write to every byte; a commit bug
  // surfaces as a segfault here rather than silently later.
  u64 size = MB(4);
  u8 *big = PushArrayNoZero(arena, u8, size);
  CHECK(big != nullptr);
  memset(big, 0xAB, size);
  CHECK_EQ(big[0], 0xAB);
  CHECK_EQ(big[size - 1], 0xAB);

  ArenaRelease(arena);
}

TEST(arena_temp_and_scratch) {
  Arena *arena = ArenaAlloc(MB(1));

  TempArena temp = TempBegin(arena);
  (void)PushArray(arena, u8, 256);
  CHECK(ArenaPos(arena) > temp.pos);
  TempEnd(temp);
  CHECK_EQ(ArenaPos(arena), temp.pos);

  // Scratch must not hand back the arena we declared as the output.
  TempArena s1 = ScratchBegin1(arena);
  CHECK(s1.arena != nullptr);
  CHECK(s1.arena != arena);

  TempArena s2 = ScratchBegin1(s1.arena);
  CHECK(s2.arena != nullptr);
  CHECK(s2.arena != s1.arena);

  ScratchEnd(s2);
  ScratchEnd(s1);
  ArenaRelease(arena);
}

TEST(string_basics) {
  CHECK_EQ(Str8Lit("hello").size, 5);
  CHECK_EQ(Str8C("hello").size, 5);
  CHECK_EQ(Str8C(nullptr).size, 0);

  CHECK(Str8Match(Str8Lit("abc"), Str8Lit("abc")));
  CHECK(!Str8Match(Str8Lit("abc"), Str8Lit("abd")));
  CHECK(!Str8Match(Str8Lit("abc"), Str8Lit("ab")));
  CHECK(Str8Match(Str8Lit("ABC"), Str8Lit("abc"), StringMatch::CaseInsensitive));

  CHECK(Str8StartsWith(Str8Lit("foobar"), Str8Lit("foo")));
  CHECK(!Str8StartsWith(Str8Lit("foobar"), Str8Lit("bar")));
  CHECK(Str8EndsWith(Str8Lit("foobar"), Str8Lit("bar")));
  CHECK(!Str8EndsWith(Str8Lit("foo"), Str8Lit("foobar")));
}

TEST(string_slicing_clamps) {
  String8 s = Str8Lit("hello world");

  CHECK_STR(Str8Prefix(s, 5), Str8Lit("hello"));
  CHECK_STR(Str8Suffix(s, 5), Str8Lit("world"));
  CHECK_STR(Str8Skip(s, 6), Str8Lit("world"));
  CHECK_STR(Str8Chop(s, 6), Str8Lit("hello"));
  CHECK_STR(Str8Substr(s, RangeU64{6, 11}), Str8Lit("world"));

  // Out-of-range slicing clamps instead of reading past the end.
  CHECK_EQ(Str8Prefix(s, 999).size, s.size);
  CHECK_EQ(Str8Skip(s, 999).size, 0);
  CHECK_EQ(Str8Chop(s, 999).size, 0);
  CHECK_EQ(Str8Substr(s, RangeU64{999, 1000}).size, 0);
  CHECK_EQ(Str8Substr(s, RangeU64{5, 2}).size, 0);

  CHECK_STR(Str8SkipChopWhitespace(Str8Lit("  \t hi \n ")), Str8Lit("hi"));
  CHECK_EQ(Str8SkipChopWhitespace(Str8Lit("   ")).size, 0);
}

TEST(string_find) {
  String8 s = Str8Lit("abcabc");

  CHECK_EQ(Str8FindFirstChar(s, 'c'), 2);
  CHECK_EQ(Str8FindFirstChar(s, 'c', 3), 5);
  CHECK_EQ(Str8FindFirstChar(s, 'z'), s.size);  // absent -> size
  CHECK_EQ(Str8FindLastChar(s, 'a'), 3);
  CHECK_EQ(Str8FindLastChar(s, 'z'), s.size);

  CHECK_EQ(Str8FindFirst(s, Str8Lit("bc")), 1);
  CHECK_EQ(Str8FindFirst(s, Str8Lit("bc"), 2), 4);
  CHECK_EQ(Str8FindFirst(s, Str8Lit("zz")), s.size);
  CHECK_EQ(Str8FindFirst(s, Str8Lit("")), s.size);
}

TEST(string_push) {
  Arena *arena = ArenaAlloc(MB(1));

  String8 copy = PushStr8Copy(arena, Str8Lit("hello"));
  CHECK_STR(copy, Str8Lit("hello"));
  CHECK_EQ(copy.str[copy.size], 0);  // null-terminated but not counted

  CHECK_STR(PushStr8Cat(arena, Str8Lit("foo"), Str8Lit("bar")), Str8Lit("foobar"));
  CHECK_STR(PushStr8F(arena, "%d-%s", 42, "x"), Str8Lit("42-x"));

  // A format longer than any fixed stack buffer would hold.
  String8 big = PushStr8F(arena, "%0500d", 7);
  CHECK_EQ(big.size, 500);

  ArenaRelease(arena);
}

TEST(string_list_and_split) {
  Arena *arena = ArenaAlloc(MB(1));

  String8List list = {};
  Str8ListPush(arena, &list, Str8Lit("a"));
  Str8ListPush(arena, &list, Str8Lit("b"));
  Str8ListPush(arena, &list, Str8Lit("c"));
  CHECK_EQ(list.node_count, 3);
  CHECK_EQ(list.total_size, 3);
  CHECK_STR(Str8ListJoin(arena, &list, Str8Lit(", ")), Str8Lit("a, b, c"));
  CHECK_STR(Str8ListJoin(arena, &list, Str8Lit("")), Str8Lit("abc"));

  String8List empty = {};
  CHECK_EQ(Str8ListJoin(arena, &empty, Str8Lit(",")).size, 0);

  // Empty elements still get separators around them, which matters whenever a
  // list is used to build lines: a blank first line must stay a blank line.
  String8List blanks = {};
  Str8ListPush(arena, &blanks, String8{nullptr, 0});
  Str8ListPush(arena, &blanks, Str8Lit("x"));
  Str8ListPush(arena, &blanks, String8{nullptr, 0});
  Str8ListPush(arena, &blanks, Str8Lit("y"));
  CHECK_STR(Str8ListJoin(arena, &blanks, Str8Lit("\n")), Str8Lit("\nx\n\ny"));

  // Empty runs are dropped, so repeated separators collapse.
  String8List parts = Str8SplitChar(arena, Str8Lit("a//b/"), '/');
  CHECK_EQ(parts.node_count, 2);
  CHECK_STR(parts.first->string, Str8Lit("a"));
  CHECK_STR(parts.first->next->string, Str8Lit("b"));

  String8List multi = Str8Split(arena, Str8Lit("a b\tc"), Str8Lit(" \t"));
  CHECK_EQ(multi.node_count, 3);

  ArenaRelease(arena);
}

TEST(string_paths) {
  CHECK_STR(Str8PathBase(Str8Lit("a/b/c.txt")), Str8Lit("c.txt"));
  CHECK_STR(Str8PathBase(Str8Lit("c.txt")), Str8Lit("c.txt"));
  CHECK_STR(Str8PathDir(Str8Lit("a/b/c.txt")), Str8Lit("a/b"));
  CHECK_EQ(Str8PathDir(Str8Lit("c.txt")).size, 0);
  CHECK_STR(Str8PathExt(Str8Lit("a/b/c.txt")), Str8Lit("txt"));
  CHECK_EQ(Str8PathExt(Str8Lit("a/b/c")).size, 0);
  // A leading dot marks a hidden file, not an extension.
  CHECK_EQ(Str8PathExt(Str8Lit(".bashrc")).size, 0);
}

TEST(utf8_decode_roundtrip) {
  // 1-, 2-, 3- and 4-byte encodings.
  u32 points[] = {'A', 0x00E9, 0x20AC, 0x1F600};
  u32 sizes[] = {1, 2, 3, 4};

  for (u64 i = 0; i < ArrayCount(points); i += 1) {
    u8 buf[4] = {};
    u32 n = Utf8Encode(buf, points[i]);
    CHECK_EQ(n, sizes[i]);
    CHECK_EQ(Utf8EncodedSize(points[i]), sizes[i]);

    DecodedCodepoint d = Utf8Decode(String8{buf, n}, 0);
    CHECK_EQ(d.codepoint, points[i]);
    CHECK_EQ(d.advance, n);
  }
}

TEST(utf8_decode_malformed) {
  // A bare continuation byte is not a valid lead.
  u8 lone_cont[] = {0x80};
  DecodedCodepoint d = Utf8Decode(String8{lone_cont, 1}, 0);
  CHECK_EQ(d.codepoint, 0xFFFD);
  CHECK_EQ(d.advance, 1);  // must still advance, or decoding loops forever

  // Truncated 3-byte sequence.
  u8 truncated[] = {0xE2, 0x82};
  CHECK_EQ(Utf8Decode(String8{truncated, 2}, 0).codepoint, 0xFFFD);

  // Overlong encoding of 'A'.
  u8 overlong[] = {0xC1, 0x81};
  CHECK_EQ(Utf8Decode(String8{overlong, 2}, 0).codepoint, 0xFFFD);

  // Surrogate half.
  u8 surrogate[] = {0xED, 0xA0, 0x80};
  CHECK_EQ(Utf8Decode(String8{surrogate, 3}, 0).codepoint, 0xFFFD);

  // Reading at or past the end yields advance 1 so scans terminate.
  CHECK_EQ(Utf8Decode(Str8Lit("ab"), 5).advance, 1);
}

TEST(utf8_decode_prev) {
  // "aé€" -- 1, 2 and 3 byte characters.
  String8 s = Str8Lit("a\xC3\xA9\xE2\x82\xAC");
  CHECK_EQ(s.size, 6);

  DecodedCodepoint d = Utf8DecodePrev(s, 6);
  CHECK_EQ(d.codepoint, 0x20AC);
  CHECK_EQ(d.advance, 3);

  d = Utf8DecodePrev(s, 3);
  CHECK_EQ(d.codepoint, 0x00E9);
  CHECK_EQ(d.advance, 2);

  d = Utf8DecodePrev(s, 1);
  CHECK_EQ(d.codepoint, 'a');
  CHECK_EQ(d.advance, 1);

  CHECK_EQ(Utf8DecodePrev(s, 0).advance, 0);
  CHECK_EQ(Utf8Length(s), 3);
}

TEST(math_ranges) {
  CHECK_EQ(RangeSize(RangeU64{2, 7}), 5);
  CHECK(RangeEmpty(RangeU64{3, 3}));
  CHECK(!RangeEmpty(RangeU64{3, 4}));

  // RangeMake normalises reversed input, which is how visual mode builds a
  // range when the cursor is before the anchor.
  RangeU64 r = RangeMake(9, 4);
  CHECK_EQ(r.min, 4);
  CHECK_EQ(r.max, 9);

  CHECK(RangeContains(RangeU64{2, 5}, 2));
  CHECK(!RangeContains(RangeU64{2, 5}, 5));  // half-open

  CHECK(RangeOverlaps(RangeU64{0, 5}, RangeU64{4, 9}));
  CHECK(!RangeOverlaps(RangeU64{0, 5}, RangeU64{5, 9}));

  RangeU64 isect = RangeIntersect(RangeU64{0, 5}, RangeU64{3, 9});
  CHECK_EQ(isect.min, 3);
  CHECK_EQ(isect.max, 5);
  CHECK(RangeEmpty(RangeIntersect(RangeU64{0, 2}, RangeU64{5, 9})));
}

TEST(math_rects) {
  RectS32 r = {2, 3, 12, 8};
  CHECK_EQ(RectWidth(r), 10);
  CHECK_EQ(RectHeight(r), 5);
  CHECK(RectContains(r, 2, 3));
  CHECK(!RectContains(r, 12, 8));  // exclusive far edge
  CHECK(!RectEmpty(r));
  CHECK(RectEmpty(RectS32{0, 0, 0, 5}));
}

TEST(enum_flag_ops) {
  StringMatch f = StringMatch::CaseInsensitive | StringMatch::RightSideSloppy;
  CHECK(HasFlag(f, StringMatch::CaseInsensitive));
  CHECK(HasFlag(f, StringMatch::RightSideSloppy));
  CHECK(HasAny(f, StringMatch::CaseInsensitive));

  // None is not "present" in anything, including itself.
  CHECK(!HasFlag(StringMatch::None, StringMatch::None));
  CHECK(!HasFlag(StringMatch::CaseInsensitive, StringMatch::RightSideSloppy));

  SetFlag(f, StringMatch::CaseInsensitive, false);
  CHECK(!HasFlag(f, StringMatch::CaseInsensitive));
  CHECK(HasFlag(f, StringMatch::RightSideSloppy));
}
