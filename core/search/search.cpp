#include "search/search.h"

#include "os/os_file.h"

#include <stdlib.h>

namespace {

const char *kSkippedDirectories[] = {
    ".git", ".hg", ".svn", "node_modules", "build", "build-release", "target",
    ".cache", "__pycache__", ".venv", "venv", "dist", ".idea", ".vscode",
};

const char *kSkippedExtensions[] = {
    "o", "a", "so", "dylib", "dll", "exe", "obj", "lib", "pdb",
    "png", "jpg", "jpeg", "gif", "bmp", "ico", "pdf", "zip", "gz", "tar",
    "ttf", "ttc", "otf", "woff", "woff2", "mp3", "mp4", "wav", "bin",
};

// Text with an embedded NUL is not text. Checking a prefix is enough to keep a
// stray binary out of grep results without reading the whole file.
[[nodiscard]] bool LooksBinary(String8 data) {
  u64 n = Min(data.size, (u64)1024);
  for (u64 i = 0; i < n; i += 1) {
    if (data.str[i] == 0) return true;
  }
  return false;
}

struct PathNode {
  PathNode *next;
  String8 path;
};

struct WalkState {
  Arena *arena;       // where the returned paths live
  Arena *nodes;       // the growing list, which must outlive every directory
  Arena *scratch;     // one directory's listing, released as the walk unwinds
  PathNode *first;
  PathNode *last;
  u64 count;
  u64 max_files;
  bool truncated;
};

// `relative` is the prefix to record with each file, so callers get paths
// relative to the root rather than absolute ones.
void WalkDirectory(WalkState *state, String8 absolute, String8 relative, u64 depth) {
  if (state->truncated || depth > kSearchMaxDepth) return;

  TempArena temp = TempBegin(state->scratch);
  FileList entries = OsDirList(state->scratch, absolute);

  for (u64 i = 0; i < entries.count; i += 1) {
    if (state->truncated) break;

    FileInfo *entry = &entries.files[i];
    // Hidden files are skipped wholesale; a project's interesting files are
    // not usually dotfiles, and .git alone would dominate the walk.
    if (entry->name.size > 0 && entry->name.str[0] == '.') continue;

    String8 child_relative = (relative.size > 0)
                                 ? OsPathJoin(state->scratch, relative, entry->name)
                                 : entry->name;

    if (entry->is_dir) {
      if (SearchShouldSkipDirectory(entry->name)) continue;
      String8 child_absolute = OsPathJoin(state->scratch, absolute, entry->name);
      WalkDirectory(state, child_absolute, child_relative, depth + 1);
      continue;
    }

    if (SearchShouldSkipFile(entry->name)) continue;

    if (state->count >= state->max_files) {
      state->truncated = true;
      break;
    }

    // The node goes in its own arena: `scratch` is popped when this directory
    // is done with, which would take the list built so far along with it.
    PathNode *node = PushStruct(state->nodes, PathNode);
    node->path = PushStr8Copy(state->arena, child_relative);

    if (state->last) {
      state->last->next = node;
      state->last = node;
    } else {
      state->first = state->last = node;
    }
    state->count += 1;
  }

  TempEnd(temp);
}

int CompareByScore(const void *a, const void *b) {
  const FuzzyResult *fa = (const FuzzyResult *)a;
  const FuzzyResult *fb = (const FuzzyResult *)b;
  if (fa->score != fb->score) return (fa->score > fb->score) ? -1 : 1;
  // Shorter candidates win ties: a query is more likely to mean the file whose
  // name it nearly is than one that merely contains it.
  if (fa->text.size != fb->text.size) return (fa->text.size < fb->text.size) ? -1 : 1;
  return 0;
}

bool PathHasGit(String8 path) {
  TempArena scratch = ScratchBegin();
  String8 marker = OsPathJoin(scratch.arena, path, Str8Lit(".git"));
  bool ok = OsDirExists(marker) || OsFileExists(marker);
  ScratchEnd(scratch);
  return ok;
}

void PushPathNode(WalkState *state, String8 relative) {
  PathNode *node = PushStruct(state->nodes, PathNode);
  node->path = PushStr8Copy(state->arena, relative);

  if (state->last) {
    state->last->next = node;
    state->last = node;
  } else {
    state->first = state->last = node;
  }
  state->count += 1;
}

// Collects directories with a `.git` marker. Unlike the file walk, hidden names
// are still skipped for descent, but `.git` itself is probed explicitly.
void WalkGitRoots(WalkState *state, String8 absolute, String8 relative, u64 depth) {
  if (state->truncated || depth > kSearchMaxDepth) return;

  if (PathHasGit(absolute)) {
    if (state->count >= state->max_files) {
      state->truncated = true;
      return;
    }
    // The walk root itself is a repo: show "." so the picker has a selectable
    // path rather than a blank line.
    String8 recorded = (relative.size > 0) ? relative : Str8Lit(".");
    PushPathNode(state, recorded);
    return;
  }

  TempArena temp = TempBegin(state->scratch);
  FileList entries = OsDirList(state->scratch, absolute);

  for (u64 i = 0; i < entries.count; i += 1) {
    if (state->truncated) break;

    FileInfo *entry = &entries.files[i];
    if (!entry->is_dir) continue;
    if (entry->name.size > 0 && entry->name.str[0] == '.') continue;
    if (SearchShouldSkipDirectory(entry->name)) continue;

    String8 child_relative = (relative.size > 0)
                                 ? OsPathJoin(state->scratch, relative, entry->name)
                                 : entry->name;
    String8 child_absolute = OsPathJoin(state->scratch, absolute, entry->name);
    WalkGitRoots(state, child_absolute, child_relative, depth + 1);
  }

  TempEnd(temp);
}

}  // namespace

bool SearchShouldSkipDirectory(String8 name) {
  for (u64 i = 0; i < ArrayCount(kSkippedDirectories); i += 1) {
    if (Str8Match(name, Str8C(kSkippedDirectories[i]))) return true;
  }
  return false;
}

bool SearchShouldSkipFile(String8 name) {
  String8 extension = Str8PathExt(name);
  if (extension.size == 0) return false;

  for (u64 i = 0; i < ArrayCount(kSkippedExtensions); i += 1) {
    if (Str8Match(extension, Str8C(kSkippedExtensions[i]), StringMatch::CaseInsensitive)) {
      return true;
    }
  }
  return false;
}

PathList SearchWalkFiles(Arena *arena, String8 root, u64 max_files) {
  PathList list = {};

  // Two scratches: one for the list being accumulated, one for the directory
  // currently being read. They cannot be the same arena, since the second is
  // released every time the walk unwinds a level.
  TempArena nodes = ScratchBegin1(arena);
  Arena *conflicts[] = {arena, nodes.arena};
  TempArena dirs = ScratchBegin(conflicts, ArrayCount(conflicts));

  WalkState state = {};
  state.arena = arena;
  state.nodes = nodes.arena;
  state.scratch = dirs.arena;
  state.max_files = max_files;

  WalkDirectory(&state, root, String8{nullptr, 0}, 0);

  list.paths = PushArrayNoZero(arena, String8, Max(state.count, (u64)1));
  list.count = state.count;
  list.truncated = state.truncated;

  u64 i = 0;
  for (PathNode *node = state.first; node; node = node->next) list.paths[i++] = node->path;

  ScratchEnd(dirs);
  ScratchEnd(nodes);
  return list;
}

PathList SearchWalkGitRoots(Arena *arena, String8 root, u64 max_roots) {
  PathList list = {};

  TempArena nodes = ScratchBegin1(arena);
  Arena *conflicts[] = {arena, nodes.arena};
  TempArena dirs = ScratchBegin(conflicts, ArrayCount(conflicts));

  WalkState state = {};
  state.arena = arena;
  state.nodes = nodes.arena;
  state.scratch = dirs.arena;
  state.max_files = max_roots;

  WalkGitRoots(&state, root, String8{nullptr, 0}, 0);

  list.paths = PushArrayNoZero(arena, String8, Max(state.count, (u64)1));
  list.count = state.count;
  list.truncated = state.truncated;

  u64 i = 0;
  for (PathNode *node = state.first; node; node = node->next) list.paths[i++] = node->path;

  ScratchEnd(dirs);
  ScratchEnd(nodes);
  return list;
}

SearchCorpus SearchLoadCorpus(Arena *arena, String8 root, u64 max_bytes) {
  SearchCorpus corpus = {};

  TempArena scratch = ScratchBegin1(arena);
  PathList files = SearchWalkFiles(scratch.arena, root);

  corpus.paths = PushArrayNoZero(arena, String8, Max(files.count, (u64)1));
  corpus.contents = PushArrayNoZero(arena, String8, Max(files.count, (u64)1));

  for (u64 i = 0; i < files.count; i += 1) {
    if (corpus.total_bytes >= max_bytes) {
      corpus.truncated = true;
      break;
    }

    // Both the output arena and the walk's scratch are declared as conflicts.
    // Naming only one lets the per-file scratch land on the other -- and since
    // it is popped after every file, that would discard the corpus entries
    // collected so far, silently losing files.
    Arena *conflicts[] = {arena, scratch.arena};
    TempArena file_scratch = ScratchBegin(conflicts, ArrayCount(conflicts));

    String8 absolute = OsPathJoin(file_scratch.arena, root, files.paths[i]);
    FileContents contents = OsFileRead(file_scratch.arena, absolute);

    bool usable = contents.ok && contents.data.size <= kSearchMaxFileSize &&
                  !LooksBinary(contents.data);
    if (usable) {
      corpus.paths[corpus.count] = PushStr8Copy(arena, files.paths[i]);
      corpus.contents[corpus.count] = PushStr8Copy(arena, contents.data);
      corpus.total_bytes += contents.data.size;
      corpus.count += 1;
    }

    ScratchEnd(file_scratch);
  }

  ScratchEnd(scratch);
  return corpus;
}

GrepResults SearchGrepCorpus(Arena *arena, const SearchCorpus *corpus, String8 pattern,
                             u64 max_matches) {
  GrepResults results = {};
  if (pattern.size == 0) return results;

  // Smartcase: a lowercase pattern matches either case, an uppercase one is
  // taken literally.
  bool has_upper = false;
  for (u64 i = 0; i < pattern.size; i += 1) {
    if (CharIsUpper(pattern.str[i])) has_upper = true;
  }
  StringMatch match_flags = has_upper ? StringMatch::None : StringMatch::CaseInsensitive;

  // Counted first so the results can go straight into one array, which keeps a
  // live search from building a linked list on every keystroke.
  u64 count = 0;
  for (u64 f = 0; f < corpus->count && count < max_matches; f += 1) {
    String8 data = corpus->contents[f];
    u64 line_start = 0;
    for (u64 p = 0; p <= data.size && count < max_matches; p += 1) {
      if (p != data.size && data.str[p] != '\n') continue;
      String8 line = String8{data.str + line_start, p - line_start};
      if (Str8FindFirst(line, pattern, 0, match_flags) < line.size) count += 1;
      line_start = p + 1;
    }
  }

  results.matches = PushArrayNoZero(arena, GrepMatch, Max(count, (u64)1));
  results.files_searched = corpus->count;

  for (u64 f = 0; f < corpus->count && results.count < count; f += 1) {
    String8 data = corpus->contents[f];
    u64 line_number = 1;
    u64 line_start = 0;

    for (u64 p = 0; p <= data.size && results.count < count; p += 1) {
      if (p != data.size && data.str[p] != '\n') continue;

      String8 line = String8{data.str + line_start, p - line_start};
      u64 hit = Str8FindFirst(line, pattern, 0, match_flags);

      if (hit < line.size) {
        GrepMatch *match = &results.matches[results.count];
        // Copied rather than referenced: a one-shot search frees the corpus it
        // loaded, and the results have to outlive it.
        match->path = PushStr8Copy(arena, corpus->paths[f]);
        match->line = line_number;
        match->column = hit + 1;
        match->text = PushStr8Copy(arena, Str8SkipChopWhitespace(line));
        results.count += 1;
      }

      line_start = p + 1;
      line_number += 1;
    }
  }

  results.truncated = (count >= max_matches);
  return results;
}

GrepResults SearchGrep(Arena *arena, String8 root, String8 pattern, u64 max_matches) {
  // A one-shot search loads the corpus into scratch and throws it away again,
  // so it costs the same memory as the tree it searched rather than keeping it.
  TempArena scratch = ScratchBegin1(arena);
  SearchCorpus corpus = SearchLoadCorpus(scratch.arena, root);
  GrepResults results = SearchGrepCorpus(arena, &corpus, pattern, max_matches);
  ScratchEnd(scratch);
  return results;
}

i32 FuzzyScore(String8 candidate, String8 query) {
  if (query.size == 0) return 0;
  if (query.size > candidate.size) return kFuzzyNoMatch;

  i32 score = 0;
  u64 candidate_index = 0;
  i32 consecutive = 0;

  for (u64 q = 0; q < query.size; q += 1) {
    u8 wanted = CharToLower(query.str[q]);

    bool found = false;
    while (candidate_index < candidate.size) {
      u8 c = candidate.str[candidate_index];

      if (CharToLower(c) == wanted) {
        // A match at the start, after a separator, or at a camelCase hump is
        // far more likely to be what was meant than one mid-word.
        bool at_start = (candidate_index == 0);
        u8 previous = at_start ? 0 : candidate.str[candidate_index - 1];
        bool after_separator = (previous == '/' || previous == '_' || previous == '-' ||
                                previous == '.' || previous == ' ');
        bool camel_hump = CharIsUpper(c) && CharIsLower(previous);

        if (at_start || after_separator) score += 10;
        if (camel_hump) score += 8;
        if (c == query.str[q]) score += 2;  // exact case
        // Weighted above the boundary bonus on purpose: "comm" should find
        // "command.cpp" ahead of "c_o_m_m_a_n_d.cpp", where every character
        // happens to sit on a separator.
        score += consecutive * 8;

        consecutive += 1;
        candidate_index += 1;
        found = true;
        break;
      }

      consecutive = 0;
      score -= 1;  // every skipped character makes the match a little worse
      candidate_index += 1;
    }

    if (!found) return kFuzzyNoMatch;
  }

  // Prefer matches concentrated near the end of a path, which is where the
  // file name lives.
  u64 basename_start = Str8FindLastChar(candidate, '/');
  if (basename_start != candidate.size && candidate_index > basename_start) score += 20;

  return score;
}

FuzzyResults FuzzyFilter(Arena *arena, const String8 *candidates, u64 count, String8 query,
                         u64 max_results) {
  FuzzyResults results = {};
  results.items = PushArrayNoZero(arena, FuzzyResult, Max(Min(count, max_results), (u64)1));

  if (query.size == 0) {
    // No query means no ranking to do; keep the original order.
    u64 n = Min(count, max_results);
    for (u64 i = 0; i < n; i += 1) results.items[i] = FuzzyResult{candidates[i], 0};
    results.count = n;
    return results;
  }

  TempArena scratch = ScratchBegin1(arena);
  FuzzyResult *scored = PushArrayNoZero(scratch.arena, FuzzyResult, Max(count, (u64)1));
  u64 scored_count = 0;

  for (u64 i = 0; i < count; i += 1) {
    i32 score = FuzzyScore(candidates[i], query);
    if (score == kFuzzyNoMatch) continue;
    scored[scored_count++] = FuzzyResult{candidates[i], score};
  }

  qsort(scored, scored_count, sizeof(FuzzyResult), CompareByScore);

  u64 n = Min(scored_count, max_results);
  for (u64 i = 0; i < n; i += 1) results.items[i] = scored[i];
  results.count = n;

  ScratchEnd(scratch);
  return results;
}
