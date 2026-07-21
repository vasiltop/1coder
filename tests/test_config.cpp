#include "base/base_arena.h"
#include "base/base_string.h"
#include "config/config.h"
#include "config/toml.h"
#include "editor/editor.h"
#include "input/keymap.h"
#include "lsp/registry.h"
#include "os/os_file.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
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
    path = PushStr8F(arena, "%.*s/build/test_config_%s_%d", (int)cwd.size, (char *)cwd.str, tag,
                     (int)serial);
    (void)OsDirDeleteRecursive(path);
    CHECK(OsMakeDirs(path));
  }

  ~ScopedFixtureDir() { (void)OsDirDeleteRecursive(path); }
};

}  // namespace

TEST(toml_parses_tables_strings_and_arrays) {
  ArenaScope scope;
  String8 text = Str8Lit(
      "# comment\n"
      "[font]\n"
      "path = \"/tmp/font.ttf\"\n"
      "size = 18.5\n"
      "\n"
      "[bindings.normal]\n"
      "\"gd\" = \"definition\"\n"
      "\n"
      "[lsp.python]\n"
      "command = \"pyright-langserver\"\n"
      "args = [\"--stdio\", \"--verbose\"]\n");

  TomlParseResult parsed = TomlParse(scope.arena, text);
  CHECK(parsed.error.size == 0);
  CHECK(parsed.root != nullptr);

  TomlValue *font = TomlTableGet(parsed.root, Str8Lit("font"));
  CHECK(font != nullptr);
  String8 path = {};
  CHECK(TomlGetString(TomlTableGet(font, Str8Lit("path")), &path));
  CHECK_STR(path, Str8Lit("/tmp/font.ttf"));
  f64 size = 0;
  CHECK(TomlGetF64(TomlTableGet(font, Str8Lit("size")), &size));
  CHECK(size > 18.0 && size < 19.0);

  TomlValue *lsp = TomlTableGet(parsed.root, Str8Lit("lsp"));
  TomlValue *python = TomlTableGet(lsp, Str8Lit("python"));
  CHECK(TomlArrayCount(TomlTableGet(python, Str8Lit("args"))) == 2);
}

TEST(config_parse_bindings_and_lsp) {
  ArenaScope scope;
  String8 text = Str8Lit(
      "[bindings.normal]\n"
      "\"gd\" = \"definition\"\n"
      "\"<leader>ff\" = \"find\"\n"
      "[lsp.python]\n"
      "command = \"pyright-langserver\"\n"
      "args = [\"--stdio\"]\n"
      "[theme]\n"
      "background = \"#112233\"\n"
      "[theme.syntax]\n"
      "keyword = \"#AABBCC\"\n");

  ConfigLoadResult loaded = ConfigParse(scope.arena, text, Str8Lit("test.toml"));
  CHECK(loaded.ok);
  CHECK_EQ(loaded.config.binding_count, 2);
  CHECK(loaded.config.lsp[(u64)LspLanguage::Python].present);
  CHECK_STR(loaded.config.lsp[(u64)LspLanguage::Python].command, Str8Lit("pyright-langserver"));
  CHECK_EQ(loaded.config.lsp[(u64)LspLanguage::Python].argument_count, 1);
  CHECK_EQ(loaded.config.color_count, 2);
}

TEST(config_parse_unknown_command_records_error) {
  ArenaScope scope;
  ConfigLoadResult loaded =
      ConfigParse(scope.arena, Str8Lit("[bindings.normal]\n\"x\" = \"not-a-real-command\"\n"),
                  Str8Lit("bad.toml"));
  CHECK(!loaded.ok);
  CHECK(loaded.error_log.size > 0);
}

TEST(config_missing_file_is_ok) {
  ArenaScope scope;
  ScopedFixtureDir dir(scope.arena, "missing");
  String8 path = OsPathJoin(scope.arena, dir.path, Str8Lit("nope.toml"));
  ConfigLoadResult loaded = ConfigLoadFile(scope.arena, path);
  CHECK(loaded.ok);
  CHECK(loaded.config.file_missing);
}

TEST(config_apply_bindings_overrides_defaults) {
  ArenaScope scope;
  Editor ed = {};
  EditorInit(&ed, scope.arena, RectS32{0, 0, 80, 24});

  KeymapLookup before = KeymapLookupSequence(ed.normal_map, Str8Lit("gd"));
  CHECK(before.command == CommandId::lsp_definition);

  Config config = {};
  ConfigBinding binding = {ConfigBindMap::Normal, Str8Lit("gd"), CommandId::lsp_hover};
  config.bindings = &binding;
  config.binding_count = 1;
  ConfigApplyBindings(&ed, &config);

  KeymapLookup after = KeymapLookupSequence(ed.normal_map, Str8Lit("gd"));
  CHECK(after.command == CommandId::lsp_hover);

  // Reloading with no overrides restores the default.
  Config empty = {};
  ConfigApplyBindings(&ed, &empty);
  KeymapLookup restored = KeymapLookupSequence(ed.normal_map, Str8Lit("gd"));
  CHECK(restored.command == CommandId::lsp_definition);

  EditorDestroy(&ed);
}

TEST(config_reload_keeps_prior_on_parse_error) {
  ArenaScope scope;
  ScopedFixtureDir dir(scope.arena, "reload");
  String8 path = OsPathJoin(scope.arena, dir.path, Str8Lit("config.toml"));
  CHECK(OsFileWrite(path, Str8Lit("[bindings.normal]\n\"gd\" = \"hover\"\n")));

  Editor ed = {};
  EditorInit(&ed, scope.arena, RectS32{0, 0, 80, 24});
  EditorConfigLoad(&ed, path, false);
  CHECK(!ed.config_has_errors);
  CHECK(KeymapLookupSequence(ed.normal_map, Str8Lit("gd")).command == CommandId::lsp_hover);

  CHECK(OsFileWrite(path, Str8Lit("[bindings.normal]\n\"gd\" = \n")));
  EditorConfigLoad(&ed, path, true);
  CHECK(ed.config_has_errors);
  CHECK(ed.config_error_log.size > 0);
  CHECK(KeymapLookupSequence(ed.normal_map, Str8Lit("gd")).command == CommandId::lsp_hover);
  CHECK(Str8FindFirst(ed.status_message, Str8Lit("config-error-log")) != ed.status_message.size);

  EditorDestroy(&ed);
}

TEST(config_example_file_parses) {
  ArenaScope scope;
  String8 cwd = OsGetCwd(scope.arena);
  String8 path = OsPathJoin(scope.arena, cwd, Str8Lit("config/config.example.toml"));
  if (!OsFileExists(path)) {
    // Running from build/ — try repo root relative to binary cwd variants.
    path = OsPathJoin(scope.arena, cwd, Str8Lit("../config/config.example.toml"));
  }
  CHECK(OsFileExists(path));
  ConfigLoadResult loaded = ConfigLoadFile(scope.arena, path);
  CHECK(loaded.ok);
  CHECK(loaded.config.binding_count > 100);
}
