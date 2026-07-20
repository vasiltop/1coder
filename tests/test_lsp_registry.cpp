#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "lsp/registry.h"
#include "os/os_file.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

struct ArenaScope {
  Arena *arena;

  explicit ArenaScope(u64 reserve_size = MB(8)) : arena(ArenaAlloc(reserve_size)) {}
  ~ArenaScope() { ArenaRelease(arena); }
};

struct ScopedFixtureDir {
  Arena *arena;
  String8 path;

  ScopedFixtureDir(Arena *arena_, const char *tag) : arena(arena_) {
    static i32 serial = 0;
    serial += 1;
    String8 cwd = OsGetCwd(arena);
    path = PushStr8F(arena, "%.*s/build/test_lsp_registry_%s_%d", (int)cwd.size, (char *)cwd.str,
                     tag, (int)serial);
    (void)OsDirDeleteRecursive(path);
    CHECK(OsMakeDirs(path));
  }

  ~ScopedFixtureDir() { (void)OsDirDeleteRecursive(path); }
};

struct ScopedEnvVar {
  Arena *arena;
  const char *name;
  bool had_old_value;
  String8 old_value;

  ScopedEnvVar(Arena *arena_, const char *name_, String8 value) : arena(arena_), name(name_) {
    const char *old = getenv(name);
    had_old_value = old != nullptr;
    if (had_old_value) old_value = PushStr8Copy(arena, Str8C(old));
    Set(value);
  }

  ~ScopedEnvVar() {
    if (had_old_value) {
      Set(old_value);
    } else {
#if defined(_WIN32)
      CHECK(SetEnvironmentVariableA(name, nullptr) != 0);
#else
      CHECK(unsetenv(name) == 0);
#endif
    }
  }

  void Set(String8 value) {
#if defined(_WIN32)
    CHECK(SetEnvironmentVariableA(name, PushCStr(arena, value)) != 0);
#else
    CHECK(setenv(name, PushCStr(arena, value), 1) == 0);
#endif
  }
};

struct ScopedWorkingDirectory {
  Arena *arena;
  String8 old_value;

  explicit ScopedWorkingDirectory(Arena *arena_) : arena(arena_), old_value(OsGetCwd(arena_)) {}
  ~ScopedWorkingDirectory() { CHECK(Set(old_value)); }

  bool Set(String8 path) {
#if defined(_WIN32)
    return SetCurrentDirectoryA(PushCStr(arena, path)) != 0;
#else
    return chdir(PushCStr(arena, path)) == 0;
#endif
  }
};

String8 UppercaseCopy(Arena *arena, String8 string) {
  u8 *copy = PushArrayNoZero(arena, u8, string.size + 1);
  for (u64 i = 0; i < string.size; i += 1) copy[i] = CharToUpper(string.str[i]);
  copy[string.size] = 0;
  return Str8(copy, string.size);
}

String8 RootOfPath(String8 path) {
#if defined(_WIN32)
  if (path.size >= 3 && path.str[1] == ':' && path.str[2] == '/') return Str8Prefix(path, 3);
#endif
  return Str8Lit("/");
}

String8 ExecutableFileName(Arena *arena, String8 bare_name) {
#if defined(_WIN32)
  return PushStr8Cat(arena, bare_name, Str8Lit(".exe"));
#else
  return PushStr8Copy(arena, bare_name);
#endif
}

String8 WriteExecutableFixture(Arena *arena, String8 dir, String8 bare_name) {
  String8 file_name = ExecutableFileName(arena, bare_name);
  String8 path = OsPathJoin(arena, dir, file_name);
  CHECK(OsFileWrite(path, String8{}));
#if !defined(_WIN32)
  CHECK(chmod(PushCStr(arena, path), 0777) == 0);
#endif
  return path;
}

void CheckLanguage(String8 path, LspLanguage expected_language, String8 expected_id) {
  LspLanguage language = LspLanguage::COUNT;
  String8 language_id = {};
  CHECK(LspLanguageForPath(path, &language, &language_id));
  CHECK_EQ((u64)language, (u64)expected_language);
  CHECK_STR(language_id, expected_id);
}

void CheckRejectsLanguage(String8 path) {
  LspLanguage language = LspLanguage::COUNT;
  String8 language_id = Str8Lit("keep");
  CHECK(!LspLanguageForPath(path, &language, &language_id));
}

void CheckCommand(const LspServerCommand &command, LspLanguage language, String8 language_id,
                  String8 executable, String8 root, String8 *arguments, u64 argument_count) {
  CHECK_EQ((u64)command.language, (u64)language);
  CHECK_STR(command.language_id, language_id);
#if defined(_WIN32)
  CHECK(Str8Match(command.executable, executable, StringMatch::CaseInsensitive));
#else
  CHECK_STR(command.executable, executable);
#endif
  CHECK_STR(command.root, root);
  CHECK_EQ(command.argument_count, argument_count);
  for (u64 i = 0; i < argument_count; i += 1) CHECK_STR(command.arguments[i], arguments[i]);
}

void ReuseScratchStorage(Arena *arena) {
  for (u64 i = 0; i < 16; i += 1) {
    TempArena scratch = ScratchBegin1(arena);
    u8 *bytes = PushArrayNoZero(scratch.arena, u8, 4096);
    memset(bytes, 'x', 4096);
    ScratchEnd(scratch);
  }
}

}  // namespace

TEST(lsp_registry_maps_supported_extensions_case_insensitively) {
  ArenaScope scope;

  struct Case {
    const char *path;
    LspLanguage language;
    String8 language_id;
  };

  const Case cases[] = {
      {"src/main.c", LspLanguage::Cpp, Str8Lit("c")},
      {"src/main.h", LspLanguage::Cpp, Str8Lit("cpp")},
      {"src/main.cc", LspLanguage::Cpp, Str8Lit("cpp")},
      {"src/main.cpp", LspLanguage::Cpp, Str8Lit("cpp")},
      {"src/main.cxx", LspLanguage::Cpp, Str8Lit("cpp")},
      {"src/main.hh", LspLanguage::Cpp, Str8Lit("cpp")},
      {"src/main.hpp", LspLanguage::Cpp, Str8Lit("cpp")},
      {"src/main.hxx", LspLanguage::Cpp, Str8Lit("cpp")},
      {"src/lib.rs", LspLanguage::Rust, Str8Lit("rust")},
      {"src/main.go", LspLanguage::Go, Str8Lit("go")},
      {"pkg/mod.py", LspLanguage::Python, Str8Lit("python")},
      {"pkg/mod.pyi", LspLanguage::Python, Str8Lit("python")},
      {"web/app.js", LspLanguage::TypeScript, Str8Lit("javascript")},
      {"web/app.mjs", LspLanguage::TypeScript, Str8Lit("javascript")},
      {"web/app.cjs", LspLanguage::TypeScript, Str8Lit("javascript")},
      {"web/app.jsx", LspLanguage::TypeScript, Str8Lit("javascriptreact")},
      {"web/app.ts", LspLanguage::TypeScript, Str8Lit("typescript")},
      {"web/app.mts", LspLanguage::TypeScript, Str8Lit("typescript")},
      {"web/app.cts", LspLanguage::TypeScript, Str8Lit("typescript")},
      {"web/app.tsx", LspLanguage::TypeScript, Str8Lit("typescriptreact")},
  };

  for (u64 i = 0; i < ArrayCount(cases); i += 1) {
    String8 path = Str8C(cases[i].path);
    CheckLanguage(path, cases[i].language, cases[i].language_id);
    CheckLanguage(UppercaseCopy(scope.arena, path), cases[i].language, cases[i].language_id);
  }

  CheckRejectsLanguage(Str8Lit("Makefile"));
  CheckRejectsLanguage(Str8Lit("notes.txt"));
  CheckRejectsLanguage(Str8Lit("image.PNG"));
  CheckRejectsLanguage(Str8Lit("plugin.lua"));
}

TEST(lsp_registry_finds_nearest_project_root_and_same_dir_wins_over_farther_priority) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "root_priority");

  String8 project = OsPathJoin(scope.arena, fixture.path, Str8Lit("space \xE2\x98\x83/project"));
  String8 nested = OsPathJoin(scope.arena, project, Str8Lit("sub/module/src"));
  CHECK(OsMakeDirs(nested));

  CHECK(OsFileWrite(OsPathJoin(scope.arena, project, Str8Lit("compile_commands.json")), Str8Lit("{}")));
  String8 closer = OsPathJoin(scope.arena, project, Str8Lit("sub/module"));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, closer, Str8Lit("CMakeLists.txt")),
                    Str8Lit("cmake_minimum_required(VERSION 3.22)\n")));

  String8 file = OsPathJoin(scope.arena, nested, Str8Lit("main.cpp"));
  CHECK_STR(LspFindProjectRoot(scope.arena, LspLanguage::Cpp, file), closer);
}

TEST(lsp_registry_finds_git_markers_falls_back_without_markers_and_stops_at_root) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "root_misc");

  String8 rust_root = OsPathJoin(scope.arena, fixture.path, Str8Lit("rust repo"));
  String8 rust_src = OsPathJoin(scope.arena, rust_root, Str8Lit("src/bin"));
  CHECK(OsMakeDirs(rust_src));
  CHECK(OsMakeDirs(OsPathJoin(scope.arena, rust_root, Str8Lit(".git"))));

  String8 js_root = OsPathJoin(scope.arena, fixture.path, Str8Lit("web"));
  String8 js_src = OsPathJoin(scope.arena, js_root, Str8Lit("packages/app"));
  CHECK(OsMakeDirs(js_src));
  CHECK(OsFileWrite(OsPathJoin(scope.arena, js_root, Str8Lit(".git")), Str8Lit("gitdir: elsewhere\n")));

  CHECK_STR(LspFindProjectRoot(scope.arena, LspLanguage::Rust,
                               OsPathJoin(scope.arena, rust_src, Str8Lit("main.rs"))),
            rust_root);
  CHECK_STR(LspFindProjectRoot(scope.arena, LspLanguage::TypeScript,
                               OsPathJoin(scope.arena, js_src, Str8Lit("index.tsx"))),
            js_root);

  String8 drive_root = RootOfPath(OsGetCwd(scope.arena));
  String8 no_marker = OsPathJoin(scope.arena, drive_root, Str8Lit("1coder-lsp-no-marker/plain/src/tool.py"));
  CHECK_STR(LspFindProjectRoot(scope.arena, LspLanguage::Python, no_marker),
            OsPathJoin(scope.arena, drive_root, Str8Lit("1coder-lsp-no-marker/plain/src")));

  String8 root_file = OsPathJoin(scope.arena, drive_root, Str8Lit("1coder_lsp_registry_root_stop.go"));
  CHECK_STR(LspFindProjectRoot(scope.arena, LspLanguage::Go, root_file), drive_root);
}

TEST(lsp_registry_normalized_root_paths_are_owned_after_scratch_reuse) {
  ArenaScope scope;

  String8 root = RootOfPath(OsGetCwd(scope.arena));
  String8 reducing = OsPathJoin(scope.arena, root, Str8Lit("1coder-lsp-root/.."));
  String8 normalized = LspNormalizePath(scope.arena, reducing);
  ReuseScratchStorage(scope.arena);

  CHECK_STR(normalized, root);
}

#if defined(_WIN32)
TEST(lsp_registry_normalizes_windows_drive_root_paths) {
  ArenaScope scope;

  String8 drive_root = RootOfPath(OsGetCwd(scope.arena));
  String8 reducing = OsPathJoin(scope.arena, drive_root, Str8Lit("1coder-lsp-root/.."));

  CHECK_STR(LspNormalizePath(scope.arena, drive_root), drive_root);
  CHECK_STR(LspNormalizePath(scope.arena, reducing), drive_root);
}
#endif

TEST(lsp_registry_resolves_server_commands_from_path_and_prefers_basedpyright) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "commands");

  String8 bin = OsPathJoin(scope.arena, fixture.path, Str8Lit("bin"));
  String8 cwd_dir = OsPathJoin(scope.arena, fixture.path, Str8Lit("cwd"));
  CHECK(OsMakeDirs(bin));
  CHECK(OsMakeDirs(cwd_dir));

  String8 clangd_path = WriteExecutableFixture(scope.arena, bin, Str8Lit("clangd"));
  String8 rust_path = WriteExecutableFixture(scope.arena, bin, Str8Lit("rust-analyzer"));
  String8 gopls_path = WriteExecutableFixture(scope.arena, bin, Str8Lit("gopls"));
  String8 ts_path = WriteExecutableFixture(scope.arena, bin, Str8Lit("typescript-language-server"));
  String8 basedpyright_path = WriteExecutableFixture(scope.arena, bin, Str8Lit("basedpyright-langserver"));
  String8 pyright_path = WriteExecutableFixture(scope.arena, bin, Str8Lit("pyright-langserver"));
  WriteExecutableFixture(scope.arena, cwd_dir, Str8Lit("clangd"));

  ScopedEnvVar path(scope.arena, "PATH", bin);
  ScopedWorkingDirectory cwd(scope.arena);
  CHECK(cwd.Set(cwd_dir));
#if defined(_WIN32)
  ScopedEnvVar pathext(scope.arena, "PATHEXT", Str8Lit(".EXE"));
#endif

  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("project"));
  CHECK(OsMakeDirs(root));

  LspServerCommand command = {};
  CHECK(LspResolveServerCommand(scope.arena, LspLanguage::Cpp, root, &command));
  CheckCommand(command, LspLanguage::Cpp, Str8Lit("cpp"), clangd_path, root, nullptr, 0);

  CHECK(LspResolveServerCommand(scope.arena, LspLanguage::Rust, root, &command));
  CheckCommand(command, LspLanguage::Rust, Str8Lit("rust"), rust_path, root, nullptr, 0);

  CHECK(LspResolveServerCommand(scope.arena, LspLanguage::Go, root, &command));
  CheckCommand(command, LspLanguage::Go, Str8Lit("go"), gopls_path, root, nullptr, 0);

  CHECK(LspResolveServerCommand(scope.arena, LspLanguage::TypeScript, Str8Lit("typescriptreact"), root,
                                &command));
  String8 ts_args[] = {Str8Lit("--stdio")};
  CheckCommand(command, LspLanguage::TypeScript, Str8Lit("typescriptreact"), ts_path, root, ts_args,
               ArrayCount(ts_args));

  CHECK(LspResolveServerCommand(scope.arena, LspLanguage::Python, root, &command));
  String8 python_args[] = {Str8Lit("--stdio")};
  CheckCommand(command, LspLanguage::Python, Str8Lit("python"), basedpyright_path, root, python_args,
               ArrayCount(python_args));

  CHECK(OsFileDelete(basedpyright_path));
  CHECK(LspResolveServerCommand(scope.arena, LspLanguage::Python, root, &command));
  CheckCommand(command, LspLanguage::Python, Str8Lit("python"), pyright_path, root, python_args,
               ArrayCount(python_args));
}

TEST(lsp_registry_returns_false_and_zeros_command_when_server_is_missing) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "missing_command");

  String8 empty_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("empty-bin"));
  String8 cwd_dir = OsPathJoin(scope.arena, fixture.path, Str8Lit("cwd"));
  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("root"));
  String8 fake_home = OsPathJoin(scope.arena, fixture.path, Str8Lit("home"));
  CHECK(OsMakeDirs(empty_path));
  CHECK(OsMakeDirs(cwd_dir));
  CHECK(OsMakeDirs(root));
  CHECK(OsMakeDirs(fake_home));
  WriteExecutableFixture(scope.arena, cwd_dir, Str8Lit("clangd"));

  ScopedEnvVar path(scope.arena, "PATH", empty_path);
  ScopedEnvVar home(scope.arena, "HOME", fake_home);
#if defined(_WIN32)
  ScopedEnvVar userprofile(scope.arena, "USERPROFILE", fake_home);
  ScopedEnvVar pathext(scope.arena, "PATHEXT", Str8Lit(".EXE"));
#endif
  ScopedWorkingDirectory cwd(scope.arena);
  CHECK(cwd.Set(cwd_dir));

  LspServerCommand command = {};
  command.language = LspLanguage::COUNT;
  command.language_id = Str8Lit("stale");
  command.executable = Str8Lit("stale");
  command.arguments = (String8 *)1;
  command.argument_count = 99;
  command.root = Str8Lit("stale");

  CHECK(!LspResolveServerCommand(scope.arena, LspLanguage::Cpp, root, &command));
  CHECK_EQ((u64)command.language, (u64)LspLanguage::Cpp);
  CHECK_EQ(command.language_id.size, (u64)0);
  CHECK_EQ(command.executable.size, (u64)0);
  CHECK(command.arguments == nullptr);
  CHECK_EQ(command.argument_count, (u64)0);
  CHECK_EQ(command.root.size, (u64)0);
}

TEST(lsp_registry_falls_back_to_mason_bin_when_path_lacks_server) {
  ArenaScope scope;
  ScopedFixtureDir fixture(scope.arena, "mason_fallback");

  String8 empty_path = OsPathJoin(scope.arena, fixture.path, Str8Lit("empty-bin"));
  String8 fake_home = OsPathJoin(scope.arena, fixture.path, Str8Lit("home"));
  String8 mason_bin =
      OsPathJoin(scope.arena, fake_home, Str8Lit(".local/share/nvim/mason/bin"));
  String8 root = OsPathJoin(scope.arena, fixture.path, Str8Lit("root"));
  CHECK(OsMakeDirs(empty_path));
  CHECK(OsMakeDirs(mason_bin));
  CHECK(OsMakeDirs(root));

  String8 clangd_path = WriteExecutableFixture(scope.arena, mason_bin, Str8Lit("clangd"));

  ScopedEnvVar path(scope.arena, "PATH", empty_path);
  ScopedEnvVar home(scope.arena, "HOME", fake_home);
#if defined(_WIN32)
  ScopedEnvVar userprofile(scope.arena, "USERPROFILE", fake_home);
  ScopedEnvVar pathext(scope.arena, "PATHEXT", Str8Lit(".EXE"));
#endif

  LspServerCommand command = {};
  CHECK(LspResolveServerCommand(scope.arena, LspLanguage::Cpp, root, &command));
  CheckCommand(command, LspLanguage::Cpp, Str8Lit("cpp"), clangd_path, root, nullptr, 0);
}
