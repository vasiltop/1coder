#include "buffers/buf_image.h"

#include "editor/editor.h"
#include "editor/filetype.h"
#include "os/os_file.h"

// Header sniffing only. Every format below stores its dimensions in the first
// few bytes, so this reads a small prefix and stops -- no decoding, no pixel
// buffer, and nothing that would drag a decoder into core.

namespace {

// Enough for a JPEG's segment chain to reach the first frame header in
// practice. A file whose dimensions sit past this reports zero, which the
// summary shows honestly rather than guessing.
inline constexpr u64 kHeaderBytes = 4096;

[[nodiscard]] u32 ReadU16BE(String8 data, u64 offset) {
  if (offset + 2 > data.size) return 0;
  return ((u32)data.str[offset] << 8) | (u32)data.str[offset + 1];
}

[[nodiscard]] u32 ReadU16LE(String8 data, u64 offset) {
  if (offset + 2 > data.size) return 0;
  return (u32)data.str[offset] | ((u32)data.str[offset + 1] << 8);
}

[[nodiscard]] u32 ReadU32BE(String8 data, u64 offset) {
  if (offset + 4 > data.size) return 0;
  return ((u32)data.str[offset] << 24) | ((u32)data.str[offset + 1] << 16) |
         ((u32)data.str[offset + 2] << 8) | (u32)data.str[offset + 3];
}

[[nodiscard]] u32 ReadU32LE(String8 data, u64 offset) {
  if (offset + 4 > data.size) return 0;
  return (u32)data.str[offset] | ((u32)data.str[offset + 1] << 8) |
         ((u32)data.str[offset + 2] << 16) | ((u32)data.str[offset + 3] << 24);
}

[[nodiscard]] bool HasBytes(String8 data, u64 offset, const char *literal, u64 length) {
  if (offset + length > data.size) return false;
  for (u64 i = 0; i < length; i += 1) {
    if (data.str[offset + i] != (u8)literal[i]) return false;
  }
  return true;
}

// JPEG stores its dimensions in a frame header somewhere past a chain of
// variable-length segments, so unlike the others it has to be walked.
void SniffJpeg(String8 data, ImageInfo *info) {
  u64 at = 2;  // past the SOI marker
  while (at + 4 <= data.size) {
    if (data.str[at] != 0xFF) break;

    u8 marker = data.str[at + 1];
    // Padding fill bytes are legal between segments.
    if (marker == 0xFF) {
      at += 1;
      continue;
    }

    u64 length = ReadU16BE(data, at + 2);
    if (length < 2) break;

    // SOF0..SOF15 carry the frame size; the four DHT/JPG/DAC/RST markers
    // interleaved in that range do not.
    bool is_frame = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 &&
                    marker != 0xCC;
    if (is_frame) {
      info->height = ReadU16BE(data, at + 5);
      info->width = ReadU16BE(data, at + 7);
      return;
    }

    at += 2 + length;
  }
}

void SniffWebp(String8 data, ImageInfo *info) {
  if (HasBytes(data, 12, "VP8X", 4)) {
    // Canvas size is stored minus one, in three little-endian bytes each.
    if (data.size < 30) return;
    info->width = ((u32)data.str[24] | ((u32)data.str[25] << 8) | ((u32)data.str[26] << 16)) + 1;
    info->height = ((u32)data.str[27] | ((u32)data.str[28] << 8) | ((u32)data.str[29] << 16)) + 1;
  } else if (HasBytes(data, 12, "VP8L", 4)) {
    if (data.size < 25) return;
    u32 bits = ReadU32LE(data, 21);
    info->width = (bits & 0x3FFF) + 1;
    info->height = ((bits >> 14) & 0x3FFF) + 1;
  } else if (HasBytes(data, 12, "VP8 ", 4)) {
    if (data.size < 30) return;
    info->width = ReadU16LE(data, 26) & 0x3FFF;
    info->height = ReadU16LE(data, 28) & 0x3FFF;
  }
}

void SniffHeader(String8 data, ImageInfo *info) {
  if (HasBytes(data, 0, "\x89PNG\r\n\x1a\n", 8)) {
    info->format = Str8Lit("PNG");
    // IHDR is required to be the first chunk, so its payload is at a fixed
    // offset: 8 signature + 4 length + 4 type.
    info->width = ReadU32BE(data, 16);
    info->height = ReadU32BE(data, 20);
  } else if (HasBytes(data, 0, "GIF87a", 6) || HasBytes(data, 0, "GIF89a", 6)) {
    info->format = Str8Lit("GIF");
    info->width = ReadU16LE(data, 6);
    info->height = ReadU16LE(data, 8);
  } else if (HasBytes(data, 0, "BM", 2)) {
    info->format = Str8Lit("BMP");
    info->width = ReadU32LE(data, 18);
    // Height is signed: negative means the rows are stored top-down.
    i32 height = (i32)ReadU32LE(data, 22);
    info->height = (u32)((height < 0) ? -height : height);
  } else if (HasBytes(data, 0, "\xFF\xD8", 2)) {
    info->format = Str8Lit("JPEG");
    SniffJpeg(data, info);
  } else if (HasBytes(data, 0, "RIFF", 4) && HasBytes(data, 8, "WEBP", 4)) {
    info->format = Str8Lit("WebP");
    SniffWebp(data, info);
  }
}

// A readable size, so the summary says "243 KB" rather than a byte count.
[[nodiscard]] String8 HumanSize(Arena *arena, u64 bytes) {
  if (bytes < KB(1)) return PushStr8F(arena, "%llu B", (unsigned long long)bytes);
  if (bytes < MB(1)) return PushStr8F(arena, "%.1f KB", (double)bytes / 1024.0);
  return PushStr8F(arena, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

BufferHandle ImageOpenHandler(Editor *ed, String8 path) { return ImageBufferOpen(ed, path); }

}  // namespace

BufferHandle ImageBufferOpen(Editor *ed, String8 path) {
  TempArena scratch = ScratchBegin1(ed->arena);
  String8 absolute = OsPathAbsolute(scratch.arena, path);

  BufferHandle existing = BufferFromPath(&ed->buffers, absolute);
  if (existing.index != 0) {
    ScratchEnd(scratch);
    return existing;
  }

  BufferHandle handle = BufferOpen(&ed->buffers, BufferKind::Image, Str8PathBase(absolute));
  Buffer *buffer = BufferFromHandle(&ed->buffers, handle);
  if (!buffer) {
    ScratchEnd(scratch);
    return BufferHandleZero();
  }

  buffer->path = PushStr8Copy(buffer->arena, absolute);

  ImageInfo *info = PushStruct(buffer->arena, ImageInfo);
  info->path = buffer->path;

  // Mapping rather than reading: only the pages actually touched fault in, so
  // sniffing a 40MB photograph costs a few kilobytes.
  FileMapping mapping = OsFileMap(absolute);
  if (mapping.ok) {
    info->byte_size = mapping.size;
    SniffHeader(String8{mapping.data, Min(mapping.size, kHeaderBytes)}, info);
    // The mapping cannot outlive this function: nothing below points into it,
    // since format is a literal and the dimensions are copied out.
    OsFileUnmap(&mapping);
  }

  info->format = PushStr8Copy(buffer->arena, info->format);
  buffer->user_data = info;

  String8List lines = {};
  Str8ListPush(scratch.arena, &lines, Str8PathBase(absolute));
  Str8ListPush(scratch.arena, &lines, String8{nullptr, 0});

  if (info->width != 0 && info->height != 0) {
    Str8ListPush(scratch.arena, &lines,
                 PushStr8F(scratch.arena, "%.*s  %ux%u", (int)info->format.size,
                           (char *)info->format.str, info->width, info->height));
  } else if (info->format.size != 0) {
    // A recognised format whose header could not be read is worth saying out
    // loud; silently showing nothing would look like a bug in the viewer.
    Str8ListPush(scratch.arena, &lines,
                 PushStr8F(scratch.arena, "%.*s  (dimensions unavailable)",
                           (int)info->format.size, (char *)info->format.str));
  } else {
    Str8ListPush(scratch.arena, &lines, Str8Lit("unrecognised image format"));
  }

  Str8ListPush(scratch.arena, &lines, HumanSize(scratch.arena, info->byte_size));
  Str8ListPush(scratch.arena, &lines, String8{nullptr, 0});
  Str8ListPush(scratch.arena, &lines, absolute);

  BufferSetText(ed, buffer, Str8ListJoin(scratch.arena, &lines, Str8Lit("\n")));

  // Read-only and with no on_write: `:w` refuses rather than writing the
  // summary text over the image.
  buffer->flags |= BufferFlags::ReadOnly;

  ScratchEnd(scratch);
  return handle;
}

const ImageInfo *ImageBufferInfo(const Buffer *buffer) {
  if (!buffer || buffer->kind != BufferKind::Image) return nullptr;
  return (const ImageInfo *)buffer->user_data;
}

void ImageRegisterFiletypes(Editor *ed) {
  static const char *kExtensions[] = {"png", "jpg", "jpeg", "gif", "bmp", "webp"};
  for (u64 i = 0; i < ArrayCount(kExtensions); i += 1) {
    FiletypeRegister(ed, Str8C(kExtensions[i]), ImageOpenHandler);
  }
}
