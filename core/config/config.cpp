#include "config/config.h"

#include "config/toml.h"
#include "editor/editor.h"
#include "input/keymap.h"
#include "os/os_file.h"
#include "os/os_process.h"

#include <stdarg.h>
#include <stdio.h>

namespace {

struct ErrorLog {
  Arena *arena;
  String8List lines;
};

void LogError(ErrorLog *log, String8 message) {
  if (!log || !log->arena || message.size == 0) return;
  Str8ListPush(log->arena, &log->lines, PushStr8Copy(log->arena, message));
}

void LogErrorF(ErrorLog *log, const char *fmt, ...) {
  if (!log || !log->arena) return;
  va_list args;
  va_start(args, fmt);
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  LogError(log, Str8C(buffer));
}

[[nodiscard]] String8 HomeDirectory(Arena *arena) {
  if (!arena) return String8{};
#if defined(_WIN32)
  String8 home = OsGetEnv(arena, Str8Lit("USERPROFILE"));
  if (home.size == 0) home = OsGetEnv(arena, Str8Lit("HOME"));
  return home;
#else
  return OsGetEnv(arena, Str8Lit("HOME"));
#endif
}

[[nodiscard]] bool BindMapFromName(String8 name, ConfigBindMap *out) {
  if (Str8Match(name, Str8Lit("global"))) {
    *out = ConfigBindMap::Global;
    return true;
  }
  if (Str8Match(name, Str8Lit("normal"))) {
    *out = ConfigBindMap::Normal;
    return true;
  }
  if (Str8Match(name, Str8Lit("insert"))) {
    *out = ConfigBindMap::Insert;
    return true;
  }
  if (Str8Match(name, Str8Lit("visual"))) {
    *out = ConfigBindMap::Visual;
    return true;
  }
  if (Str8Match(name, Str8Lit("pending")) || Str8Match(name, Str8Lit("operator-pending"))) {
    *out = ConfigBindMap::Pending;
    return true;
  }
  if (Str8Match(name, Str8Lit("place")) || Str8Match(name, Str8Lit("cursor-place"))) {
    *out = ConfigBindMap::Place;
    return true;
  }
  return false;
}

[[nodiscard]] bool LspLanguageFromName(String8 name, LspLanguage *out) {
  if (Str8Match(name, Str8Lit("cpp")) || Str8Match(name, Str8Lit("c"))) {
    *out = LspLanguage::Cpp;
    return true;
  }
  if (Str8Match(name, Str8Lit("rust"))) {
    *out = LspLanguage::Rust;
    return true;
  }
  if (Str8Match(name, Str8Lit("go"))) {
    *out = LspLanguage::Go;
    return true;
  }
  if (Str8Match(name, Str8Lit("python"))) {
    *out = LspLanguage::Python;
    return true;
  }
  if (Str8Match(name, Str8Lit("typescript")) || Str8Match(name, Str8Lit("javascript"))) {
    *out = LspLanguage::TypeScript;
    return true;
  }
  return false;
}

[[nodiscard]] Keymap *MapForBind(Editor *ed, ConfigBindMap map) {
  switch (map) {
    case ConfigBindMap::Global: return ed->global_map;
    case ConfigBindMap::Normal: return ed->normal_map;
    case ConfigBindMap::Insert: return ed->insert_map;
    case ConfigBindMap::Visual: return ed->visual_map;
    case ConfigBindMap::Pending: return ed->operator_pending_map;
    case ConfigBindMap::Place: return ed->cursor_place_map;
    case ConfigBindMap::COUNT: break;
  }
  return nullptr;
}

void PushBinding(Arena *arena, Config *config, ConfigBindMap map, String8 spec, CommandId command) {
  ConfigBinding *grown = PushArrayNoZero(arena, ConfigBinding, config->binding_count + 1);
  for (u64 i = 0; i < config->binding_count; i += 1) grown[i] = config->bindings[i];
  grown[config->binding_count] = ConfigBinding{map, PushStr8Copy(arena, spec), command};
  config->bindings = grown;
  config->binding_count += 1;
}

void PushColor(Arena *arena, Config *config, String8 name, String8 hex) {
  ConfigColor *grown = PushArrayNoZero(arena, ConfigColor, config->color_count + 1);
  for (u64 i = 0; i < config->color_count; i += 1) grown[i] = config->colors[i];
  grown[config->color_count] = ConfigColor{PushStr8Copy(arena, name), PushStr8Copy(arena, hex)};
  config->colors = grown;
  config->color_count += 1;
}

void CollectStringArray(Arena *arena, const TomlValue *value, String8 **out_items, u64 *out_count,
                        ErrorLog *log, String8 context) {
  *out_items = nullptr;
  *out_count = 0;
  if (!value) return;
  if (value->kind != TomlKind::Array) {
    LogErrorF(log, "%.*s: expected string array", (int)context.size, (char *)context.str);
    return;
  }
  u64 count = TomlArrayCount(value);
  if (count == 0) return;
  String8 *items = PushArrayNoZero(arena, String8, count);
  u64 written = 0;
  for (u64 i = 0; i < count; i += 1) {
    String8 s = {};
    if (!TomlGetString(TomlArrayItem(value, i), &s)) {
      LogErrorF(log, "%.*s: array items must be strings", (int)context.size, (char *)context.str);
      continue;
    }
    items[written] = PushStr8Copy(arena, s);
    written += 1;
  }
  *out_items = items;
  *out_count = written;
}

void ParseFontTable(Arena *arena, const TomlValue *table, Config *config, ErrorLog *log) {
  if (!table) return;
  if (TomlValue *path = TomlTableGet(table, Str8Lit("path"))) {
    String8 s = {};
    if (TomlGetString(path, &s)) {
      config->font_path = PushStr8Copy(arena, s);
      config->has_font_path = true;
    } else {
      LogError(log, Str8Lit("font.path: expected string"));
    }
  }
  if (TomlValue *face = TomlTableGet(table, Str8Lit("face"))) {
    String8 s = {};
    if (TomlGetString(face, &s)) {
      config->font_face = PushStr8Copy(arena, s);
      config->has_font_face = true;
    } else {
      LogError(log, Str8Lit("font.face: expected string"));
    }
  }
  if (TomlValue *size = TomlTableGet(table, Str8Lit("size"))) {
    f64 n = 0;
    if (TomlGetF64(size, &n) && n > 4.0) {
      config->font_size = (f32)n;
      config->has_font_size = true;
    } else {
      LogError(log, Str8Lit("font.size: expected number > 4"));
    }
  }
}

void ParseThemeTable(Arena *arena, const TomlValue *table, Config *config, ErrorLog *log,
                     String8 prefix) {
  if (!table || table->kind != TomlKind::Table) return;
  for (TomlMember *m = table->table; m; m = m->next) {
    if (m->value && m->value->kind == TomlKind::Table) {
      String8 nested = prefix.size == 0
                           ? PushStr8Cat(arena, m->name, Str8Lit(""))
                           : PushStr8Cat(arena, PushStr8Cat(arena, prefix, Str8Lit(".")), m->name);
      // For [theme.syntax], prefix becomes "syntax"
      String8 child_prefix =
          prefix.size == 0 ? m->name : PushStr8Cat(arena, PushStr8Cat(arena, prefix, Str8Lit(".")), m->name);
      ParseThemeTable(arena, m->value, config, log, child_prefix);
      (void)nested;
      continue;
    }
    String8 hex = {};
    if (!TomlGetString(m->value, &hex)) {
      LogErrorF(log, "theme.%.*s: expected color string", (int)m->name.size, (char *)m->name.str);
      continue;
    }
    String8 name = prefix.size == 0
                       ? PushStr8Copy(arena, m->name)
                       : PushStr8Cat(arena, PushStr8Cat(arena, prefix, Str8Lit(".")), m->name);
    PushColor(arena, config, name, hex);
  }
}

void ParseBindingsTable(Arena *arena, const TomlValue *table, Config *config, ErrorLog *log) {
  if (!table || table->kind != TomlKind::Table) return;
  for (TomlMember *map_member = table->table; map_member; map_member = map_member->next) {
    ConfigBindMap map = {};
    if (!BindMapFromName(map_member->name, &map)) {
      LogErrorF(log, "bindings.%.*s: unknown map (use global/normal/insert/visual/pending/place)",
                (int)map_member->name.size, (char *)map_member->name.str);
      continue;
    }
    if (!map_member->value || map_member->value->kind != TomlKind::Table) {
      LogErrorF(log, "bindings.%.*s: expected table of key = command", (int)map_member->name.size,
                (char *)map_member->name.str);
      continue;
    }
    for (TomlMember *bind = map_member->value->table; bind; bind = bind->next) {
      String8 command_name = {};
      if (!TomlGetString(bind->value, &command_name)) {
        LogErrorF(log, "bindings.%.*s: values must be command name strings",
                  (int)map_member->name.size, (char *)map_member->name.str);
        continue;
      }
      CommandId id = CommandIdFromName(command_name);
      if (id == CommandId::None) {
        LogErrorF(log, "bindings.%.*s: unknown command '%.*s'", (int)map_member->name.size,
                  (char *)map_member->name.str, (int)command_name.size, (char *)command_name.str);
        continue;
      }
      PushBinding(arena, config, map, bind->name, id);
    }
  }
}

void ParseLspTable(Arena *arena, const TomlValue *table, Config *config, ErrorLog *log) {
  if (!table || table->kind != TomlKind::Table) return;
  for (TomlMember *lang = table->table; lang; lang = lang->next) {
    LspLanguage language = {};
    if (!LspLanguageFromName(lang->name, &language)) {
      LogErrorF(log, "lsp.%.*s: unknown language", (int)lang->name.size, (char *)lang->name.str);
      continue;
    }
    if (!lang->value || lang->value->kind != TomlKind::Table) {
      LogErrorF(log, "lsp.%.*s: expected table with command/args", (int)lang->name.size,
                (char *)lang->name.str);
      continue;
    }
    ConfigLspOverride *override = &config->lsp[(u64)language];
    override->language = language;
    TomlValue *command = TomlTableGet(lang->value, Str8Lit("command"));
    String8 command_str = {};
    if (!command || !TomlGetString(command, &command_str) || command_str.size == 0) {
      LogErrorF(log, "lsp.%.*s.command: required string", (int)lang->name.size,
                (char *)lang->name.str);
      continue;
    }
    override->command = PushStr8Copy(arena, command_str);
    override->present = true;
    CollectStringArray(arena, TomlTableGet(lang->value, Str8Lit("args")), &override->arguments,
                       &override->argument_count, log,
                       PushStr8F(arena, "lsp.%.*s.args", (int)lang->name.size, (char *)lang->name.str));
  }
}

}  // namespace

String8 ConfigDefaultPath(Arena *arena) {
  if (!arena) return String8{};

  // Explicit XDG wins on every platform when set.
  String8 xdg = OsGetEnv(arena, Str8Lit("XDG_CONFIG_HOME"));
  if (xdg.size > 0) return OsPathJoin(arena, xdg, Str8Lit("1coder/config.toml"));

#if defined(_WIN32)
  // Native Windows convention: %APPDATA%\1coder\config.toml
  String8 appdata = OsGetEnv(arena, Str8Lit("APPDATA"));
  if (appdata.size > 0) return OsPathJoin(arena, appdata, Str8Lit("1coder/config.toml"));
  String8 home = HomeDirectory(arena);
  if (home.size == 0) return String8{};
  return OsPathJoin(arena, home, Str8Lit("AppData/Roaming/1coder/config.toml"));
#else
  String8 home = HomeDirectory(arena);
  if (home.size == 0) return String8{};
  return OsPathJoin(arena, home, Str8Lit(".config/1coder/config.toml"));
#endif
}

ConfigLoadResult ConfigParse(Arena *arena, String8 text, String8 path) {
  ConfigLoadResult result = {};
  result.config.path = PushStr8Copy(arena, path);

  ErrorLog log = {};
  log.arena = arena;

  TomlParseResult parsed = TomlParse(arena, text);
  if (parsed.error.size > 0) {
    result.ok = false;
    if (parsed.error_line > 0) {
      result.error_log = PushStr8F(arena, "%.*s:%llu: %.*s", (int)path.size, (char *)path.str,
                                   (unsigned long long)parsed.error_line, (int)parsed.error.size,
                                   (char *)parsed.error.str);
    } else {
      result.error_log = PushStr8F(arena, "%.*s: %.*s", (int)path.size, (char *)path.str,
                                   (int)parsed.error.size, (char *)parsed.error.str);
    }
    return result;
  }

  result.config.loaded = true;
  TomlValue *root = parsed.root;
  ParseFontTable(arena, TomlTableGet(root, Str8Lit("font")), &result.config, &log);
  ParseThemeTable(arena, TomlTableGet(root, Str8Lit("theme")), &result.config, &log, String8{});
  ParseBindingsTable(arena, TomlTableGet(root, Str8Lit("bindings")), &result.config, &log);
  ParseLspTable(arena, TomlTableGet(root, Str8Lit("lsp")), &result.config, &log);

  if (log.lines.node_count > 0) {
    result.ok = false;
    result.error_log = Str8ListJoin(arena, &log.lines, Str8Lit("\n"));
    return result;
  }

  result.ok = true;
  return result;
}

ConfigLoadResult ConfigLoadFile(Arena *arena, String8 path) {
  ConfigLoadResult result = {};
  if (!arena) {
    result.error_log = Str8Lit("config: missing arena");
    return result;
  }

  String8 resolved = path.size > 0 ? PushStr8Copy(arena, path) : ConfigDefaultPath(arena);
  result.config.path = resolved;
  if (resolved.size == 0) {
    result.config.file_missing = true;
    result.ok = true;
    return result;
  }

  if (!OsFileExists(resolved)) {
    result.config.file_missing = true;
    result.ok = true;
    return result;
  }

  FileContents contents = OsFileRead(arena, resolved);
  if (!contents.ok) {
    result.ok = false;
    result.error_log =
        PushStr8F(arena, "%.*s: failed to read file", (int)resolved.size, (char *)resolved.str);
    return result;
  }

  return ConfigParse(arena, contents.data, resolved);
}

void ConfigApplyBindings(Editor *ed, const Config *config) {
  if (!ed) return;

  EditorCancelPendingInput(ed);
  KeymapClear(ed->global_map);
  KeymapClear(ed->normal_map);
  KeymapClear(ed->insert_map);
  KeymapClear(ed->visual_map);
  KeymapClear(ed->operator_pending_map);
  KeymapClear(ed->cursor_place_map);
  EditorInstallDefaultBindings(ed);

  if (!config) return;
  for (u64 i = 0; i < config->binding_count; i += 1) {
    const ConfigBinding *binding = &config->bindings[i];
    Keymap *map = MapForBind(ed, binding->map);
    if (!map) continue;
    (void)KeymapBind(map, binding->spec, binding->command);
  }
}

bool ConfigResolveLspCommand(Arena *arena, const Config *config, LspLanguage language,
                             String8 language_id, String8 root, LspServerCommand *command) {
  if (command) *command = {};
  if (!arena || !command) return false;

  if (config && (u64)language < (u64)LspLanguage::COUNT) {
    const ConfigLspOverride *override = &config->lsp[(u64)language];
    if (override->present && override->command.size > 0) {
      String8 executable = OsFindExecutable(arena, override->command);
      if (executable.size == 0 && OsFileExists(override->command)) {
        executable = PushStr8Copy(arena, override->command);
      }
      if (executable.size == 0) {
        // Fall through to built-ins when the preferred server is missing.
      } else {
        command->language = language;
        command->language_id = PushStr8Copy(arena, language_id);
        command->executable = executable;
        if (override->argument_count > 0) {
          command->arguments = PushArrayNoZero(arena, String8, override->argument_count);
          for (u64 i = 0; i < override->argument_count; i += 1) {
            command->arguments[i] = PushStr8Copy(arena, override->arguments[i]);
          }
        }
        command->argument_count = override->argument_count;
        command->root = PushStr8Copy(arena, root);
        return true;
      }
    }
  }

  return LspResolveServerCommand(arena, language, language_id, root, command);
}
