#include "lsp/registry.h"

#include "os/os_file.h"
#include "os/os_process.h"

namespace {

struct LspExtensionMapping {
  String8 extension;
  LspLanguage language;
  String8 language_id;
};

struct LspServerCandidate {
  String8 executable;
  const String8 *arguments;
  u64 argument_count;
};

struct LspLanguageConfig {
  LspLanguage language;
  String8 default_language_id;
  const String8 *root_markers;
  u64 root_marker_count;
  const LspServerCandidate *server_candidates;
  u64 server_candidate_count;
};

bool IsPathSep(u8 c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

bool PathIsAbsolute(String8 path) {
  if (path.size == 0) return false;
  if (IsPathSep(path.str[0])) return true;
#if defined(_WIN32)
  return path.size >= 3 && path.str[1] == ':' && IsPathSep(path.str[2]);
#else
  return false;
#endif
}

String8 RootPrefix(String8 path) {
#if defined(_WIN32)
  if (path.size >= 3 && path.str[1] == ':' && IsPathSep(path.str[2])) return Str8Prefix(path, 3);
#endif
  if (path.size > 0 && IsPathSep(path.str[0])) return Str8Lit("/");
  return String8{};
}

String8 ParentDirectory(String8 path) {
  if (Str8PathIsRoot(path)) return path;

  u64 slash = Str8FindLastChar(path, '/');
#if defined(_WIN32)
  u64 backslash = Str8FindLastChar(path, '\\');
  if (backslash != path.size && (slash == path.size || backslash > slash)) slash = backslash;
#endif
  if (slash == path.size) return String8{};
  if (slash == 0) return RootPrefix(path);
#if defined(_WIN32)
  if (slash == 2 && path.size >= 3 && path.str[1] == ':' && IsPathSep(path.str[2])) return RootPrefix(path);
#endif
  return Str8Prefix(path, slash);
}

String8 LexicallyNormalizeAbsolutePath(Arena *arena, String8 path) {
  TempArena scratch = ScratchBegin1(arena);
  String8List components = {};
  u64 root_size = RootPrefix(path).size;

  for (u64 start = root_size; start < path.size;) {
    while (start < path.size && IsPathSep(path.str[start])) start += 1;
    if (start >= path.size) break;

    u64 end = start;
    while (end < path.size && !IsPathSep(path.str[end])) end += 1;
    String8 component = Str8Substr(path, RangeU64{start, end});

    if (Str8Match(component, Str8Lit("."))) {
      start = end + 1;
      continue;
    }
    if (Str8Match(component, Str8Lit(".."))) {
      if (components.last) {
        String8Node *node = components.first;
        String8Node *prev = nullptr;
        while (node && node->next) {
          prev = node;
          node = node->next;
        }
        if (prev) {
          prev->next = nullptr;
          components.last = prev;
        } else {
          components.first = components.last = nullptr;
        }
        components.node_count -= 1;
        components.total_size -= node->string.size;
      }
      start = end + 1;
      continue;
    }

    Str8ListPush(scratch.arena, &components, component);
    start = end + 1;
  }

  String8 root = RootPrefix(path);
  String8 normalized = String8{};
  if (components.node_count == 0) {
    normalized = (root.size > 0) ? root : path;
  } else {
    String8 joined = Str8ListJoin(scratch.arena, &components, Str8Lit("/"));
    normalized = (root.size > 0) ? PushStr8Cat(arena, root, joined) : PushStr8Copy(arena, joined);
  }

  ScratchEnd(scratch);
  return normalized;
}

String8 NormalizeFilePath(Arena *arena, String8 file_path) {
  TempArena scratch = ScratchBegin1(arena);
  String8 absolute = OsPathAbsolute(scratch.arena, file_path);
  if (PathIsAbsolute(absolute)) {
    String8 normalized = LexicallyNormalizeAbsolutePath(arena, absolute);
    ScratchEnd(scratch);
    return normalized;
  }

  String8 cwd = OsGetCwd(scratch.arena);
  if (!PathIsAbsolute(cwd)) {
    String8 fallback = PushStr8Copy(arena, file_path);
    ScratchEnd(scratch);
    return fallback;
  }

  String8 combined = OsPathJoin(scratch.arena, cwd, file_path);
  String8 normalized = LexicallyNormalizeAbsolutePath(arena, combined);
  ScratchEnd(scratch);
  return normalized;
}

bool DirectoryHasMarker(String8 directory, const LspLanguageConfig *config) {
  for (u64 i = 0; i < config->root_marker_count; i += 1) {
    TempArena scratch = ScratchBegin();
    String8 marker_path = OsPathJoin(scratch.arena, directory, config->root_markers[i]);
    bool found = OsFileExists(marker_path) || OsDirExists(marker_path);
    ScratchEnd(scratch);
    if (found) return true;
  }
  return false;
}

String8 DefaultLanguageId(LspLanguage language) {
  switch (language) {
    case LspLanguage::Cpp: return Str8Lit("cpp");
    case LspLanguage::Rust: return Str8Lit("rust");
    case LspLanguage::Go: return Str8Lit("go");
    case LspLanguage::Python: return Str8Lit("python");
    case LspLanguage::TypeScript: return Str8Lit("typescript");
    case LspLanguage::COUNT: break;
  }
  return String8{};
}

const String8 kCppMarkers[] = {Str8LitComp("compile_commands.json"), Str8LitComp("compile_flags.txt"),
                               Str8LitComp("CMakeLists.txt"), Str8LitComp(".git")};
const String8 kRustMarkers[] = {Str8LitComp("Cargo.toml"), Str8LitComp(".git")};
const String8 kGoMarkers[] = {Str8LitComp("go.work"), Str8LitComp("go.mod"), Str8LitComp(".git")};
const String8 kPythonMarkers[] = {Str8LitComp("pyproject.toml"), Str8LitComp("setup.py"),
                                  Str8LitComp("setup.cfg"), Str8LitComp("requirements.txt"),
                                  Str8LitComp(".git")};
const String8 kTypeScriptMarkers[] = {Str8LitComp("tsconfig.json"), Str8LitComp("jsconfig.json"),
                                      Str8LitComp("package.json"), Str8LitComp(".git")};

const String8 kStdioArgs[] = {Str8LitComp("--stdio")};

const LspServerCandidate kCppServers[] = {{Str8LitComp("clangd"), nullptr, 0}};
const LspServerCandidate kRustServers[] = {
    {Str8LitComp("rust-analyzer"), nullptr, 0}};
const LspServerCandidate kGoServers[] = {{Str8LitComp("gopls"), nullptr, 0}};
const LspServerCandidate kPythonServers[] = {
    {Str8LitComp("basedpyright-langserver"), kStdioArgs, ArrayCount(kStdioArgs)},
    {Str8LitComp("pyright-langserver"), kStdioArgs, ArrayCount(kStdioArgs)},
};
const LspServerCandidate kTypeScriptServers[] = {
    {Str8LitComp("typescript-language-server"), kStdioArgs, ArrayCount(kStdioArgs)}};

const LspLanguageConfig kLanguageConfigs[] = {
    {LspLanguage::Cpp, Str8LitComp("cpp"), kCppMarkers, ArrayCount(kCppMarkers), kCppServers,
     ArrayCount(kCppServers)},
    {LspLanguage::Rust, Str8LitComp("rust"), kRustMarkers, ArrayCount(kRustMarkers), kRustServers,
     ArrayCount(kRustServers)},
    {LspLanguage::Go, Str8LitComp("go"), kGoMarkers, ArrayCount(kGoMarkers), kGoServers,
     ArrayCount(kGoServers)},
    {LspLanguage::Python, Str8LitComp("python"), kPythonMarkers, ArrayCount(kPythonMarkers),
     kPythonServers, ArrayCount(kPythonServers)},
    {LspLanguage::TypeScript, Str8LitComp("typescript"), kTypeScriptMarkers,
     ArrayCount(kTypeScriptMarkers), kTypeScriptServers, ArrayCount(kTypeScriptServers)},
};

const LspExtensionMapping kExtensionMappings[] = {
    {Str8LitComp("c"), LspLanguage::Cpp, Str8LitComp("c")},
    {Str8LitComp("h"), LspLanguage::Cpp, Str8LitComp("cpp")},
    {Str8LitComp("cc"), LspLanguage::Cpp, Str8LitComp("cpp")},
    {Str8LitComp("cpp"), LspLanguage::Cpp, Str8LitComp("cpp")},
    {Str8LitComp("cxx"), LspLanguage::Cpp, Str8LitComp("cpp")},
    {Str8LitComp("hh"), LspLanguage::Cpp, Str8LitComp("cpp")},
    {Str8LitComp("hpp"), LspLanguage::Cpp, Str8LitComp("cpp")},
    {Str8LitComp("hxx"), LspLanguage::Cpp, Str8LitComp("cpp")},
    {Str8LitComp("rs"), LspLanguage::Rust, Str8LitComp("rust")},
    {Str8LitComp("go"), LspLanguage::Go, Str8LitComp("go")},
    {Str8LitComp("py"), LspLanguage::Python, Str8LitComp("python")},
    {Str8LitComp("pyi"), LspLanguage::Python, Str8LitComp("python")},
    {Str8LitComp("js"), LspLanguage::TypeScript, Str8LitComp("javascript")},
    {Str8LitComp("mjs"), LspLanguage::TypeScript, Str8LitComp("javascript")},
    {Str8LitComp("cjs"), LspLanguage::TypeScript, Str8LitComp("javascript")},
    {Str8LitComp("jsx"), LspLanguage::TypeScript, Str8LitComp("javascriptreact")},
    {Str8LitComp("ts"), LspLanguage::TypeScript, Str8LitComp("typescript")},
    {Str8LitComp("mts"), LspLanguage::TypeScript, Str8LitComp("typescript")},
    {Str8LitComp("cts"), LspLanguage::TypeScript, Str8LitComp("typescript")},
    {Str8LitComp("tsx"), LspLanguage::TypeScript, Str8LitComp("typescriptreact")},
};

const LspLanguageConfig *GetLanguageConfig(LspLanguage language) {
  for (u64 i = 0; i < ArrayCount(kLanguageConfigs); i += 1) {
    if (kLanguageConfigs[i].language == language) return &kLanguageConfigs[i];
  }
  return nullptr;
}

void ZeroCommand(LspServerCommand *command) {
  if (command) *command = {};
}

}  // namespace

bool LspLanguageForPath(String8 path, LspLanguage *language, String8 *language_id) {
  if (language) *language = {};
  if (language_id) *language_id = {};

  String8 extension = Str8PathExt(path);
  if (extension.size == 0) return false;

  for (u64 i = 0; i < ArrayCount(kExtensionMappings); i += 1) {
    const LspExtensionMapping *mapping = &kExtensionMappings[i];
    if (!Str8Match(extension, mapping->extension, StringMatch::CaseInsensitive)) continue;
    if (language) *language = mapping->language;
    if (language_id) *language_id = mapping->language_id;
    return true;
  }
  return false;
}

String8 LspFindProjectRoot(Arena *arena, LspLanguage language, String8 file_path) {
  if (!arena) return String8{};

  TempArena scratch = ScratchBegin1(arena);
  const LspLanguageConfig *config = GetLanguageConfig(language);
  String8 absolute = NormalizeFilePath(scratch.arena, file_path);
  String8 fallback = ParentDirectory(absolute);
  if (fallback.size == 0) fallback = absolute;
  if (!config) {
    String8 result = PushStr8Copy(arena, fallback);
    ScratchEnd(scratch);
    return result;
  }

  String8 current = fallback;
  for (;;) {
    if (current.size == 0) break;
    if (DirectoryHasMarker(current, config)) {
      String8 result = PushStr8Copy(arena, current);
      ScratchEnd(scratch);
      return result;
    }
    if (Str8PathIsRoot(current)) break;
    String8 parent = ParentDirectory(current);
    if (parent.size == 0 || Str8Match(parent, current)) break;
    current = parent;
  }

  String8 result = PushStr8Copy(arena, fallback);
  ScratchEnd(scratch);
  return result;
}

bool LspResolveServerCommand(Arena *arena, LspLanguage language, String8 root, LspServerCommand *command) {
  return LspResolveServerCommand(arena, language, DefaultLanguageId(language), root, command);
}

bool LspResolveServerCommand(Arena *arena, LspLanguage language, String8 language_id, String8 root,
                             LspServerCommand *command) {
  ZeroCommand(command);
  if (!arena || !command) return false;

  const LspLanguageConfig *config = GetLanguageConfig(language);
  if (!config) return false;
  if (language_id.size == 0) language_id = config->default_language_id;

  for (u64 i = 0; i < config->server_candidate_count; i += 1) {
    const LspServerCandidate *candidate = &config->server_candidates[i];
    String8 executable = OsFindExecutable(arena, candidate->executable);
    if (executable.size == 0) continue;

    command->language = language;
    command->language_id = PushStr8Copy(arena, language_id);
    command->executable = PushStr8Copy(arena, executable);
    if (candidate->argument_count > 0) {
      command->arguments = PushArrayNoZero(arena, String8, candidate->argument_count);
      for (u64 arg = 0; arg < candidate->argument_count; arg += 1) {
        command->arguments[arg] = PushStr8Copy(arena, candidate->arguments[arg]);
      }
    }
    command->argument_count = candidate->argument_count;
    command->root = PushStr8Copy(arena, root);
    return true;
  }

  ZeroCommand(command);
  return false;
}
