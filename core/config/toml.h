#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

// Minimal TOML subset for ~/.config/1coder/config.toml:
// comments, tables, dotted keys, strings, bools, numbers, and string arrays.

enum class TomlKind : u8 { Null, Bool, Number, String, Array, Table };

struct TomlValue;

struct TomlMember {
  String8 name;
  TomlValue *value;
  TomlMember *next;
};

struct TomlValue {
  TomlKind kind;
  union {
    bool boolean;
    f64 number;
    String8 string;
    struct {
      TomlValue **items;
      u64 count;
    } array;
    TomlMember *table;
  };
};

struct TomlParseResult {
  TomlValue *root;
  String8 error;
  u64 error_line;
};

[[nodiscard]] TomlParseResult TomlParse(Arena *arena, String8 text);
[[nodiscard]] TomlValue *TomlTableGet(const TomlValue *table, String8 name);
[[nodiscard]] TomlValue *TomlArrayItem(const TomlValue *value, u64 index);
[[nodiscard]] u64 TomlArrayCount(const TomlValue *value);
[[nodiscard]] bool TomlGetString(const TomlValue *value, String8 *out);
[[nodiscard]] bool TomlGetBool(const TomlValue *value, bool *out);
[[nodiscard]] bool TomlGetF64(const TomlValue *value, f64 *out);
[[nodiscard]] bool TomlGetI64(const TomlValue *value, i64 *out);
