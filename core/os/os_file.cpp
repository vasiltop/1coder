#include "os/os_file.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <limits.h>
#  include <sys/mman.h>
#  include <stdio.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

int CompareEntries(const void *a, const void *b) {
  const FileInfo *fa = (const FileInfo *)a;
  const FileInfo *fb = (const FileInfo *)b;

  // Directories first, so an explorer listing groups them at the top.
  if (fa->is_dir != fb->is_dir) return fa->is_dir ? -1 : 1;

  u64 n = Min(fa->name.size, fb->name.size);
  for (u64 i = 0; i < n; i += 1) {
    u8 ca = CharToLower(fa->name.str[i]);
    u8 cb = CharToLower(fb->name.str[i]);
    if (ca != cb) return (ca < cb) ? -1 : 1;
  }
  if (fa->name.size != fb->name.size) return (fa->name.size < fb->name.size) ? -1 : 1;
  return 0;
}

bool IsPathSep(u8 c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// Platform-independent. These compose the primitives below rather than calling
// the OS themselves, so there is one copy rather than one per platform.
// ---------------------------------------------------------------------------

String8 OsPathJoin(Arena *arena, String8 a, String8 b) {
  if (a.size == 0) return PushStr8Copy(arena, b);
  if (b.size == 0) return PushStr8Copy(arena, a);

  bool a_ends = IsPathSep(a.str[a.size - 1]);
  bool b_starts = IsPathSep(b.str[0]);

  if (a_ends && b_starts) return PushStr8Cat(arena, a, Str8Skip(b, 1));
  if (a_ends || b_starts) return PushStr8Cat(arena, a, b);

  // Forward slash even on Windows, where the Win32 APIs accept it and the rest
  // of the editor displays paths in one style.
  TempArena scratch = ScratchBegin1(arena);
  String8 joined = PushStr8Cat(scratch.arena, a, Str8Lit("/"));
  String8 result = PushStr8Cat(arena, joined, b);
  ScratchEnd(scratch);
  return result;
}

bool OsMakeDirs(String8 path) {
  if (path.size == 0) return false;

  TempArena scratch = ScratchBegin();
  String8 partial = PushStr8Copy(scratch.arena, path);

  // Walk forwards, terminating the string at each separator in turn. Mutating
  // the copy in place avoids one allocation per component.
  bool ok = true;
  for (u64 i = 1; i <= partial.size && ok; i += 1) {
    bool at_end = (i == partial.size);
    if (!at_end && !IsPathSep(partial.str[i])) continue;

    // "C:" is a drive, not a directory that can be created.
    if (i == 2 && partial.str[1] == ':') continue;

    u8 saved = at_end ? 0 : partial.str[i];
    partial.str[i] = 0;
    ok = OsMakeDir(String8{partial.str, i});
    if (!at_end) partial.str[i] = saved;
  }

  ScratchEnd(scratch);
  return ok;
}

namespace {

// Creates the parent of `path` so that a create or a move into a directory that
// does not exist yet works the way typing the line implies.
bool MakeParentDirs(String8 path) {
  String8 parent = Str8PathDir(path);
  if (parent.size == 0 || Str8Match(parent, path)) return true;
  return OsMakeDirs(parent);
}

}  // namespace

bool OsDirDeleteRecursive(String8 path) {
  TempArena scratch = ScratchBegin();
  FileList entries = OsDirList(scratch.arena, path);

  bool ok = true;
  for (u64 i = 0; i < entries.count; i += 1) {
    FileInfo *info = &entries.files[i];
    String8 child = OsPathJoin(scratch.arena, path, info->name);

    // A symlink is unlinked whatever it points at. Descending would let this
    // delete files outside the tree being removed.
    if (info->is_dir && !info->is_link) {
      if (!OsDirDeleteRecursive(child)) ok = false;
    } else {
      if (!OsFileDelete(child)) ok = false;
    }
  }

  ScratchEnd(scratch);

  if (!ok) return false;
  return OsDirDelete(path);
}

#if !defined(_WIN32)

// ---------------------------------------------------------------------------
// POSIX
// ---------------------------------------------------------------------------

FileContents OsFileRead(Arena *arena, String8 path) {
  TempArena scratch = ScratchBegin1(arena);
  const char *cpath = PushCStr(scratch.arena, path);

  FILE *file = fopen(cpath, "rb");
  ScratchEnd(scratch);
  if (!file) return FileContents{String8{nullptr, 0}, false};

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return FileContents{String8{nullptr, 0}, false};
  }
  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return FileContents{String8{nullptr, 0}, false};
  }
  rewind(file);

  u8 *data = PushArrayNoZero(arena, u8, (u64)size + 1);
  size_t read = fread(data, 1, (size_t)size, file);
  fclose(file);

  if (read != (size_t)size) return FileContents{String8{nullptr, 0}, false};

  data[size] = 0;
  return FileContents{String8{data, (u64)size}, true};
}

bool OsFileWrite(String8 path, String8 data) {
  TempArena scratch = ScratchBegin();

  // Write to a temporary beside the target, then rename: a crash or a full disk
  // leaves the original file intact rather than half-written.
  String8 temp_path = PushStr8F(scratch.arena, "%.*s.tmp%d", (int)path.size, (char *)path.str,
                                (int)getpid());
  const char *ctemp = (const char *)temp_path.str;
  const char *cpath = PushCStr(scratch.arena, path);

  FILE *file = fopen(ctemp, "wb");
  if (!file) {
    ScratchEnd(scratch);
    return false;
  }

  bool ok = true;
  if (data.size) ok = (fwrite(data.str, 1, data.size, file) == data.size);
  if (fclose(file) != 0) ok = false;

  if (ok) {
    ok = (rename(ctemp, cpath) == 0);
  }
  if (!ok) remove(ctemp);

  ScratchEnd(scratch);
  return ok;
}

FileMapping OsFileMap(String8 path) {
  FileMapping mapping = {};

  TempArena scratch = ScratchBegin();
  const char *cpath = PushCStr(scratch.arena, path);
  int fd = open(cpath, O_RDONLY);
  ScratchEnd(scratch);

  if (fd < 0) return mapping;

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return mapping;
  }

  void *data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  // The mapping survives the descriptor, so there is no reason to hold it.
  close(fd);

  if (data == MAP_FAILED) return mapping;

  mapping.data = (u8 *)data;
  mapping.size = (u64)st.st_size;
  mapping.ok = true;
  return mapping;
}

void OsFileUnmap(FileMapping *mapping) {
  if (!mapping->ok) return;
  munmap(mapping->data, (size_t)mapping->size);
  *mapping = FileMapping{};
}

bool OsFileExists(String8 path) {
  TempArena scratch = ScratchBegin();
  const char *cpath = PushCStr(scratch.arena, path);
  struct stat st;
  bool ok = (stat(cpath, &st) == 0) && S_ISREG(st.st_mode);
  ScratchEnd(scratch);
  return ok;
}

bool OsDirExists(String8 path) {
  TempArena scratch = ScratchBegin();
  const char *cpath = PushCStr(scratch.arena, path);
  struct stat st;
  bool ok = (stat(cpath, &st) == 0) && S_ISDIR(st.st_mode);
  ScratchEnd(scratch);
  return ok;
}

FileList OsDirList(Arena *arena, String8 path) {
  FileList list = {};

  TempArena scratch = ScratchBegin1(arena);
  const char *cpath = PushCStr(scratch.arena, path);

  DIR *dir = opendir(cpath);
  if (!dir) {
    ScratchEnd(scratch);
    return list;
  }

  // Gather into a list first, since the entry count is not known up front.
  struct Node {
    Node *next;
    FileInfo info;
  };
  Node *first = nullptr, *last = nullptr;
  u64 count = 0;

  for (struct dirent *entry = readdir(dir); entry; entry = readdir(dir)) {
    String8 name = Str8C(entry->d_name);
    if (Str8Match(name, Str8Lit(".")) || Str8Match(name, Str8Lit(".."))) continue;

    Node *node = PushStruct(scratch.arena, Node);
    node->info.name = PushStr8Copy(arena, name);

    // d_type is unavailable on some filesystems, so fall back to stat.
    bool is_dir = false;
    u64 size = 0;
#  if defined(DT_DIR)
    if (entry->d_type != DT_UNKNOWN) {
      is_dir = (entry->d_type == DT_DIR);
    } else
#  endif
    {
      String8 full = OsPathJoin(scratch.arena, path, name);
      struct stat st;
      if (stat((const char *)full.str, &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
        size = (u64)st.st_size;
      }
    }
    // Only links pay for a stat: is_dir must describe the target (so a link to
    // a directory lists and opens like one) while is_link records that the
    // entry itself is a link.
    bool is_link = false;
#  if defined(DT_LNK)
    if (entry->d_type == DT_LNK) {
      is_link = true;
      String8 full = OsPathJoin(scratch.arena, path, name);
      struct stat st;
      if (stat((const char *)full.str, &st) == 0) {
        is_dir = S_ISDIR(st.st_mode);
        size = (u64)st.st_size;
      }
    }
#  endif

    node->info.is_dir = is_dir;
    node->info.is_link = is_link;
    node->info.size = size;

    if (last) {
      last->next = node;
      last = node;
    } else {
      first = last = node;
    }
    count += 1;
  }
  closedir(dir);

  list.files = PushArrayNoZero(arena, FileInfo, Max(count, (u64)1));
  list.count = count;
  u64 i = 0;
  for (Node *n = first; n; n = n->next) list.files[i++] = n->info;

  ScratchEnd(scratch);

  qsort(list.files, list.count, sizeof(FileInfo), CompareEntries);
  return list;
}

bool OsMakeDir(String8 path) {
  TempArena scratch = ScratchBegin();
  const char *cpath = PushCStr(scratch.arena, path);
  bool ok = (mkdir(cpath, 0777) == 0) || (errno == EEXIST && OsDirExists(path));
  ScratchEnd(scratch);
  return ok;
}

bool OsFileCreate(String8 path) {
  if (!MakeParentDirs(path)) return false;

  TempArena scratch = ScratchBegin();
  const char *cpath = PushCStr(scratch.arena, path);
  // O_EXCL is what makes this refuse to truncate something already there.
  int fd = open(cpath, O_WRONLY | O_CREAT | O_EXCL, 0666);
  ScratchEnd(scratch);

  if (fd < 0) return false;
  close(fd);
  return true;
}

bool OsFileDelete(String8 path) {
  TempArena scratch = ScratchBegin();
  const char *cpath = PushCStr(scratch.arena, path);
  bool ok = (unlink(cpath) == 0);
  ScratchEnd(scratch);
  return ok;
}

bool OsDirDelete(String8 path) {
  TempArena scratch = ScratchBegin();
  const char *cpath = PushCStr(scratch.arena, path);
  bool ok = (rmdir(cpath) == 0);
  ScratchEnd(scratch);
  return ok;
}

namespace {

// rename(2) cannot cross filesystems, which a temp directory and a project
// directory frequently are.
bool CopyThenDelete(String8 from, String8 to) {
  TempArena scratch = ScratchBegin();
  FileContents contents = OsFileRead(scratch.arena, from);
  bool ok = contents.ok && OsFileWrite(to, contents.data) && OsFileDelete(from);
  ScratchEnd(scratch);
  return ok;
}

}  // namespace

bool OsRename(String8 from, String8 to) {
  if (!MakeParentDirs(to)) return false;

  TempArena scratch = ScratchBegin();
  const char *cfrom = PushCStr(scratch.arena, from);
  const char *cto = PushCStr(scratch.arena, to);

  bool ok = (rename(cfrom, cto) == 0);
  bool cross_device = (!ok && errno == EXDEV);
  ScratchEnd(scratch);

  // Only regular files get the fallback; copying a directory tree across
  // devices is a different job and the explorer reports the failure instead.
  if (cross_device && OsFileExists(from)) ok = CopyThenDelete(from, to);
  return ok;
}

String8 OsGetCwd(Arena *arena) {
  char buffer[PATH_MAX];
  if (!getcwd(buffer, sizeof(buffer))) return String8{nullptr, 0};
  return PushStr8Copy(arena, Str8C(buffer));
}

bool OsSetCwd(String8 path) {
  TempArena scratch = ScratchBegin();
  bool ok = chdir(PushCStr(scratch.arena, path)) == 0;
  ScratchEnd(scratch);
  return ok;
}

String8 OsPathAbsolute(Arena *arena, String8 path) {
  TempArena scratch = ScratchBegin1(arena);
  const char *cpath = PushCStr(scratch.arena, path);

  char buffer[PATH_MAX];
  char *resolved = realpath(cpath, buffer);
  ScratchEnd(scratch);

  // realpath fails for paths that do not exist yet, which is legitimate for a
  // file about to be created, so fall back to the input.
  if (!resolved) return PushStr8Copy(arena, path);
  return PushStr8Copy(arena, Str8C(resolved));
}

#else

// ---------------------------------------------------------------------------
// Windows
//
// Everything goes through the wide APIs. The "A" variants would encode paths in
// the active code page, which mangles any name outside it -- String8 is UTF-8
// and stays that way, converted at the call boundary and nowhere else.
// ---------------------------------------------------------------------------

namespace {

// A single ReadFile/WriteFile takes a DWORD, so larger files go round in
// chunks. 256MB keeps the loop count trivial without overflowing.
constexpr u64 kIoChunk = 0x10000000;

wchar_t *PushWide(Arena *arena, String8 s) {
  int need = 0;
  if (s.size) {
    need = MultiByteToWideChar(CP_UTF8, 0, (const char *)s.str, (int)s.size, nullptr, 0);
    if (need < 0) need = 0;
  }
  wchar_t *out = PushArrayNoZero(arena, wchar_t, (u64)need + 1);
  if (need > 0) {
    MultiByteToWideChar(CP_UTF8, 0, (const char *)s.str, (int)s.size, out, need);
  }
  out[need] = 0;
  return out;
}

String8 PushUtf8(Arena *arena, const wchar_t *w) {
  // `need` counts the terminator, since -1 asks for the whole NUL-terminated
  // string to be measured.
  int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1) return String8{nullptr, 0};

  u8 *out = PushArrayNoZero(arena, u8, (u64)need);
  WideCharToMultiByte(CP_UTF8, 0, w, -1, (char *)out, need, nullptr, nullptr);
  return String8{out, (u64)need - 1};
}

// Win32 hands back backslashes. Everything above the os layer -- the explorer,
// the search results, the status line -- then has to cope with two separators
// meaning the same thing, so they are rewritten once here instead. Win32
// accepts forward slashes on the way back in, so nothing is lost.
//
// Rewrites in place, which is safe because every caller has just pushed the
// string onto an arena of its own.
String8 NormalizeSeparators(String8 path) {
  for (u64 i = 0; i < path.size; i += 1) {
    if (path.str[i] == '\\') path.str[i] = '/';
  }
  return path;
}

DWORD PathAttributes(String8 path) {
  TempArena scratch = ScratchBegin();
  DWORD attrs = GetFileAttributesW(PushWide(scratch.arena, path));
  ScratchEnd(scratch);
  return attrs;
}

}  // namespace

FileContents OsFileRead(Arena *arena, String8 path) {
  TempArena scratch = ScratchBegin1(arena);
  HANDLE file =
      CreateFileW(PushWide(scratch.arena, path), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ScratchEnd(scratch);
  if (file == INVALID_HANDLE_VALUE) return FileContents{String8{nullptr, 0}, false};

  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    CloseHandle(file);
    return FileContents{String8{nullptr, 0}, false};
  }

  u8 *data = PushArrayNoZero(arena, u8, (u64)size.QuadPart + 1);
  u64 total = 0;
  bool ok = true;
  while (total < (u64)size.QuadPart) {
    DWORD chunk = (DWORD)Min((u64)size.QuadPart - total, kIoChunk);
    DWORD got = 0;
    if (!ReadFile(file, data + total, chunk, &got, nullptr) || got == 0) {
      ok = false;
      break;
    }
    total += got;
  }
  CloseHandle(file);

  if (!ok) return FileContents{String8{nullptr, 0}, false};

  data[total] = 0;
  return FileContents{String8{data, total}, true};
}

bool OsFileWrite(String8 path, String8 data) {
  TempArena scratch = ScratchBegin();

  // Write to a temporary beside the target, then rename over it: a crash or a
  // full disk leaves the original file intact rather than half-written.
  String8 temp_path = PushStr8F(scratch.arena, "%.*s.tmp%u", (int)path.size, (char *)path.str,
                                (unsigned)GetCurrentProcessId());
  wchar_t *wtemp = PushWide(scratch.arena, temp_path);

  HANDLE file = CreateFileW(wtemp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    ScratchEnd(scratch);
    return false;
  }

  bool ok = true;
  u64 total = 0;
  while (total < data.size) {
    DWORD chunk = (DWORD)Min(data.size - total, kIoChunk);
    DWORD put = 0;
    if (!WriteFile(file, data.str + total, chunk, &put, nullptr) || put == 0) {
      ok = false;
      break;
    }
    total += put;
  }
  if (!CloseHandle(file)) ok = false;

  if (ok) {
    // COPY_ALLOWED is what lets this cross volumes, so there is no separate
    // copy-and-unlink fallback the way POSIX needs for EXDEV.
    ok = MoveFileExW(wtemp, PushWide(scratch.arena, path),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
  }
  if (!ok) DeleteFileW(wtemp);

  ScratchEnd(scratch);
  return ok;
}

FileMapping OsFileMap(String8 path) {
  FileMapping mapping = {};

  TempArena scratch = ScratchBegin();
  HANDLE file = CreateFileW(PushWide(scratch.arena, path), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ScratchEnd(scratch);
  if (file == INVALID_HANDLE_VALUE) return mapping;

  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
    CloseHandle(file);
    return mapping;
  }

  HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  CloseHandle(file);
  if (!section) return mapping;

  void *data = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
  // The view survives the section handle, so there is no reason to hold it.
  CloseHandle(section);
  if (!data) return mapping;

  mapping.data = (u8 *)data;
  mapping.size = (u64)size.QuadPart;
  mapping.ok = true;
  return mapping;
}

void OsFileUnmap(FileMapping *mapping) {
  if (!mapping->ok) return;
  UnmapViewOfFile(mapping->data);
  *mapping = FileMapping{};
}

bool OsFileExists(String8 path) {
  DWORD attrs = PathAttributes(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool OsDirExists(String8 path) {
  DWORD attrs = PathAttributes(path);
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

FileList OsDirList(Arena *arena, String8 path) {
  FileList list = {};

  TempArena scratch = ScratchBegin1(arena);
  String8 pattern = OsPathJoin(scratch.arena, path, Str8Lit("*"));

  WIN32_FIND_DATAW find;
  HANDLE handle = FindFirstFileW(PushWide(scratch.arena, pattern), &find);
  if (handle == INVALID_HANDLE_VALUE) {
    ScratchEnd(scratch);
    return list;
  }

  // Gather into a list first, since the entry count is not known up front.
  struct Node {
    Node *next;
    FileInfo info;
  };
  Node *first = nullptr, *last = nullptr;
  u64 count = 0;

  do {
    String8 name = PushUtf8(scratch.arena, find.cFileName);
    if (Str8Match(name, Str8Lit(".")) || Str8Match(name, Str8Lit(".."))) continue;

    Node *node = PushStruct(scratch.arena, Node);
    node->info.name = PushStr8Copy(arena, name);

    // FindFirstFile reports the link itself, and a directory symlink or
    // junction keeps the directory attribute, so is_dir already describes the
    // target the way the POSIX side arranges with an extra stat.
    node->info.is_dir = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    node->info.is_link = (find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    node->info.size = ((u64)find.nFileSizeHigh << 32) | (u64)find.nFileSizeLow;

    if (last) {
      last->next = node;
      last = node;
    } else {
      first = last = node;
    }
    count += 1;
  } while (FindNextFileW(handle, &find));
  FindClose(handle);

  list.files = PushArrayNoZero(arena, FileInfo, Max(count, (u64)1));
  list.count = count;
  u64 i = 0;
  for (Node *n = first; n; n = n->next) list.files[i++] = n->info;

  ScratchEnd(scratch);

  qsort(list.files, list.count, sizeof(FileInfo), CompareEntries);
  return list;
}

bool OsMakeDir(String8 path) {
  TempArena scratch = ScratchBegin();
  bool ok = CreateDirectoryW(PushWide(scratch.arena, path), nullptr) != 0;
  bool exists = (!ok && GetLastError() == ERROR_ALREADY_EXISTS);
  ScratchEnd(scratch);

  return ok || (exists && OsDirExists(path));
}

bool OsFileCreate(String8 path) {
  if (!MakeParentDirs(path)) return false;

  TempArena scratch = ScratchBegin();
  // CREATE_NEW is what makes this refuse to truncate something already there.
  HANDLE file = CreateFileW(PushWide(scratch.arena, path), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  ScratchEnd(scratch);

  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  return true;
}

bool OsFileDelete(String8 path) {
  TempArena scratch = ScratchBegin();
  wchar_t *wpath = PushWide(scratch.arena, path);

  bool ok = DeleteFileW(wpath) != 0;
  if (!ok) {
    // A directory symlink or junction goes through RemoveDirectory rather than
    // DeleteFile. It removes the link, never what it points at, which is the
    // behaviour OsDirDeleteRecursive depends on to stay inside the subtree.
    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
      ok = RemoveDirectoryW(wpath) != 0;
    }
  }

  ScratchEnd(scratch);
  return ok;
}

bool OsDirDelete(String8 path) {
  TempArena scratch = ScratchBegin();
  bool ok = RemoveDirectoryW(PushWide(scratch.arena, path)) != 0;
  ScratchEnd(scratch);
  return ok;
}

bool OsRename(String8 from, String8 to) {
  if (!MakeParentDirs(to)) return false;

  TempArena scratch = ScratchBegin();
  bool ok = MoveFileExW(PushWide(scratch.arena, from), PushWide(scratch.arena, to),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
  ScratchEnd(scratch);
  return ok;
}

String8 OsGetCwd(Arena *arena) {
  TempArena scratch = ScratchBegin1(arena);

  // With a zero buffer this returns the length required, terminator included.
  DWORD need = GetCurrentDirectoryW(0, nullptr);
  if (need == 0) {
    ScratchEnd(scratch);
    return String8{nullptr, 0};
  }

  wchar_t *buffer = PushArrayNoZero(scratch.arena, wchar_t, (u64)need);
  String8 result = String8{nullptr, 0};
  if (GetCurrentDirectoryW(need, buffer) != 0) result = NormalizeSeparators(PushUtf8(arena, buffer));

  ScratchEnd(scratch);
  return result;
}

bool OsSetCwd(String8 path) {
  TempArena scratch = ScratchBegin();
  bool ok = SetCurrentDirectoryW(PushWide(scratch.arena, path)) != 0;
  ScratchEnd(scratch);
  return ok;
}

String8 OsPathAbsolute(Arena *arena, String8 path) {
  TempArena scratch = ScratchBegin1(arena);
  wchar_t *wpath = PushWide(scratch.arena, path);

  DWORD need = GetFullPathNameW(wpath, 0, nullptr, nullptr);
  String8 result = String8{nullptr, 0};
  if (need != 0) {
    wchar_t *buffer = PushArrayNoZero(scratch.arena, wchar_t, (u64)need);
    if (GetFullPathNameW(wpath, need, buffer, nullptr) != 0) {
      result = NormalizeSeparators(PushUtf8(arena, buffer));
    }
  }

  ScratchEnd(scratch);

  // GetFullPathName is purely lexical and does not require the path to exist,
  // but it still fails on a malformed one, so fall back to the input.
  if (result.size == 0) return PushStr8Copy(arena, path);
  return result;
}

#endif
