#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"
#include "command/command_id.h"
#include "lsp/registry.h"

struct Editor;

enum class ConfigBindMap : u8 {
  Global,
  Normal,
  Insert,
  Visual,
  Pending,
  Place,
  COUNT,
};

struct ConfigBinding {
  ConfigBindMap map;
  String8 spec;
  CommandId command;
};

struct ConfigLspOverride {
  LspLanguage language;
  String8 command;
  String8 *arguments;
  u64 argument_count;
  bool present;
};

struct ConfigColor {
  String8 name;  // theme field or "syntax.<kind>"
  String8 hex;   // #RRGGBB or #RRGGBBAA
};

struct Config {
  bool loaded;        // true when a file was read (even if empty)
  bool file_missing;  // true when the path did not exist
  String8 path;

  String8 font_path;
  String8 font_face;
  f32 font_size;  // 0 = unset
  bool has_font_path;
  bool has_font_face;
  bool has_font_size;

  ConfigBinding *bindings;
  u64 binding_count;

  ConfigLspOverride lsp[(u64)LspLanguage::COUNT];

  ConfigColor *colors;
  u64 color_count;
};

struct ConfigLoadResult {
  Config config;
  String8 error_log;  // multi-line details; empty on success
  bool ok;
};

// $XDG_CONFIG_HOME/1coder/config.toml when set; else ~/.config/1coder/config.toml
// on Unix, or %APPDATA%\1coder\config.toml on Windows.
[[nodiscard]] String8 ConfigDefaultPath(Arena *arena);

// Parse TOML text into a Config. Does not touch the editor.
[[nodiscard]] ConfigLoadResult ConfigParse(Arena *arena, String8 text, String8 path);

// Read and parse the default path (or `path` if non-empty). Missing file is ok.
[[nodiscard]] ConfigLoadResult ConfigLoadFile(Arena *arena, String8 path);

// Clear mode keymaps, reinstall defaults, apply [bindings.*].
void ConfigApplyBindings(Editor *ed, const Config *config);

// Prefer user [lsp.<lang>] command when present; otherwise built-in discovery.
[[nodiscard]] bool ConfigResolveLspCommand(Arena *arena, const Config *config, LspLanguage language,
                                           String8 language_id, String8 root,
                                           LspServerCommand *command);
