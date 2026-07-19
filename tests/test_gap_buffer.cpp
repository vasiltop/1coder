#include "test.h"
#include "text/gap_buffer.h"

#include <string.h>

namespace {

// Every gap buffer owns its arena exclusively, so each fixture gets its own.
struct Fixture {
  Arena *arena;
  GapBuffer gb;
};

Fixture MakeFixture(String8 initial, u64 capacity = kGapBufferMinCapacity) {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(64));
  if (initial.size) {
    GapBufferInitFrom(&f.gb, f.arena, initial);
  } else {
    GapBufferInit(&f.gb, f.arena, capacity);
  }
  return f;
}

void Destroy(Fixture *f) { ArenaRelease(f->arena); }

// Reads the buffer back through the byte-at-a-time path, which exercises the
// logical->physical mapping independently of the bulk copy path.
String8 ReadByBytes(Arena *arena, const GapBuffer *gb) {
  u64 n = GapBufferSize(gb);
  u8 *dst = PushArrayNoZero(arena, u8, n + 1);
  for (u64 i = 0; i < n; i += 1) dst[i] = GapBufferByteAt(gb, i);
  dst[n] = 0;
  return String8{dst, n};
}

}  // namespace

TEST(gap_buffer_starts_empty) {
  Fixture f = MakeFixture(String8{nullptr, 0});
  CHECK_EQ(GapBufferSize(&f.gb), 0);
  CHECK_EQ(GapBufferByteAt(&f.gb, 0), 0);  // out of range reads as 0
  CHECK_EQ(GapBufferCopyAll(f.arena, &f.gb).size, 0);
  Destroy(&f);
}

TEST(gap_buffer_init_from_text) {
  Fixture f = MakeFixture(Str8Lit("hello"));
  CHECK_EQ(GapBufferSize(&f.gb), 5);
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("hello"));
  CHECK_STR(ReadByBytes(f.arena, &f.gb), Str8Lit("hello"));
  Destroy(&f);
}

TEST(gap_buffer_insert_positions) {
  Fixture f = MakeFixture(String8{nullptr, 0});

  GapBufferInsert(&f.gb, 0, Str8Lit("world"));
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("world"));

  GapBufferInsert(&f.gb, 0, Str8Lit("hello "));  // at start
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("hello world"));

  GapBufferInsert(&f.gb, 11, Str8Lit("!"));  // at end
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("hello world!"));

  GapBufferInsert(&f.gb, 5, Str8Lit(","));  // in the middle
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("hello, world!"));

  // Reading through both paths must agree regardless of where the gap sits.
  CHECK_STR(ReadByBytes(f.arena, &f.gb), Str8Lit("hello, world!"));

  GapBufferInsert(&f.gb, 3, String8{nullptr, 0});  // empty insert is a no-op
  CHECK_EQ(GapBufferSize(&f.gb), 13);

  Destroy(&f);
}

TEST(gap_buffer_delete_positions) {
  Fixture f = MakeFixture(Str8Lit("hello, world!"));

  GapBufferDelete(&f.gb, RangeU64{5, 7});  // middle
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("helloworld!"));

  GapBufferDelete(&f.gb, RangeU64{10, 11});  // end
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("helloworld"));

  GapBufferDelete(&f.gb, RangeU64{0, 5});  // start
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("world"));
  CHECK_STR(ReadByBytes(f.arena, &f.gb), Str8Lit("world"));

  Destroy(&f);
}

TEST(gap_buffer_delete_clamps) {
  Fixture f = MakeFixture(Str8Lit("abc"));

  GapBufferDelete(&f.gb, RangeU64{2, 2});  // empty range
  CHECK_EQ(GapBufferSize(&f.gb), 3);

  GapBufferDelete(&f.gb, RangeU64{5, 9});  // wholly past the end
  CHECK_EQ(GapBufferSize(&f.gb), 3);

  GapBufferDelete(&f.gb, RangeU64{1, 999});  // straddling the end
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("a"));

  GapBufferDelete(&f.gb, RangeU64{0, 999});
  CHECK_EQ(GapBufferSize(&f.gb), 0);

  Destroy(&f);
}

TEST(gap_buffer_gap_moves_both_directions) {
  Fixture f = MakeFixture(Str8Lit("abcdefghij"));

  // Alternate edits at opposite ends to force the gap to travel each way.
  GapBufferInsert(&f.gb, 10, Str8Lit("Z"));
  GapBufferInsert(&f.gb, 0, Str8Lit("A"));
  GapBufferInsert(&f.gb, 6, Str8Lit("M"));
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("AabcdeMfghijZ"));

  GapBufferDelete(&f.gb, RangeU64{0, 1});
  GapBufferDelete(&f.gb, RangeU64{11, 12});
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("abcdeMfghij"));
  CHECK_STR(ReadByBytes(f.arena, &f.gb), Str8Lit("abcdeMfghij"));

  Destroy(&f);
}

TEST(gap_buffer_grows_past_capacity) {
  // Start small so the very first insert must grow the block.
  Fixture f = MakeFixture(String8{nullptr, 0}, kGapBufferMinCapacity);
  u64 initial_capacity = f.gb.capacity;

  // Insert well past the initial capacity, always at the front, so the tail is
  // repeatedly slid to the new far end as the block grows.
  u64 count = initial_capacity * 3;
  for (u64 i = 0; i < count; i += 1) {
    u8 c = (u8)('a' + (i % 26));
    GapBufferInsert(&f.gb, 0, String8{&c, 1});
  }

  CHECK_EQ(GapBufferSize(&f.gb), count);
  CHECK(f.gb.capacity > initial_capacity);

  // The i-th insert went to the front, so the buffer reads back reversed.
  String8 all = GapBufferCopyAll(f.arena, &f.gb);
  CHECK_EQ(all.size, count);
  for (u64 i = 0; i < count; i += 1) {
    CHECK_EQ(all.str[i], (u8)('a' + ((count - 1 - i) % 26)));
  }

  Destroy(&f);
}

TEST(gap_buffer_grows_on_large_single_insert) {
  Fixture f = MakeFixture(String8{nullptr, 0}, kGapBufferMinCapacity);

  // One insert far larger than the doubling step.
  Arena *scratch = ArenaAlloc(MB(4));
  u64 n = MB(1);
  u8 *big = PushArrayNoZero(scratch, u8, n);
  memset(big, 'x', n);

  GapBufferInsert(&f.gb, 0, String8{big, n});
  CHECK_EQ(GapBufferSize(&f.gb), n);
  CHECK_EQ(GapBufferByteAt(&f.gb, 0), 'x');
  CHECK_EQ(GapBufferByteAt(&f.gb, n - 1), 'x');

  ArenaRelease(scratch);
  Destroy(&f);
}

TEST(gap_buffer_copy_range_straddles_gap) {
  Fixture f = MakeFixture(Str8Lit("0123456789"));

  // Park the gap in the middle by editing there, then read across it.
  GapBufferInsert(&f.gb, 5, Str8Lit("-"));
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("01234-56789"));

  CHECK_STR(GapBufferCopyRange(f.arena, &f.gb, RangeU64{0, 5}), Str8Lit("01234"));
  CHECK_STR(GapBufferCopyRange(f.arena, &f.gb, RangeU64{6, 11}), Str8Lit("56789"));
  CHECK_STR(GapBufferCopyRange(f.arena, &f.gb, RangeU64{3, 9}), Str8Lit("34-567"));

  // Ranges are clamped rather than asserted.
  CHECK_STR(GapBufferCopyRange(f.arena, &f.gb, RangeU64{8, 999}), Str8Lit("789"));
  CHECK_EQ(GapBufferCopyRange(f.arena, &f.gb, RangeU64{99, 999}).size, 0);
  CHECK_EQ(GapBufferCopyRange(f.arena, &f.gb, RangeU64{5, 5}).size, 0);

  Destroy(&f);
}

TEST(gap_buffer_find_char) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));
  // Force the gap into the middle so searches must cross it.
  GapBufferInsert(&f.gb, 7, String8{nullptr, 0});
  GapBufferDelete(&f.gb, RangeU64{7, 7});

  CHECK_EQ(GapBufferFindChar(&f.gb, '\n', 0), 3);
  CHECK_EQ(GapBufferFindChar(&f.gb, '\n', 4), 7);
  CHECK_EQ(GapBufferFindChar(&f.gb, '\n', 8), GapBufferSize(&f.gb));  // absent
  CHECK_EQ(GapBufferFindChar(&f.gb, 'z', 0), GapBufferSize(&f.gb));

  CHECK_EQ(GapBufferFindCharReverse(&f.gb, '\n', GapBufferSize(&f.gb)), 7);
  CHECK_EQ(GapBufferFindCharReverse(&f.gb, '\n', 7), 3);
  CHECK_EQ(GapBufferFindCharReverse(&f.gb, '\n', 3), GapBufferSize(&f.gb));

  Destroy(&f);
}

TEST(gap_buffer_utf8_is_byte_transparent) {
  // The gap buffer stores bytes; multi-byte characters must survive edits that
  // move the gap around them.
  Fixture f = MakeFixture(Str8Lit("a\xC3\xA9\xE2\x82\xAC z"));
  u64 size = GapBufferSize(&f.gb);

  GapBufferInsert(&f.gb, 1, Str8Lit("\xF0\x9F\x98\x80"));  // 4-byte emoji
  CHECK_EQ(GapBufferSize(&f.gb), size + 4);

  String8 all = GapBufferCopyAll(f.arena, &f.gb);
  CHECK_EQ(Utf8Decode(all, 1).codepoint, 0x1F600);
  CHECK_EQ(Utf8Decode(all, 5).codepoint, 0x00E9);
  CHECK_EQ(Utf8Decode(all, 7).codepoint, 0x20AC);

  Destroy(&f);
}

TEST(gap_buffer_clear) {
  Fixture f = MakeFixture(Str8Lit("hello"));
  u64 capacity = f.gb.capacity;

  GapBufferClear(&f.gb);
  CHECK_EQ(GapBufferSize(&f.gb), 0);
  CHECK_EQ(f.gb.capacity, capacity);  // storage is retained for reuse

  GapBufferInsert(&f.gb, 0, Str8Lit("again"));
  CHECK_STR(GapBufferCopyAll(f.arena, &f.gb), Str8Lit("again"));

  Destroy(&f);
}
