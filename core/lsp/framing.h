#pragma once

#include "base/base_arena.h"
#include "base/base_string.h"

struct LspFrameDecoder {
  void *impl;
};

void LspFrameDecoderInit(LspFrameDecoder *decoder);
void LspFrameDecoderDestroy(LspFrameDecoder *decoder);
[[nodiscard]] bool LspFrameDecoderFeed(LspFrameDecoder *decoder, String8 bytes);
[[nodiscard]] bool LspFrameDecoderFinish(LspFrameDecoder *decoder);
[[nodiscard]] String8 LspFrameDecoderError(const LspFrameDecoder *decoder);
[[nodiscard]] u64 LspFrameDecoderQueuedCount(const LspFrameDecoder *decoder);
[[nodiscard]] String8 LspFrameDecoderPop(LspFrameDecoder *decoder, Arena *arena);
[[nodiscard]] String8 LspFrameEncode(Arena *arena, String8 json);
