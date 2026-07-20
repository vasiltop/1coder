#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

enum class LspLanguage : u8 { Cpp, Rust, Go, Python, TypeScript, COUNT };

struct LspServerCommand {
  LspLanguage language;
  String8 language_id;
  String8 executable;
  String8 *arguments;
  u64 argument_count;
  String8 root;
};

[[nodiscard]] bool LspLanguageForPath(String8 path, LspLanguage *language, String8 *language_id);
[[nodiscard]] String8 LspFindProjectRoot(Arena *arena, LspLanguage language, String8 file_path);
[[nodiscard]] bool LspResolveServerCommand(Arena *arena, LspLanguage language, String8 root,
                                           LspServerCommand *command);
[[nodiscard]] bool LspResolveServerCommand(Arena *arena, LspLanguage language, String8 language_id,
                                           String8 root, LspServerCommand *command);
