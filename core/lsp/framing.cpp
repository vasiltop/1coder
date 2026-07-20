#include "lsp/framing.h"

#include "base/base_string.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

namespace {

inline constexpr u64 kLspHeaderCap = KB(64);
inline constexpr u64 kLspBodyCap = MB(16);

struct LspFrameNode {
  LspFrameNode *next;
  u8 *data;
  u64 size;
};

struct LspFrameDecoderImpl {
  u8 *buffer;
  u64 size;
  u64 capacity;
  u64 expected_body_size;
  bool have_header;
  String8 error;
  LspFrameNode *first;
  LspFrameNode *last;
  u64 queued_count;
};

bool LspSetError(LspFrameDecoderImpl *impl, String8 error) {
  if (impl->error.size == 0) impl->error = error;
  return false;
}

bool LspEnsureCapacity(LspFrameDecoderImpl *impl, u64 extra) {
  if (impl->error.size != 0) return false;
  if (extra > ULLONG_MAX - impl->size) return LspSetError(impl, Str8Lit("frame buffer overflow"));
  u64 needed = impl->size + extra;
  if (needed <= impl->capacity) return true;

  u64 capacity = impl->capacity ? impl->capacity : 256;
  while (capacity < needed) {
    if (capacity > ULLONG_MAX / 2) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }

  u8 *buffer = (u8 *)realloc(impl->buffer, (size_t)capacity);
  if (buffer == nullptr) return LspSetError(impl, Str8Lit("frame buffer out of memory"));
  impl->buffer = buffer;
  impl->capacity = capacity;
  return true;
}

void LspConsumePrefix(LspFrameDecoderImpl *impl, u64 amount) {
  if (amount == 0) return;
  amount = Min(amount, impl->size);
  memmove(impl->buffer, impl->buffer + amount, (size_t)(impl->size - amount));
  impl->size -= amount;
}

u64 LspFindHeaderEnd(String8 buffer) {
  for (u64 i = 0; i + 3 < buffer.size; i += 1) {
    if (buffer.str[i] == '\r' && buffer.str[i + 1] == '\n' &&
        buffer.str[i + 2] == '\r' && buffer.str[i + 3] == '\n') {
      return i;
    }
  }
  return buffer.size;
}

bool LspHasMalformedLf(String8 buffer) {
  for (u64 i = 0; i < buffer.size; i += 1) {
    if (buffer.str[i] == '\n' && (i == 0 || buffer.str[i - 1] != '\r')) return true;
    if (buffer.str[i] == '\r' && i + 1 < buffer.size && buffer.str[i + 1] != '\n') return true;
  }
  return false;
}

bool LspParseContentLength(String8 value, u64 *out) {
  value = Str8SkipChopWhitespace(value);
  if (value.size == 0) return false;

  u64 result = 0;
  for (u64 i = 0; i < value.size; i += 1) {
    if (!CharIsDigit(value.str[i])) return false;
    u64 digit = (u64)(value.str[i] - '0');
    if (result > (ULLONG_MAX - digit) / 10) return false;
    result = result * 10 + digit;
  }

  *out = result;
  return true;
}

bool LspQueueBody(LspFrameDecoderImpl *impl, String8 body) {
  LspFrameNode *node = (LspFrameNode *)calloc(1, sizeof(LspFrameNode));
  if (node == nullptr) return LspSetError(impl, Str8Lit("frame queue out of memory"));
  node->data = (u8 *)malloc((size_t)body.size);
  if (body.size != 0 && node->data == nullptr) {
    free(node);
    return LspSetError(impl, Str8Lit("frame queue out of memory"));
  }
  if (body.size != 0) memcpy(node->data, body.str, (size_t)body.size);
  node->size = body.size;

  if (impl->last) {
    impl->last->next = node;
    impl->last = node;
  } else {
    impl->first = impl->last = node;
  }
  impl->queued_count += 1;
  return true;
}

bool LspParseHeader(LspFrameDecoderImpl *impl, String8 header) {
  bool saw_content_length = false;
  u64 content_length = 0;
  u64 line_start = 0;

  while (line_start < header.size) {
    u64 line_end = line_start;
    while (line_end + 1 < header.size &&
           !(header.str[line_end] == '\r' && header.str[line_end + 1] == '\n')) {
      line_end += 1;
    }

    bool has_line_break = (line_end + 1 < header.size);
    String8 line = Str8(header.str + line_start,
                        has_line_break ? (line_end - line_start) : (header.size - line_start));
    u64 colon = Str8FindFirstChar(line, ':');
    if (colon == line.size) return LspSetError(impl, Str8Lit("malformed header line"));

    String8 name = Str8Chop(line, line.size - colon);
    String8 value = Str8Skip(line, colon + 1);
    if (Str8Match(Str8SkipChopWhitespace(name), Str8Lit("Content-Length"),
                  StringMatch::CaseInsensitive)) {
      if (saw_content_length) return LspSetError(impl, Str8Lit("duplicate Content-Length header"));
      if (!LspParseContentLength(value, &content_length)) {
        return LspSetError(impl, Str8Lit("invalid Content-Length header"));
      }
      if (content_length > kLspBodyCap) return LspSetError(impl, Str8Lit("body too large"));
      saw_content_length = true;
    }

    if (!has_line_break) break;
    line_start = line_end + 2;
  }

  if (!saw_content_length) return LspSetError(impl, Str8Lit("missing Content-Length header"));
  impl->expected_body_size = content_length;
  impl->have_header = true;
  return true;
}

bool LspProcess(LspFrameDecoderImpl *impl) {
  while (impl->error.size == 0) {
    if (!impl->have_header) {
      String8 buffer = Str8(impl->buffer, impl->size);
      u64 header_end = LspFindHeaderEnd(buffer);
      if (header_end == buffer.size) {
        if (impl->size > kLspHeaderCap) return LspSetError(impl, Str8Lit("header too large"));
        if (LspHasMalformedLf(buffer)) return LspSetError(impl, Str8Lit("malformed header delimiter"));
        return true;
      }
      if (header_end + 4 > kLspHeaderCap) return LspSetError(impl, Str8Lit("header too large"));
      if (LspHasMalformedLf(Str8(impl->buffer, header_end + 4))) {
        return LspSetError(impl, Str8Lit("malformed header delimiter"));
      }
      if (!LspParseHeader(impl, Str8(impl->buffer, header_end))) return false;
      LspConsumePrefix(impl, header_end + 4);
    }

    if (impl->size < impl->expected_body_size) return true;
    if (!LspQueueBody(impl, Str8(impl->buffer, impl->expected_body_size))) return false;
    LspConsumePrefix(impl, impl->expected_body_size);
    impl->expected_body_size = 0;
    impl->have_header = false;
  }

  return false;
}

LspFrameDecoderImpl *LspImpl(const LspFrameDecoder *decoder) {
  return decoder ? (LspFrameDecoderImpl *)decoder->impl : nullptr;
}

}  // namespace

void LspFrameDecoderInit(LspFrameDecoder *decoder) {
  if (decoder == nullptr) return;
  decoder->impl = calloc(1, sizeof(LspFrameDecoderImpl));
}

void LspFrameDecoderDestroy(LspFrameDecoder *decoder) {
  LspFrameDecoderImpl *impl = LspImpl(decoder);
  if (impl == nullptr) return;

  free(impl->buffer);
  for (LspFrameNode *node = impl->first; node;) {
    LspFrameNode *next = node->next;
    free(node->data);
    free(node);
    node = next;
  }
  free(impl);
  decoder->impl = nullptr;
}

bool LspFrameDecoderFeed(LspFrameDecoder *decoder, String8 bytes) {
  LspFrameDecoderImpl *impl = LspImpl(decoder);
  if (impl == nullptr) return false;
  if (impl->error.size != 0) return false;
  if (!LspEnsureCapacity(impl, bytes.size)) return false;
  if (bytes.size != 0) memcpy(impl->buffer + impl->size, bytes.str, (size_t)bytes.size);
  impl->size += bytes.size;
  return LspProcess(impl);
}

bool LspFrameDecoderFinish(LspFrameDecoder *decoder) {
  LspFrameDecoderImpl *impl = LspImpl(decoder);
  if (impl == nullptr) return false;
  if (impl->error.size != 0) return false;
  if (impl->have_header || impl->size != 0) return LspSetError(impl, Str8Lit("partial frame at eof"));
  return true;
}

String8 LspFrameDecoderError(const LspFrameDecoder *decoder) {
  LspFrameDecoderImpl *impl = LspImpl(decoder);
  return impl ? impl->error : String8{};
}

u64 LspFrameDecoderQueuedCount(const LspFrameDecoder *decoder) {
  LspFrameDecoderImpl *impl = LspImpl(decoder);
  return impl ? impl->queued_count : 0;
}

String8 LspFrameDecoderPop(LspFrameDecoder *decoder, Arena *arena) {
  LspFrameDecoderImpl *impl = LspImpl(decoder);
  if (impl == nullptr || impl->first == nullptr) return {};

  LspFrameNode *node = impl->first;
  impl->first = node->next;
  if (impl->first == nullptr) impl->last = nullptr;
  impl->queued_count -= 1;

  String8 result = PushStr8Copy(arena, Str8(node->data, node->size));
  free(node->data);
  free(node);
  return result;
}

String8 LspFrameEncode(Arena *arena, String8 json) {
  String8 header = PushStr8F(arena, "Content-Length: %llu\r\n\r\n",
                             (unsigned long long)json.size);
  u8 *bytes = PushArrayNoZero(arena, u8, header.size + json.size + 1);
  memcpy(bytes, header.str, (size_t)header.size);
  if (json.size != 0) memcpy(bytes + header.size, json.str, (size_t)json.size);
  bytes[header.size + json.size] = 0;
  return Str8(bytes, header.size + json.size);
}
