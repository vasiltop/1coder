#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"
#include "base/base_types.h"

enum class JsonKind : u8 { Null, Bool, Number, String, Array, Object };

struct JsonValue;

struct JsonMember {
  String8 name;
  JsonValue *value;
  JsonMember *next;
};

struct JsonValue {
  JsonKind kind;
  union {
    bool boolean;
    f64 number;
    String8 string;
    struct {
      JsonValue **items;
      u64 count;
    } array;
    JsonMember *object;
  };
};

struct JsonParseResult {
  JsonValue *root;
  String8 error;
  u64 error_offset;
};

[[nodiscard]] JsonParseResult JsonParse(Arena *arena, String8 text);
[[nodiscard]] JsonValue *JsonObjectGet(const JsonValue *object, String8 name);
[[nodiscard]] JsonValue *JsonArrayItem(const JsonValue *value, u64 index);
[[nodiscard]] u64 JsonArrayCount(const JsonValue *value);
[[nodiscard]] bool JsonGetString(const JsonValue *value, String8 *out);
[[nodiscard]] bool JsonGetBool(const JsonValue *value, bool *out);
[[nodiscard]] bool JsonGetF64(const JsonValue *value, f64 *out);
[[nodiscard]] bool JsonGetI64(const JsonValue *value, i64 *out);
[[nodiscard]] bool JsonGetU64(const JsonValue *value, u64 *out);

enum class JsonWriterScopeKind : u8 { Array, Object };

struct JsonWriterScope {
  JsonWriterScopeKind kind;
  u64 count;
  bool object_expects_key;
};

struct JsonWriter {
  Arena *arena;
  u8 *buffer;
  u64 size;
  u64 capacity;
  String8 error;
  bool root_written;
  u64 scope_count;
  JsonWriterScope scopes[128];
};

void JsonWriterInit(JsonWriter *writer, Arena *arena);
void JsonWriterDestroy(JsonWriter *writer);
[[nodiscard]] String8 JsonWriterError(const JsonWriter *writer);
[[nodiscard]] bool JsonWriteObjectBegin(JsonWriter *writer);
[[nodiscard]] bool JsonWriteObjectEnd(JsonWriter *writer);
[[nodiscard]] bool JsonWriteArrayBegin(JsonWriter *writer);
[[nodiscard]] bool JsonWriteArrayEnd(JsonWriter *writer);
[[nodiscard]] bool JsonWriteObjectKey(JsonWriter *writer, String8 key);
[[nodiscard]] bool JsonWriteNull(JsonWriter *writer);
[[nodiscard]] bool JsonWriteBool(JsonWriter *writer, bool value);
[[nodiscard]] bool JsonWriteI64(JsonWriter *writer, i64 value);
[[nodiscard]] bool JsonWriteU64(JsonWriter *writer, u64 value);
[[nodiscard]] bool JsonWriteF64(JsonWriter *writer, f64 value);
[[nodiscard]] bool JsonWriteString(JsonWriter *writer, String8 value);
[[nodiscard]] String8 JsonWriterFinish(JsonWriter *writer);
