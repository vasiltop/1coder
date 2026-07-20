#include "lsp/json.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

inline constexpr u64 kJsonMaxInputBytes = MB(16);
inline constexpr u64 kJsonMaxDepth = 128;

struct JsonParser {
  Arena *arena;
  String8 text;
  u64 at;
  String8 error;
  u64 error_offset;
};

struct JsonValueNode {
  JsonValueNode *next;
  JsonValue *value;
};

struct JsonValueExt {
  JsonValue base;
  bool has_exact_i64;
  bool has_exact_u64;
  i64 exact_i64;
  u64 exact_u64;
};

String8 JsonError(String8 message) { return message; }

bool JsonSetError(JsonParser *parser, String8 message, u64 offset) {
  if (parser->error.size == 0) {
    parser->error = message;
    parser->error_offset = Min(offset, parser->text.size);
  }
  return false;
}

void JsonSkipWhitespace(JsonParser *parser) {
  while (parser->at < parser->text.size && CharIsSpace(parser->text.str[parser->at])) {
    parser->at += 1;
  }
}

bool JsonConsume(JsonParser *parser, u8 expected) {
  if (parser->at < parser->text.size && parser->text.str[parser->at] == expected) {
    parser->at += 1;
    return true;
  }
  return false;
}

u32 JsonHexValue(u8 c) {
  if (c >= '0' && c <= '9') return (u32)(c - '0');
  if (c >= 'a' && c <= 'f') return (u32)(10 + c - 'a');
  if (c >= 'A' && c <= 'F') return (u32)(10 + c - 'A');
  return UINT32_MAX;
}

bool JsonReadHex4(JsonParser *parser, u32 *out) {
  if (parser->at + 4 > parser->text.size) {
    return JsonSetError(parser, JsonError(Str8Lit("truncated unicode escape")), parser->at);
  }

  u32 value = 0;
  for (u64 i = 0; i < 4; i += 1) {
    u32 hex = JsonHexValue(parser->text.str[parser->at + i]);
    if (hex == UINT32_MAX) {
      return JsonSetError(parser, JsonError(Str8Lit("invalid unicode escape")), parser->at + i);
    }
    value = (value << 4) | hex;
  }

  parser->at += 4;
  *out = value;
  return true;
}

JsonValue *JsonParseValue(JsonParser *parser, u64 depth);

JsonValue *JsonPushValue(Arena *arena, JsonKind kind) {
  JsonValueExt *value = PushStruct(arena, JsonValueExt);
  memset(value, 0, sizeof(*value));
  value->base.kind = kind;
  return &value->base;
}

JsonValueExt *JsonValueMetadata(JsonValue *value) {
  return (JsonValueExt *)value;
}

const JsonValueExt *JsonValueMetadata(const JsonValue *value) {
  return (const JsonValueExt *)value;
}

bool JsonParseU64Exact(String8 digits, u64 *out) {
  if (digits.size == 0) return false;
  u64 value = 0;
  for (u64 i = 0; i < digits.size; i += 1) {
    if (!CharIsDigit(digits.str[i])) return false;
    u64 digit = (u64)(digits.str[i] - '0');
    if (value > (ULLONG_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

void JsonSetExactIntegerMetadata(JsonValue *value, String8 token, f64 number) {
  JsonValueExt *meta = JsonValueMetadata(value);
  meta->has_exact_i64 = false;
  meta->has_exact_u64 = false;

  bool has_fraction_or_exponent =
      Str8FindFirstChar(token, '.') != token.size ||
      Str8FindFirstChar(token, 'e') != token.size ||
      Str8FindFirstChar(token, 'E') != token.size;

  if (!has_fraction_or_exponent) {
    bool negative = token.size > 0 && token.str[0] == '-';
    String8 digits = negative ? Str8Skip(token, 1) : token;
    u64 magnitude = 0;
    if (JsonParseU64Exact(digits, &magnitude)) {
      if (!negative && (u64)(f64)magnitude == magnitude && (f64)magnitude == number) {
        meta->has_exact_u64 = true;
        meta->exact_u64 = magnitude;
      }
      if (magnitude <= (u64)LLONG_MAX) {
        i64 signed_value = negative ? -(i64)magnitude : (i64)magnitude;
        if ((i64)(f64)signed_value == signed_value && (f64)signed_value == number) {
          meta->has_exact_i64 = true;
          meta->exact_i64 = signed_value;
        }
      }
    }
    return;
  }

  if (trunc(number) != number) return;
  if (number >= 0 && number <= (f64)ULLONG_MAX) {
    u64 unsigned_value = (u64)number;
    if ((f64)unsigned_value == number) {
      meta->has_exact_u64 = true;
      meta->exact_u64 = unsigned_value;
    }
  }
  if (number >= (f64)LLONG_MIN && number <= (f64)LLONG_MAX) {
    i64 signed_value = (i64)number;
    if ((f64)signed_value == number) {
      meta->has_exact_i64 = true;
      meta->exact_i64 = signed_value;
    }
  }
}

String8 JsonParseStringBytes(JsonParser *parser) {
  u64 quote_offset = parser->at;
  if (!JsonConsume(parser, '"')) {
    JsonSetError(parser, JsonError(Str8Lit("expected string")), parser->at);
    return {};
  }

  u64 max_output = parser->text.size - parser->at;
  u8 *out = PushArray(parser->arena, u8, max_output + 1);
  u64 out_size = 0;

  while (parser->at < parser->text.size) {
    u8 c = parser->text.str[parser->at++];
    if (c == '"') {
      out[out_size] = 0;
      return Str8(out, out_size);
    }
    if (c < 0x20) {
      JsonSetError(parser, JsonError(Str8Lit("unescaped control character in string")), parser->at - 1);
      return {};
    }
    if (c != '\\') {
      out[out_size++] = c;
      continue;
    }

    if (parser->at >= parser->text.size) {
      JsonSetError(parser, JsonError(Str8Lit("truncated escape sequence")), parser->at);
      return {};
    }

    u8 escape = parser->text.str[parser->at++];
    switch (escape) {
      case '"': out[out_size++] = '"'; break;
      case '\\': out[out_size++] = '\\'; break;
      case '/': out[out_size++] = '/'; break;
      case 'b': out[out_size++] = '\b'; break;
      case 'f': out[out_size++] = '\f'; break;
      case 'n': out[out_size++] = '\n'; break;
      case 'r': out[out_size++] = '\r'; break;
      case 't': out[out_size++] = '\t'; break;
      case 'u': {
        u64 escape_offset = parser->at - 2;
        u32 codepoint = 0;
        if (!JsonReadHex4(parser, &codepoint)) return {};

        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
          if (parser->at + 2 > parser->text.size ||
              parser->text.str[parser->at] != '\\' ||
              parser->text.str[parser->at + 1] != 'u') {
            JsonSetError(parser, JsonError(Str8Lit("lone high surrogate")), escape_offset);
            return {};
          }
          parser->at += 2;

          u32 low = 0;
          if (!JsonReadHex4(parser, &low)) return {};
          if (low < 0xDC00 || low > 0xDFFF) {
            JsonSetError(parser, JsonError(Str8Lit("invalid surrogate pair")), parser->at - 4);
            return {};
          }
          codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
          JsonSetError(parser, JsonError(Str8Lit("lone low surrogate")), escape_offset);
          return {};
        }

        out_size += Utf8Encode(out + out_size, codepoint);
      } break;
      default: {
        JsonSetError(parser, JsonError(Str8Lit("invalid escape sequence")), parser->at - 1);
        return {};
      } break;
    }
  }

  JsonSetError(parser, JsonError(Str8Lit("unterminated string")), quote_offset);
  return {};
}

JsonValue *JsonParseString(JsonParser *parser) {
  String8 text = JsonParseStringBytes(parser);
  if (parser->error.size != 0) return nullptr;
  JsonValue *value = JsonPushValue(parser->arena, JsonKind::String);
  value->string = text;
  return value;
}

bool JsonIsNumberTerminator(u8 c) {
  return CharIsSpace(c) || c == ',' || c == ']' || c == '}';
}

JsonValue *JsonParseNumber(JsonParser *parser) {
  u64 start = parser->at;

  if (JsonConsume(parser, '-')) {
    if (parser->at >= parser->text.size) {
      JsonSetError(parser, JsonError(Str8Lit("truncated number")), parser->at);
      return nullptr;
    }
  }

  if (parser->at >= parser->text.size || !CharIsDigit(parser->text.str[parser->at])) {
    JsonSetError(parser, JsonError(Str8Lit("expected digit")), parser->at);
    return nullptr;
  }

  if (parser->text.str[parser->at] == '0') {
    parser->at += 1;
    if (parser->at < parser->text.size && CharIsDigit(parser->text.str[parser->at])) {
      JsonSetError(parser, JsonError(Str8Lit("leading zero is not allowed")), parser->at);
      return nullptr;
    }
  } else {
    while (parser->at < parser->text.size && CharIsDigit(parser->text.str[parser->at])) {
      parser->at += 1;
    }
  }

  if (JsonConsume(parser, '.')) {
    if (parser->at >= parser->text.size || !CharIsDigit(parser->text.str[parser->at])) {
      JsonSetError(parser, JsonError(Str8Lit("expected fractional digit")), parser->at);
      return nullptr;
    }
    while (parser->at < parser->text.size && CharIsDigit(parser->text.str[parser->at])) {
      parser->at += 1;
    }
  }

  if (parser->at < parser->text.size &&
      (parser->text.str[parser->at] == 'e' || parser->text.str[parser->at] == 'E')) {
    parser->at += 1;
    if (parser->at < parser->text.size &&
        (parser->text.str[parser->at] == '+' || parser->text.str[parser->at] == '-')) {
      parser->at += 1;
    }
    if (parser->at >= parser->text.size || !CharIsDigit(parser->text.str[parser->at])) {
      JsonSetError(parser, JsonError(Str8Lit("expected exponent digit")), parser->at);
      return nullptr;
    }
    while (parser->at < parser->text.size && CharIsDigit(parser->text.str[parser->at])) {
      parser->at += 1;
    }
  }

  if (parser->at < parser->text.size && !JsonIsNumberTerminator(parser->text.str[parser->at])) {
    JsonSetError(parser, JsonError(Str8Lit("invalid character after number")), parser->at);
    return nullptr;
  }

  String8 token = Str8(parser->text.str + start, parser->at - start);
  TempArena scratch = ScratchBegin1(parser->arena);
  defer { ScratchEnd(scratch); };
  const char *cstr = PushCStr(scratch.arena, token);

  errno = 0;
  char *end = nullptr;
  f64 number = strtod(cstr, &end);
  if (end == nullptr || (u64)(end - cstr) != token.size) {
    JsonSetError(parser, JsonError(Str8Lit("invalid number")), start);
    return nullptr;
  }
  if (errno == ERANGE || !isfinite(number)) {
    JsonSetError(parser, JsonError(Str8Lit("number is out of range")), start);
    return nullptr;
  }

  JsonValue *value = JsonPushValue(parser->arena, JsonKind::Number);
  value->number = number;
  JsonSetExactIntegerMetadata(value, token, number);
  return value;
}

bool JsonConsumeKeyword(JsonParser *parser, String8 keyword) {
  if (parser->at + keyword.size > parser->text.size) return false;
  if (!Str8Match(Str8(parser->text.str + parser->at, keyword.size), keyword)) return false;
  parser->at += keyword.size;
  return true;
}

JsonValue *JsonParseArray(JsonParser *parser, u64 depth) {
  if (depth >= kJsonMaxDepth) {
    JsonSetError(parser, JsonError(Str8Lit("maximum nesting depth exceeded")), parser->at);
    return nullptr;
  }

  JsonConsume(parser, '[');
  JsonSkipWhitespace(parser);

  JsonValueNode *first = nullptr;
  JsonValueNode *last = nullptr;
  u64 count = 0;

  if (JsonConsume(parser, ']')) {
    JsonValue *value = JsonPushValue(parser->arena, JsonKind::Array);
    value->array.items = nullptr;
    value->array.count = 0;
    return value;
  }

  for (;;) {
    JsonValue *item = JsonParseValue(parser, depth + 1);
    if (item == nullptr) return nullptr;

    JsonValueNode *node = PushStruct(parser->arena, JsonValueNode);
    node->next = nullptr;
    node->value = item;
    if (last) {
      last->next = node;
      last = node;
    } else {
      first = last = node;
    }
    count += 1;

    JsonSkipWhitespace(parser);
    if (JsonConsume(parser, ']')) break;
    if (!JsonConsume(parser, ',')) {
      JsonSetError(parser, JsonError(Str8Lit("expected ',' or ']'")), parser->at);
      return nullptr;
    }
    JsonSkipWhitespace(parser);
  }

  JsonValue *value = JsonPushValue(parser->arena, JsonKind::Array);
  value->array.count = count;
  value->array.items = count ? PushArray(parser->arena, JsonValue *, count) : nullptr;
  u64 index = 0;
  for (JsonValueNode *node = first; node; node = node->next) {
    value->array.items[index++] = node->value;
  }
  return value;
}

JsonValue *JsonParseObject(JsonParser *parser, u64 depth) {
  if (depth >= kJsonMaxDepth) {
    JsonSetError(parser, JsonError(Str8Lit("maximum nesting depth exceeded")), parser->at);
    return nullptr;
  }

  JsonConsume(parser, '{');
  JsonSkipWhitespace(parser);

  JsonMember *first = nullptr;
  JsonMember *last = nullptr;

  if (JsonConsume(parser, '}')) {
    JsonValue *value = JsonPushValue(parser->arena, JsonKind::Object);
    value->object = nullptr;
    return value;
  }

  for (;;) {
    if (parser->at >= parser->text.size || parser->text.str[parser->at] != '"') {
      JsonSetError(parser, JsonError(Str8Lit("expected object key")), parser->at);
      return nullptr;
    }

    String8 key = JsonParseStringBytes(parser);
    if (parser->error.size != 0) return nullptr;

    JsonSkipWhitespace(parser);
    if (!JsonConsume(parser, ':')) {
      JsonSetError(parser, JsonError(Str8Lit("expected ':' after object key")), parser->at);
      return nullptr;
    }
    JsonSkipWhitespace(parser);

    JsonValue *member_value = JsonParseValue(parser, depth + 1);
    if (member_value == nullptr) return nullptr;

    JsonMember *member = PushStruct(parser->arena, JsonMember);
    member->name = key;
    member->value = member_value;
    member->next = nullptr;
    if (last) {
      last->next = member;
      last = member;
    } else {
      first = last = member;
    }

    JsonSkipWhitespace(parser);
    if (JsonConsume(parser, '}')) break;
    if (!JsonConsume(parser, ',')) {
      JsonSetError(parser, JsonError(Str8Lit("expected ',' or '}'")), parser->at);
      return nullptr;
    }
    JsonSkipWhitespace(parser);
  }

  JsonValue *value = JsonPushValue(parser->arena, JsonKind::Object);
  value->object = first;
  return value;
}

JsonValue *JsonParseValue(JsonParser *parser, u64 depth) {
  JsonSkipWhitespace(parser);
  if (parser->at >= parser->text.size) {
    JsonSetError(parser, JsonError(Str8Lit("expected value")), parser->at);
    return nullptr;
  }

  switch (parser->text.str[parser->at]) {
    case 'n': {
      if (!JsonConsumeKeyword(parser, Str8Lit("null"))) {
        JsonSetError(parser, JsonError(Str8Lit("invalid literal")), parser->at);
        return nullptr;
      }
      return JsonPushValue(parser->arena, JsonKind::Null);
    } break;
    case 't': {
      if (!JsonConsumeKeyword(parser, Str8Lit("true"))) {
        JsonSetError(parser, JsonError(Str8Lit("invalid literal")), parser->at);
        return nullptr;
      }
      JsonValue *value = JsonPushValue(parser->arena, JsonKind::Bool);
      value->boolean = true;
      return value;
    } break;
    case 'f': {
      if (!JsonConsumeKeyword(parser, Str8Lit("false"))) {
        JsonSetError(parser, JsonError(Str8Lit("invalid literal")), parser->at);
        return nullptr;
      }
      JsonValue *value = JsonPushValue(parser->arena, JsonKind::Bool);
      value->boolean = false;
      return value;
    } break;
    case '"': return JsonParseString(parser);
    case '[': return JsonParseArray(parser, depth);
    case '{': return JsonParseObject(parser, depth);
    default:
      if (parser->text.str[parser->at] == '-' || CharIsDigit(parser->text.str[parser->at])) {
        return JsonParseNumber(parser);
      }
      JsonSetError(parser, JsonError(Str8Lit("expected value")), parser->at);
      return nullptr;
  }
}

bool JsonWriterSetError(JsonWriter *writer, String8 error) {
  if (writer == nullptr) return false;
  if (writer->error.size == 0) writer->error = error;
  return false;
}

bool JsonWriterEnsure(JsonWriter *writer, u64 extra) {
  if (writer->error.size != 0) return false;
  if (extra > ULLONG_MAX - writer->size) {
    return JsonWriterSetError(writer, Str8Lit("json writer overflow"));
  }
  u64 needed = writer->size + extra;
  if (needed <= writer->capacity) return true;

  u64 capacity = writer->capacity ? writer->capacity : 256;
  while (capacity < needed) {
    if (capacity > ULLONG_MAX / 2) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }

  u8 *buffer = (u8 *)realloc(writer->buffer, (size_t)capacity);
  if (buffer == nullptr) return JsonWriterSetError(writer, Str8Lit("json writer out of memory"));
  writer->buffer = buffer;
  writer->capacity = capacity;
  return true;
}

bool JsonWriterAppendBytes(JsonWriter *writer, String8 bytes) {
  if (!JsonWriterEnsure(writer, bytes.size)) return false;
  memcpy(writer->buffer + writer->size, bytes.str, (size_t)bytes.size);
  writer->size += bytes.size;
  return true;
}

bool JsonWriterAppendByte(JsonWriter *writer, u8 byte) {
  if (!JsonWriterEnsure(writer, 1)) return false;
  writer->buffer[writer->size++] = byte;
  return true;
}

bool JsonWriterBeginValue(JsonWriter *writer) {
  if (writer->error.size != 0) return false;
  if (writer->scope_count == 0) {
    if (writer->root_written) return JsonWriterSetError(writer, Str8Lit("json writer already has a root value"));
    return true;
  }

  JsonWriterScope *scope = &writer->scopes[writer->scope_count - 1];
  if (scope->kind == JsonWriterScopeKind::Array) {
    if (scope->count > 0 && !JsonWriterAppendByte(writer, ',')) return false;
    return true;
  }

  if (scope->object_expects_key) return JsonWriterSetError(writer, Str8Lit("json writer expected an object key"));
  return true;
}

void JsonWriterFinishValue(JsonWriter *writer) {
  if (writer->scope_count == 0) {
    writer->root_written = true;
    return;
  }

  JsonWriterScope *scope = &writer->scopes[writer->scope_count - 1];
  scope->count += 1;
  if (scope->kind == JsonWriterScopeKind::Object) scope->object_expects_key = true;
}

bool JsonWriterPushScope(JsonWriter *writer, JsonWriterScopeKind kind) {
  if (writer->scope_count >= ArrayCount(writer->scopes)) {
    return JsonWriterSetError(writer, Str8Lit("json writer maximum nesting depth exceeded"));
  }
  writer->scopes[writer->scope_count++] = JsonWriterScope{kind, 0, kind == JsonWriterScopeKind::Object};
  return true;
}

bool JsonWriterWriteEscapedString(JsonWriter *writer, String8 value) {
  if (!JsonWriterAppendByte(writer, '"')) return false;
  for (u64 i = 0; i < value.size; i += 1) {
    u8 c = value.str[i];
    switch (c) {
      case '"':
        if (!JsonWriterAppendBytes(writer, Str8Lit("\\\""))) return false;
        break;
      case '\\':
        if (!JsonWriterAppendBytes(writer, Str8Lit("\\\\"))) return false;
        break;
      case '\b':
        if (!JsonWriterAppendBytes(writer, Str8Lit("\\b"))) return false;
        break;
      case '\f':
        if (!JsonWriterAppendBytes(writer, Str8Lit("\\f"))) return false;
        break;
      case '\n':
        if (!JsonWriterAppendBytes(writer, Str8Lit("\\n"))) return false;
        break;
      case '\r':
        if (!JsonWriterAppendBytes(writer, Str8Lit("\\r"))) return false;
        break;
      case '\t':
        if (!JsonWriterAppendBytes(writer, Str8Lit("\\t"))) return false;
        break;
      default:
        if (c < 0x20) {
          char escape[7];
          snprintf(escape, sizeof(escape), "\\u%04X", c);
          if (!JsonWriterAppendBytes(writer, Str8((u8 *)escape, 6))) return false;
        } else {
          if (!JsonWriterAppendByte(writer, c)) return false;
        }
        break;
    }
  }
  return JsonWriterAppendByte(writer, '"');
}

bool JsonWriterWriteFormatted(JsonWriter *writer, const char *fmt, auto value) {
  char buffer[64];
  int written = snprintf(buffer, sizeof(buffer), fmt, value);
  if (written < 0 || (u64)written >= sizeof(buffer)) {
    return JsonWriterSetError(writer, Str8Lit("json writer format failure"));
  }
  return JsonWriterAppendBytes(writer, Str8((u8 *)buffer, (u64)written));
}

}  // namespace

JsonParseResult JsonParse(Arena *arena, String8 text) {
  JsonParseResult result = {};
  if (text.size > kJsonMaxInputBytes) {
    result.error = Str8Lit("json input exceeds 16 MiB");
    result.error_offset = 0;
    return result;
  }

  JsonParser parser = {arena, text, 0, {}, 0};
  result.root = JsonParseValue(&parser, 0);
  if (result.root == nullptr) {
    result.error = parser.error;
    result.error_offset = parser.error_offset;
    return result;
  }

  JsonSkipWhitespace(&parser);
  if (parser.at != text.size) {
    result.root = nullptr;
    result.error = Str8Lit("trailing characters after JSON value");
    result.error_offset = parser.at;
    return result;
  }

  return result;
}

JsonValue *JsonObjectGet(const JsonValue *object, String8 name) {
  if (object == nullptr || object->kind != JsonKind::Object) return nullptr;
  JsonValue *found = nullptr;
  for (JsonMember *member = object->object; member; member = member->next) {
    if (Str8Match(member->name, name)) found = member->value;
  }
  return found;
}

JsonValue *JsonArrayItem(const JsonValue *value, u64 index) {
  if (value == nullptr || value->kind != JsonKind::Array) return nullptr;
  if (index >= value->array.count) return nullptr;
  return value->array.items[index];
}

u64 JsonArrayCount(const JsonValue *value) {
  if (value == nullptr || value->kind != JsonKind::Array) return 0;
  return value->array.count;
}

bool JsonGetString(const JsonValue *value, String8 *out) {
  if (out) *out = {};
  if (value == nullptr || value->kind != JsonKind::String) return false;
  if (out) *out = value->string;
  return true;
}

bool JsonGetBool(const JsonValue *value, bool *out) {
  if (out) *out = false;
  if (value == nullptr || value->kind != JsonKind::Bool) return false;
  if (out) *out = value->boolean;
  return true;
}

bool JsonGetF64(const JsonValue *value, f64 *out) {
  if (out) *out = 0;
  if (value == nullptr || value->kind != JsonKind::Number || !isfinite(value->number)) return false;
  if (out) *out = value->number;
  return true;
}

bool JsonGetI64(const JsonValue *value, i64 *out) {
  if (out) *out = 0;
  if (value == nullptr || value->kind != JsonKind::Number) return false;
  const JsonValueExt *meta = JsonValueMetadata(value);
  if (!meta->has_exact_i64) return false;
  if (out) *out = meta->exact_i64;
  return true;
}

bool JsonGetU64(const JsonValue *value, u64 *out) {
  if (out) *out = 0;
  if (value == nullptr || value->kind != JsonKind::Number) return false;
  const JsonValueExt *meta = JsonValueMetadata(value);
  if (!meta->has_exact_u64) return false;
  if (out) *out = meta->exact_u64;
  return true;
}

void JsonWriterInit(JsonWriter *writer, Arena *arena) {
  memset(writer, 0, sizeof(*writer));
  writer->arena = arena;
}

void JsonWriterDestroy(JsonWriter *writer) {
  free(writer->buffer);
  memset(writer, 0, sizeof(*writer));
}

String8 JsonWriterError(const JsonWriter *writer) { return writer ? writer->error : String8{}; }

bool JsonWriteObjectBegin(JsonWriter *writer) {
  if (!JsonWriterBeginValue(writer)) return false;
  if (!JsonWriterAppendByte(writer, '{')) return false;
  return JsonWriterPushScope(writer, JsonWriterScopeKind::Object);
}

bool JsonWriteObjectEnd(JsonWriter *writer) {
  if (writer == nullptr || writer->scope_count == 0 ||
      writer->scopes[writer->scope_count - 1].kind != JsonWriterScopeKind::Object) {
    return JsonWriterSetError(writer, Str8Lit("json writer object end without object scope"));
  }
  if (!writer->scopes[writer->scope_count - 1].object_expects_key) {
    return JsonWriterSetError(writer, Str8Lit("json writer object is missing a value"));
  }

  writer->scope_count -= 1;
  if (!JsonWriterAppendByte(writer, '}')) return false;
  JsonWriterFinishValue(writer);
  return true;
}

bool JsonWriteArrayBegin(JsonWriter *writer) {
  if (!JsonWriterBeginValue(writer)) return false;
  if (!JsonWriterAppendByte(writer, '[')) return false;
  return JsonWriterPushScope(writer, JsonWriterScopeKind::Array);
}

bool JsonWriteArrayEnd(JsonWriter *writer) {
  if (writer == nullptr || writer->scope_count == 0 ||
      writer->scopes[writer->scope_count - 1].kind != JsonWriterScopeKind::Array) {
    return JsonWriterSetError(writer, Str8Lit("json writer array end without array scope"));
  }

  writer->scope_count -= 1;
  if (!JsonWriterAppendByte(writer, ']')) return false;
  JsonWriterFinishValue(writer);
  return true;
}

bool JsonWriteObjectKey(JsonWriter *writer, String8 key) {
  if (writer == nullptr || writer->scope_count == 0 ||
      writer->scopes[writer->scope_count - 1].kind != JsonWriterScopeKind::Object) {
    return JsonWriterSetError(writer, Str8Lit("json writer object key outside object"));
  }

  JsonWriterScope *scope = &writer->scopes[writer->scope_count - 1];
  if (!scope->object_expects_key) {
    return JsonWriterSetError(writer, Str8Lit("json writer object key without a value for the previous key"));
  }

  if (scope->count > 0 && !JsonWriterAppendByte(writer, ',')) return false;
  if (!JsonWriterWriteEscapedString(writer, key)) return false;
  if (!JsonWriterAppendByte(writer, ':')) return false;
  scope->object_expects_key = false;
  return true;
}

bool JsonWriteNull(JsonWriter *writer) {
  if (!JsonWriterBeginValue(writer)) return false;
  if (!JsonWriterAppendBytes(writer, Str8Lit("null"))) return false;
  JsonWriterFinishValue(writer);
  return true;
}

bool JsonWriteBool(JsonWriter *writer, bool value) {
  if (!JsonWriterBeginValue(writer)) return false;
  if (!JsonWriterAppendBytes(writer, value ? Str8Lit("true") : Str8Lit("false"))) return false;
  JsonWriterFinishValue(writer);
  return true;
}

bool JsonWriteI64(JsonWriter *writer, i64 value) {
  if (!JsonWriterBeginValue(writer)) return false;
  if (!JsonWriterWriteFormatted(writer, "%lld", (long long)value)) return false;
  JsonWriterFinishValue(writer);
  return true;
}

bool JsonWriteU64(JsonWriter *writer, u64 value) {
  if (!JsonWriterBeginValue(writer)) return false;
  if (!JsonWriterWriteFormatted(writer, "%llu", (unsigned long long)value)) return false;
  JsonWriterFinishValue(writer);
  return true;
}

bool JsonWriteF64(JsonWriter *writer, f64 value) {
  if (!isfinite(value)) return JsonWriterSetError(writer, Str8Lit("json writer does not allow non-finite numbers"));
  if (!JsonWriterBeginValue(writer)) return false;
  if (!JsonWriterWriteFormatted(writer, "%.17g", value)) return false;
  JsonWriterFinishValue(writer);
  return true;
}

bool JsonWriteString(JsonWriter *writer, String8 value) {
  if (!JsonWriterBeginValue(writer)) return false;
  if (!JsonWriterWriteEscapedString(writer, value)) return false;
  JsonWriterFinishValue(writer);
  return true;
}

String8 JsonWriterFinish(JsonWriter *writer) {
  if (writer == nullptr) return {};
  if (writer->error.size != 0) {
    free(writer->buffer);
    writer->buffer = nullptr;
    writer->size = 0;
    writer->capacity = 0;
    return {};
  }
  if (writer->scope_count != 0) {
    writer->error = Str8Lit("json writer has an unfinished container");
    free(writer->buffer);
    writer->buffer = nullptr;
    writer->size = 0;
    writer->capacity = 0;
    return {};
  }
  if (!writer->root_written) {
    writer->error = Str8Lit("json writer has no root value");
    free(writer->buffer);
    writer->buffer = nullptr;
    writer->size = 0;
    writer->capacity = 0;
    return {};
  }

  String8 result = PushStr8Copy(writer->arena, Str8(writer->buffer, writer->size));
  JsonWriterDestroy(writer);
  return result;
}
