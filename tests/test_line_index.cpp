#include "test.h"
#include "text/gap_buffer.h"
#include "text/line_index.h"

#include <string.h>

namespace {

// The gap buffer and the line index each own their arena exclusively, so a
// fixture holds one of each.
struct Fixture {
  Arena *text_arena;
  Arena *index_arena;
  Arena *scratch;
  GapBuffer gb;
  LineIndex idx;
};

Fixture MakeFixture(String8 initial) {
  Fixture f = {};
  f.text_arena = ArenaAlloc(MB(64));
  f.index_arena = ArenaAlloc(MB(64));
  f.scratch = ArenaAlloc(MB(64));
  GapBufferInitFrom(&f.gb, f.text_arena, initial);
  LineIndexInit(&f.idx, f.index_arena);
  LineIndexRebuild(&f.idx, &f.gb);
  return f;
}

void Destroy(Fixture *f) {
  ArenaRelease(f->text_arena);
  ArenaRelease(f->index_arena);
  ArenaRelease(f->scratch);
}

// Applies an edit through the incremental path, then checks the resulting index
// against a full rescan. This is the property that matters: LineIndexEdit must
// be indistinguishable from LineIndexRebuild.
bool ApplyEditAndVerify(Fixture *f, RangeU64 range, String8 insert, const char *what) {
  u64 size = GapBufferSize(&f->gb);
  RangeU64 clamped = RangeU64{Min(range.min, size), Min(range.max, size)};
  if (clamped.max < clamped.min) clamped.max = clamped.min;

  if (!RangeEmpty(clamped)) GapBufferDelete(&f->gb, clamped);
  if (insert.size) GapBufferInsert(&f->gb, clamped.min, insert);

  LineIndexEdit(&f->idx, &f->gb, clamped, insert.size);

  // Reference index, rebuilt from scratch over the same text.
  Arena *ref_arena = ArenaAlloc(MB(64));
  LineIndex ref = {};
  LineIndexInit(&ref, ref_arena);
  LineIndexRebuild(&ref, &f->gb);

  bool ok = (ref.count == f->idx.count);
  if (!ok) {
    TestFail(__FILE__, __LINE__, "%s: line count %llu, expected %llu", what,
             (unsigned long long)f->idx.count, (unsigned long long)ref.count);
  } else {
    for (u64 i = 0; i < ref.count; i += 1) {
      if (ref.line_starts[i] != f->idx.line_starts[i]) {
        TestFail(__FILE__, __LINE__, "%s: line %llu starts at %llu, expected %llu", what,
                 (unsigned long long)i, (unsigned long long)f->idx.line_starts[i],
                 (unsigned long long)ref.line_starts[i]);
        ok = false;
        break;
      }
    }
  }

  ArenaRelease(ref_arena);
  return ok;
}

struct Rng {
  u64 state;
};

u64 NextRandom(Rng *rng) {
  // xorshift64*
  u64 x = rng->state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng->state = x;
  return x * 0x2545F4914F6CDD1DULL;
}

u64 RandomBelow(Rng *rng, u64 n) { return n ? NextRandom(rng) % n : 0; }

}  // namespace

TEST(line_index_empty_buffer_has_one_line) {
  Fixture f = MakeFixture(String8{nullptr, 0});
  CHECK_EQ(LineCount(&f.idx), 1);
  CHECK_EQ(OffsetFromLine(&f.idx, 0), 0);
  CHECK_EQ(LineFromOffset(&f.idx, 0), 0);
  CHECK_EQ(LineLength(&f.idx, &f.gb, 0), 0);
  Destroy(&f);
}

TEST(line_index_counts_lines) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));
  CHECK_EQ(LineCount(&f.idx), 3);
  CHECK_EQ(OffsetFromLine(&f.idx, 0), 0);
  CHECK_EQ(OffsetFromLine(&f.idx, 1), 4);
  CHECK_EQ(OffsetFromLine(&f.idx, 2), 8);
  Destroy(&f);
}

TEST(line_index_trailing_newline_makes_empty_last_line) {
  // Newlines separate lines, so "a\n" is two lines, the second empty.
  Fixture f = MakeFixture(Str8Lit("a\n"));
  CHECK_EQ(LineCount(&f.idx), 2);
  CHECK_EQ(OffsetFromLine(&f.idx, 1), 2);
  CHECK_EQ(LineLength(&f.idx, &f.gb, 1), 0);
  Destroy(&f);
}

TEST(line_index_consecutive_newlines) {
  Fixture f = MakeFixture(Str8Lit("a\n\n\nb"));
  CHECK_EQ(LineCount(&f.idx), 4);
  CHECK_EQ(LineLength(&f.idx, &f.gb, 1), 0);
  CHECK_EQ(LineLength(&f.idx, &f.gb, 2), 0);
  CHECK_EQ(LineLength(&f.idx, &f.gb, 3), 1);
  Destroy(&f);
}

TEST(line_index_offset_conversion) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));

  CHECK_EQ(LineFromOffset(&f.idx, 0), 0);
  CHECK_EQ(LineFromOffset(&f.idx, 3), 0);  // the newline belongs to its line
  CHECK_EQ(LineFromOffset(&f.idx, 4), 1);
  CHECK_EQ(LineFromOffset(&f.idx, 7), 1);
  CHECK_EQ(LineFromOffset(&f.idx, 8), 2);
  CHECK_EQ(LineFromOffset(&f.idx, 12), 2);

  // Both directions clamp rather than assert.
  CHECK_EQ(LineFromOffset(&f.idx, 9999), 2);
  CHECK_EQ(OffsetFromLine(&f.idx, 9999), 8);

  Destroy(&f);
}

TEST(line_index_line_ranges) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));

  RangeU64 r0 = LineRange(&f.idx, &f.gb, 0);
  CHECK_EQ(r0.min, 0);
  CHECK_EQ(r0.max, 3);  // excludes the newline

  RangeU64 r0n = LineRangeWithNewline(&f.idx, &f.gb, 0);
  CHECK_EQ(r0n.max, 4);  // includes it

  // The last line has no newline, so both forms agree.
  RangeU64 r2 = LineRange(&f.idx, &f.gb, 2);
  CHECK_EQ(r2.min, 8);
  CHECK_EQ(r2.max, 13);
  CHECK_EQ(LineRangeWithNewline(&f.idx, &f.gb, 2).max, 13);

  CHECK_EQ(LineEndOffset(&f.idx, &f.gb, 1), 7);
  CHECK_STR(GapBufferCopyRange(f.scratch, &f.gb, LineRange(&f.idx, &f.gb, 1)), Str8Lit("two"));

  Destroy(&f);
}

TEST(line_index_edit_within_one_line) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));

  CHECK(ApplyEditAndVerify(&f, RangeU64{4, 7}, Str8Lit("TWO!"), "replace within line"));
  CHECK_EQ(LineCount(&f.idx), 3);
  CHECK_EQ(OffsetFromLine(&f.idx, 2), 9);  // tail shifted by +1

  Destroy(&f);
}

TEST(line_index_edit_inserting_newlines) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));

  CHECK(ApplyEditAndVerify(&f, RangeU64{5, 5}, Str8Lit("\nX\n"), "insert newlines mid-line"));
  CHECK_EQ(LineCount(&f.idx), 5);

  Destroy(&f);
}

TEST(line_index_edit_deleting_newline_merges_lines) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));

  CHECK(ApplyEditAndVerify(&f, RangeU64{3, 4}, String8{nullptr, 0}, "delete newline"));
  CHECK_EQ(LineCount(&f.idx), 2);
  CHECK_STR(GapBufferCopyRange(f.scratch, &f.gb, LineRange(&f.idx, &f.gb, 0)), Str8Lit("onetwo"));

  Destroy(&f);
}

TEST(line_index_edit_ending_exactly_on_newline) {
  // The boundary case: the inserted text ends with a newline, so the line start
  // immediately after the edit must be regenerated, not shifted.
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));

  CHECK(ApplyEditAndVerify(&f, RangeU64{4, 4}, Str8Lit("inserted\n"), "insert ending in newline"));
  CHECK_EQ(LineCount(&f.idx), 4);
  CHECK_STR(GapBufferCopyRange(f.scratch, &f.gb, LineRange(&f.idx, &f.gb, 1)), Str8Lit("inserted"));
  CHECK_STR(GapBufferCopyRange(f.scratch, &f.gb, LineRange(&f.idx, &f.gb, 2)), Str8Lit("two"));

  Destroy(&f);
}

TEST(line_index_edit_at_buffer_start_and_end) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo"));

  CHECK(ApplyEditAndVerify(&f, RangeU64{0, 0}, Str8Lit("zero\n"), "insert at start"));
  CHECK_EQ(LineCount(&f.idx), 3);
  CHECK_EQ(OffsetFromLine(&f.idx, 1), 5);

  u64 size = GapBufferSize(&f.gb);
  CHECK(ApplyEditAndVerify(&f, RangeU64{size, size}, Str8Lit("\nlast"), "insert at end"));
  CHECK_EQ(LineCount(&f.idx), 4);

  Destroy(&f);
}

TEST(line_index_edit_spanning_many_lines) {
  Fixture f = MakeFixture(Str8Lit("a\nb\nc\nd\ne\nf"));
  CHECK_EQ(LineCount(&f.idx), 6);

  // Delete across several line boundaries at once. [2,9) removes "b\nc\nd\ne",
  // leaving the newline at offset 9 in place.
  CHECK(ApplyEditAndVerify(&f, RangeU64{2, 9}, Str8Lit("X"), "replace across lines"));
  CHECK_EQ(LineCount(&f.idx), 3);
  CHECK_STR(GapBufferCopyAll(f.scratch, &f.gb), Str8Lit("a\nX\nf"));

  Destroy(&f);
}

TEST(line_index_edit_deleting_everything) {
  Fixture f = MakeFixture(Str8Lit("one\ntwo\nthree"));

  CHECK(ApplyEditAndVerify(&f, RangeU64{0, 13}, String8{nullptr, 0}, "delete all"));
  CHECK_EQ(LineCount(&f.idx), 1);
  CHECK_EQ(GapBufferSize(&f.gb), 0);

  Destroy(&f);
}

TEST(line_index_grows_past_capacity) {
  Fixture f = MakeFixture(String8{nullptr, 0});

  // More lines than the initial array holds, forcing in-place growth.
  u64 count = kLineIndexMinCapacity * 4;
  for (u64 i = 0; i < count; i += 1) {
    u64 size = GapBufferSize(&f.gb);
    GapBufferInsert(&f.gb, size, Str8Lit("line\n"));
    LineIndexEdit(&f.idx, &f.gb, RangeU64{size, size}, 5);
  }

  CHECK_EQ(LineCount(&f.idx), count + 1);
  CHECK(f.idx.capacity >= count + 1);
  CHECK_EQ(OffsetFromLine(&f.idx, count), count * 5);

  Destroy(&f);
}

TEST(line_index_random_edits_match_full_rebuild) {
  // The real safety net: thousands of arbitrary edits, each checked against a
  // full rescan, so any incremental-patch bug shows up as a concrete mismatch.
  Fixture f = MakeFixture(Str8Lit("alpha\nbeta\ngamma\ndelta\n"));
  Rng rng = {0x1234ABCDULL};

  const char *inserts[] = {"", "x", "\n", "\n\n", "a\nb", "hello", "\nend\n", "z\n\nz"};

  for (u32 iter = 0; iter < 3000; iter += 1) {
    u64 size = GapBufferSize(&f.gb);

    u64 a = RandomBelow(&rng, size + 1);
    u64 b = RandomBelow(&rng, size + 1);
    RangeU64 range = RangeMake(a, b);

    // Bias toward small edits, which is what real typing looks like.
    if (RandomBelow(&rng, 4) != 0 && RangeSize(range) > 8) {
      range.max = range.min + RandomBelow(&rng, 8);
    }

    String8 insert = Str8C(inserts[RandomBelow(&rng, ArrayCount(inserts))]);

    if (!ApplyEditAndVerify(&f, range, insert, "random edit")) {
      TestFail(__FILE__, __LINE__, "diverged at iteration %u", iter);
      break;
    }

    // Keep the buffer from growing without bound over the run.
    if (GapBufferSize(&f.gb) > 4096) {
      GapBufferDelete(&f.gb, RangeU64{2048, GapBufferSize(&f.gb)});
      LineIndexRebuild(&f.idx, &f.gb);
    }
  }

  Destroy(&f);
}
