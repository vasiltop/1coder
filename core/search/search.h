#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Project-wide search: walking a tree, grepping it, and scoring paths for a
// fuzzy finder.
//
// This lives in core rather than behind the platform layer so it can be tested
// without a window, and so the buffers built on top of it stay ordinary
// buffers.

// Directories and files that are never worth walking into. Keeping this here
// rather than in a config file is the same choice the rest of the editor makes:
// changing it means editing the source.
[[nodiscard]] bool SearchShouldSkipDirectory(String8 name);
[[nodiscard]] bool SearchShouldSkipFile(String8 name);

struct PathList {
  String8 *paths;  // relative to the root the walk started from
  u64 count;
  bool truncated;  // a limit was hit, so the list is incomplete
};

inline constexpr u64 kSearchMaxFiles = 50000;
inline constexpr u64 kSearchMaxDepth = 32;
// Files larger than this are assumed not to be source and are skipped.
inline constexpr u64 kSearchMaxFileSize = MB(4);

[[nodiscard]] PathList SearchWalkFiles(Arena *arena, String8 root,
                                       u64 max_files = kSearchMaxFiles);

// ---------------------------------------------------------------------------
// Grep
// ---------------------------------------------------------------------------

struct GrepMatch {
  String8 path;  // relative to the root
  u64 line;      // 1-based, as displayed
  u64 column;    // 1-based byte column of the match
  String8 text;  // the whole line, with surrounding whitespace trimmed
};

struct GrepResults {
  GrepMatch *matches;
  u64 count;
  u64 files_searched;
  bool truncated;
};

inline constexpr u64 kGrepMaxMatches = 5000;

// Literal substring search -- no regular expressions. Matching is
// case-sensitive only when the pattern contains an uppercase letter, which is
// vim's 'smartcase' and what people expect from a search box.
[[nodiscard]] GrepResults SearchGrep(Arena *arena, String8 root, String8 pattern,
                                     u64 max_matches = kGrepMaxMatches);

// ---------------------------------------------------------------------------
// Fuzzy matching
// ---------------------------------------------------------------------------

// Scores `candidate` against `query`, higher being better. Returns
// kFuzzyNoMatch when the query is not a subsequence of the candidate.
//
// The scoring rewards matches at the start of a word or path segment and runs
// of consecutive characters, so "cmdln" finds "core/editor/command_line.cpp"
// ahead of paths that merely contain those letters scattered about.
inline constexpr i32 kFuzzyNoMatch = -1000000;

[[nodiscard]] i32 FuzzyScore(String8 candidate, String8 query);

struct FuzzyResult {
  String8 text;
  i32 score;
};

struct FuzzyResults {
  FuzzyResult *items;
  u64 count;
};

// Filters and ranks, best first. An empty query keeps everything in its
// original order.
[[nodiscard]] FuzzyResults FuzzyFilter(Arena *arena, const String8 *candidates, u64 count,
                                       String8 query, u64 max_results);
