#include "base/base_arena.h"
#include "base/base_string.h"
#include "lsp/framing.h"
#include "lsp/json.h"
#include "test.h"

#include <math.h>
#include <string.h>

namespace {

struct ArenaScope {
  Arena *arena;

  explicit ArenaScope(u64 reserve_size = MB(32)) : arena(ArenaAlloc(reserve_size)) {}
  ~ArenaScope() { ArenaRelease(arena); }
};

JsonValue *ParseJsonOrFail(Arena *arena, String8 text) {
  JsonParseResult result = JsonParse(arena, text);
  if (result.root == nullptr) {
    TestFail(__FILE__, __LINE__, "parse failed at %llu: %.*s",
             (unsigned long long)result.error_offset, (int)result.error.size,
             (char *)result.error.str);
  }
  return result.root;
}

void CheckParseError(String8 text, u64 min_offset, u64 max_offset) {
  ArenaScope scope;
  JsonParseResult result = JsonParse(scope.arena, text);
  CHECK(result.root == nullptr);
  CHECK(result.error.size > 0);
  CHECK(result.error_offset >= min_offset);
  CHECK(result.error_offset <= max_offset);
}

void CheckJsonI64(const JsonValue *value, i64 expected) {
  i64 actual = 0;
  CHECK(JsonGetI64(value, &actual));
  CHECK_EQ(actual, expected);
}

void CheckJsonU64(const JsonValue *value, u64 expected) {
  u64 actual = 0;
  CHECK(JsonGetU64(value, &actual));
  CHECK_EQ(actual, expected);
}

void CheckJsonF64(const JsonValue *value, f64 expected) {
  f64 actual = 0;
  CHECK(JsonGetF64(value, &actual));
  CHECK(actual == expected);
}

void CheckJsonBool(const JsonValue *value, bool expected) {
  bool actual = !expected;
  CHECK(JsonGetBool(value, &actual));
  CHECK(actual == expected);
}

void CheckJsonString(const JsonValue *value, String8 expected) {
  String8 actual = {};
  CHECK(JsonGetString(value, &actual));
  CHECK_STR(actual, expected);
}

String8 WriterFinishOrFail(JsonWriter *writer) {
  String8 result = JsonWriterFinish(writer);
  if (result.size == 0 && JsonWriterError(writer).size != 0) {
    String8 error = JsonWriterError(writer);
    TestFail(__FILE__, __LINE__, "writer failed: %.*s", (int)error.size, (char *)error.str);
  }
  return result;
}

void CheckDecoderError(String8 frame, String8 expected_prefix, bool finish = false) {
  LspFrameDecoder decoder = {};
  LspFrameDecoderInit(&decoder);
  bool ok = LspFrameDecoderFeed(&decoder, frame);
  if (ok && finish) ok = LspFrameDecoderFinish(&decoder);
  CHECK(!ok);
  CHECK(Str8StartsWith(LspFrameDecoderError(&decoder), expected_prefix));
  LspFrameDecoderDestroy(&decoder);
}

}  // namespace

TEST(lsp_json_parse_nested_values_and_accessors) {
  ArenaScope scope;
  String8 text =
      Str8Lit("{\"ok\":true,\"missing\":null,\"list\":[1,-2.5e1,{\"name\":\"x\"}],"
              "\"dup\":1,\"dup\":2,\"big\":9007199254740992,\"too_big\":9007199254740993}");
  JsonValue *root = ParseJsonOrFail(scope.arena, text);

  CHECK(root->kind == JsonKind::Object);

  JsonValue *ok = JsonObjectGet(root, Str8Lit("ok"));
  CHECK(ok != nullptr);
  CheckJsonBool(ok, true);
  CHECK(JsonObjectGet(root, Str8Lit("missing"))->kind == JsonKind::Null);

  JsonValue *list = JsonObjectGet(root, Str8Lit("list"));
  CHECK_EQ(JsonArrayCount(list), (u64)3);
  CheckJsonI64(JsonArrayItem(list, 0), 1);
  CheckJsonF64(JsonArrayItem(list, 1), -25.0);
  JsonValue *inner = JsonArrayItem(list, 2);
  CheckJsonString(JsonObjectGet(inner, Str8Lit("name")), Str8Lit("x"));

  CheckJsonI64(JsonObjectGet(root, Str8Lit("dup")), 2);
  CheckJsonU64(JsonObjectGet(root, Str8Lit("big")), 9007199254740992ull);
  u64 too_big = 0;
  CHECK(!JsonGetU64(JsonObjectGet(root, Str8Lit("too_big")), &too_big));
  CHECK(JsonArrayItem(list, 99) == nullptr);
}

TEST(lsp_json_parse_strings_unicode_and_null_termination) {
  ArenaScope scope;
  String8 text = Str8Lit(
      "[\"quote:\\\" slash:\\/ backslash:\\\\ controls:\\b\\f\\n\\r\\t\","
      "\"\\u263A\",\"\\uD83D\\uDE00\",\"plain\"]");
  JsonValue *root = ParseJsonOrFail(scope.arena, text);
  CHECK_EQ(JsonArrayCount(root), (u64)4);

  CheckJsonString(JsonArrayItem(root, 0),
                  Str8Lit("quote:\" slash:/ backslash:\\ controls:\b\f\n\r\t"));
  CheckJsonString(JsonArrayItem(root, 1), Str8Lit("\xE2\x98\xBA"));
  CheckJsonString(JsonArrayItem(root, 2), Str8Lit("\xF0\x9F\x98\x80"));

  String8 plain = {};
  CHECK(JsonGetString(JsonArrayItem(root, 3), &plain));
  CHECK(plain.str != text.str);
  CHECK_EQ(plain.str[plain.size], 0);
}

TEST(lsp_json_parse_rejects_malformed_inputs) {
  CheckParseError(Str8Lit("{\"a\":\"\\x\"}"), 6, 8);
  CheckParseError(Str8Lit("{\"a\":\"\\u12\"}"), 6, 11);
  CheckParseError(Str8Lit("{\"a\":\"\\uD800\"}"), 6, 13);
  CheckParseError(Str8Lit("{\"a\":\"\\uD83D\\u0041\"}"), 6, 19);
  CheckParseError(Str8Lit("01"), 0, 1);
  CheckParseError(Str8Lit("-01"), 1, 2);
  CheckParseError(Str8Lit("\"unterminated"), 0, 12);
  CheckParseError(Str8Lit("tru"), 0, 3);
  CheckParseError(Str8Lit("[1,"), 2, 3);
  CheckParseError(Str8Lit("{\"a\":"), 5, 6);
  CheckParseError(Str8Lit("true false"), 4, 9);
  CheckParseError(Str8Lit("1e309"), 0, 5);
}

TEST(lsp_json_parse_rejects_depth_and_size_limits) {
  ArenaScope scope;

  u8 *deep_bytes = PushArray(scope.arena, u8, 260);
  u64 deep_size = 0;
  for (u64 i = 0; i < 129; i += 1) deep_bytes[deep_size++] = '[';
  for (u64 i = 0; i < 129; i += 1) deep_bytes[deep_size++] = ']';
  CheckParseError(Str8(deep_bytes, deep_size), 128, deep_size);

  u64 huge_size = MB(16) + 1;
  u8 *huge = PushArrayNoZero(scope.arena, u8, huge_size);
  memset(huge, ' ', huge_size);
  huge[huge_size - 2] = '[';
  huge[huge_size - 1] = ']';
  CheckParseError(Str8(huge, huge_size), 0, huge_size);
}

TEST(lsp_json_writer_emits_valid_nested_json) {
  ArenaScope scope;
  JsonWriter writer = {};
  JsonWriterInit(&writer, scope.arena);

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("name")));
  CHECK(JsonWriteString(&writer, Str8Lit("A\"B\\C")));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("count")));
  CHECK(JsonWriteU64(&writer, 42));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("ok")));
  CHECK(JsonWriteBool(&writer, true));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("values")));
  CHECK(JsonWriteArrayBegin(&writer));
  CHECK(JsonWriteNull(&writer));
  CHECK(JsonWriteI64(&writer, -7));
  CHECK(JsonWriteF64(&writer, 1.5));
  CHECK(JsonWriteString(&writer, Str8Lit("\xC3\xA9\xF0\x9F\x98\x80")));
  CHECK(JsonWriteArrayEnd(&writer));
  CHECK(JsonWriteObjectEnd(&writer));

  String8 json = WriterFinishOrFail(&writer);
  CHECK_STR(json,
            Str8Lit("{\"name\":\"A\\\"B\\\\C\",\"count\":42,\"ok\":true,\"values\":[null,-7,1.5,"
                    "\"\xC3\xA9\xF0\x9F\x98\x80\"]}"));
  CHECK_EQ(json.str[json.size], 0);
  JsonWriterDestroy(&writer);
}

TEST(lsp_json_writer_rejects_nonfinite_numbers_and_bad_structure) {
  ArenaScope scope;
  JsonWriter writer = {};
  JsonWriterInit(&writer, scope.arena);

  CHECK(JsonWriteObjectBegin(&writer));
  CHECK(JsonWriteObjectKey(&writer, Str8Lit("bad")));
  CHECK(!JsonWriteF64(&writer, INFINITY));
  CHECK(JsonWriterError(&writer).size > 0);
  JsonWriterDestroy(&writer);

  JsonWriter other = {};
  JsonWriterInit(&other, scope.arena);
  CHECK(JsonWriteObjectBegin(&other));
  CHECK(JsonWriteObjectKey(&other, Str8Lit("x")));
  CHECK(!JsonWriteObjectEnd(&other));
  CHECK(JsonWriterError(&other).size > 0);
  JsonWriterDestroy(&other);
}

TEST(lsp_json_frame_encode_and_decode_single_message) {
  ArenaScope scope;
  String8 body = Str8Lit("{\"jsonrpc\":\"2.0\"}");
  String8 frame = LspFrameEncode(scope.arena, body);
  CHECK_STR(frame, Str8Lit("Content-Length: 17\r\n\r\n{\"jsonrpc\":\"2.0\"}"));

  LspFrameDecoder decoder = {};
  LspFrameDecoderInit(&decoder);
  CHECK(LspFrameDecoderFeed(&decoder, frame));
  CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)1);
  String8 popped = LspFrameDecoderPop(&decoder, scope.arena);
  CHECK_STR(popped, body);
  CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)0);
  CHECK(LspFrameDecoderFinish(&decoder));
  LspFrameDecoderDestroy(&decoder);
}

TEST(lsp_json_frame_decoder_handles_header_and_body_fragmentation) {
  ArenaScope scope;
  String8 body = Str8Lit("{\"method\":\"x\"}");
  String8 frame = LspFrameEncode(scope.arena, body);
  u64 header_size = Str8FindFirst(frame, Str8Lit("\r\n\r\n")) + 4;

  for (u64 split = 0; split <= header_size; split += 1) {
    LspFrameDecoder decoder = {};
    LspFrameDecoderInit(&decoder);
    CHECK(LspFrameDecoderFeed(&decoder, Str8Prefix(frame, split)));
    CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)0);
    CHECK(LspFrameDecoderFeed(&decoder, Str8Skip(frame, split)));
    CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)1);
    String8 popped = LspFrameDecoderPop(&decoder, scope.arena);
    CHECK_STR(popped, body);
    CHECK(LspFrameDecoderFinish(&decoder));
    LspFrameDecoderDestroy(&decoder);
  }

  LspFrameDecoder decoder = {};
  LspFrameDecoderInit(&decoder);
  CHECK(LspFrameDecoderFeed(&decoder, Str8Prefix(frame, header_size)));
  for (u64 i = header_size; i < frame.size; i += 1) {
    CHECK(LspFrameDecoderFeed(&decoder, Str8(frame.str + i, 1)));
    if (i + 1 < frame.size) CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)0);
  }
  CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)1);
  CHECK_STR(LspFrameDecoderPop(&decoder, scope.arena), body);
  CHECK(LspFrameDecoderFinish(&decoder));
  LspFrameDecoderDestroy(&decoder);
}

TEST(lsp_json_frame_decoder_handles_multiple_frames_and_partial_next_frame) {
  ArenaScope scope;
  String8 one = LspFrameEncode(scope.arena, Str8Lit("{}"));
  String8 two = LspFrameEncode(scope.arena, Str8Lit("[]"));
  String8 both = PushStr8Cat(scope.arena, one, two);

  LspFrameDecoder decoder = {};
  LspFrameDecoderInit(&decoder);
  CHECK(LspFrameDecoderFeed(&decoder, both));
  CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)2);
  CHECK_STR(LspFrameDecoderPop(&decoder, scope.arena), Str8Lit("{}"));
  CHECK_STR(LspFrameDecoderPop(&decoder, scope.arena), Str8Lit("[]"));
  CHECK(LspFrameDecoderFinish(&decoder));
  LspFrameDecoderDestroy(&decoder);

  String8 partial = Str8Prefix(two, 10);
  decoder = {};
  LspFrameDecoderInit(&decoder);
  CHECK(LspFrameDecoderFeed(&decoder, PushStr8Cat(scope.arena, one, partial)));
  CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)1);
  CHECK_STR(LspFrameDecoderPop(&decoder, scope.arena), Str8Lit("{}"));
  CHECK(LspFrameDecoderFeed(&decoder, Str8Skip(two, 10)));
  CHECK_STR(LspFrameDecoderPop(&decoder, scope.arena), Str8Lit("[]"));
  CHECK(LspFrameDecoderFinish(&decoder));
  LspFrameDecoderDestroy(&decoder);
}

TEST(lsp_json_frame_decoder_accepts_extra_headers_and_binary_bodies) {
  ArenaScope scope;
  u8 raw[] = {'C','o','N','t','E','n','T','-','L','e','N','g','t','H',':',' ','7','\r','\n',
              'X','-','T','e','s','t',':',' ','1','\r','\n','\r','\n',
              'a','b','c',0,'d','e','f'};
  String8 frame = Str8(raw, ArrayCount(raw));

  LspFrameDecoder decoder = {};
  LspFrameDecoderInit(&decoder);
  CHECK(LspFrameDecoderFeed(&decoder, frame));
  CHECK_EQ(LspFrameDecoderQueuedCount(&decoder), (u64)1);
  String8 body = LspFrameDecoderPop(&decoder, scope.arena);
  CHECK_EQ(body.size, (u64)7);
  CHECK_EQ(memcmp(body.str, raw + ArrayCount(raw) - 7, 7), 0);
  CHECK(LspFrameDecoderFinish(&decoder));
  LspFrameDecoderDestroy(&decoder);
}

TEST(lsp_json_frame_decoder_rejects_bad_headers_and_partial_finish) {
  CheckDecoderError(Str8Lit("X-Test: 1\r\n\r\n{}"), Str8Lit("missing Content-Length"));
  CheckDecoderError(Str8Lit("Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}"),
                    Str8Lit("duplicate Content-Length"));
  CheckDecoderError(Str8Lit("Content-Length: 2\r\nContent-Length: 3\r\n\r\n{}"),
                    Str8Lit("duplicate Content-Length"));
  CheckDecoderError(Str8Lit("Content-Length: x\r\n\r\n{}"), Str8Lit("invalid Content-Length"));
  CheckDecoderError(Str8Lit("Content-Length: 999999999999999999999\r\n\r\n{}"),
                    Str8Lit("invalid Content-Length"));
  CheckDecoderError(Str8Lit("Content-Length: 2\n\n{}"), Str8Lit("malformed header delimiter"));

  ArenaScope scope;
  u64 huge_header_size = KB(64) + 32;
  u8 *huge_header = PushArrayNoZero(scope.arena, u8, huge_header_size);
  memset(huge_header, 'a', huge_header_size);
  memcpy(huge_header, "X:", 2);
  huge_header[huge_header_size - 4] = '\r';
  huge_header[huge_header_size - 3] = '\n';
  huge_header[huge_header_size - 2] = '\r';
  huge_header[huge_header_size - 1] = '\n';
  CheckDecoderError(Str8(huge_header, huge_header_size), Str8Lit("header too large"));

  CheckDecoderError(Str8Lit("Content-Length: 16777217\r\n\r\n"), Str8Lit("body too large"));

  LspFrameDecoder decoder = {};
  LspFrameDecoderInit(&decoder);
  CHECK(LspFrameDecoderFeed(&decoder, Str8Lit("Content-Length: 2\r\n")));
  CHECK(!LspFrameDecoderFinish(&decoder));
  CHECK(Str8StartsWith(LspFrameDecoderError(&decoder), Str8Lit("partial frame at eof")));
  LspFrameDecoderDestroy(&decoder);

  decoder = {};
  LspFrameDecoderInit(&decoder);
  CHECK(LspFrameDecoderFeed(&decoder, Str8Lit("Content-Length: 2\r\n\r\n{")));
  CHECK(!LspFrameDecoderFinish(&decoder));
  CHECK(Str8StartsWith(LspFrameDecoderError(&decoder), Str8Lit("partial frame at eof")));
  LspFrameDecoderDestroy(&decoder);
}
