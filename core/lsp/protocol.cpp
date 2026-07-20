#include "lsp/protocol.h"

#include "editor/buffer.h"
#include "lsp/json.h"

#include <string.h>

namespace {

[[nodiscard]] bool Fail(Arena *arena, String8 *error, const char *message) {
  if (error) *error = PushStr8F(arena, "%s", message);
  return false;
}

[[nodiscard]] bool FailExpected(Arena *arena, String8 *error, const char *name, const char *kind) {
  if (error) *error = PushStr8F(arena, "%s must be %s", name, kind);
  return false;
}

[[nodiscard]] bool CharIsHex(u8 c) {
  return CharIsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] u8 HexValue(u8 c) {
  if (CharIsDigit(c)) return (u8)(c - '0');
  if (c >= 'a' && c <= 'f') return (u8)(10 + c - 'a');
  if (c >= 'A' && c <= 'F') return (u8)(10 + c - 'A');
  return 0xFF;
}

[[nodiscard]] bool IsUnreserved(u8 c) {
  return CharIsAlnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

[[nodiscard]] bool IsWindowsDrivePath(String8 path) {
  return path.size >= 3 && CharIsAlpha(path.str[0]) && path.str[1] == ':' &&
         (path.str[2] == '/' || path.str[2] == '\\');
}

[[nodiscard]] bool IsUncPath(String8 path) {
  return path.size >= 2 &&
         ((path.str[0] == '/' && path.str[1] == '/') ||
          (path.str[0] == '\\' && path.str[1] == '\\'));
}

[[nodiscard]] bool HasEmbeddedNul(String8 s) {
  for (u64 i = 0; i < s.size; i += 1) {
    if (s.str[i] == 0) return true;
  }
  return false;
}

[[nodiscard]] u64 CountObjectMembers(const JsonValue *value) {
  if (value == nullptr || value->kind != JsonKind::Object) return 0;
  u64 count = 0;
  for (JsonMember *member = value->object; member; member = member->next) count += 1;
  return count;
}

void AppendByte(u8 *dst, u64 *size, u8 value) { dst[(*size)++] = value; }

void AppendEncoded(u8 *dst, u64 *size, u8 value) {
  static constexpr u8 kHex[] = "0123456789ABCDEF";
  AppendByte(dst, size, '%');
  AppendByte(dst, size, kHex[value >> 4]);
  AppendByte(dst, size, kHex[value & 0x0F]);
}

[[nodiscard]] String8 CopyDecodedUriBytes(Arena *arena, String8 source, bool preserve_slash,
                                          bool *ok) {
  u8 *dst = PushArrayNoZero(arena, u8, source.size + 1);
  u64 size = 0;
  for (u64 i = 0; i < source.size; i += 1) {
    u8 c = source.str[i];
    if (c == 0) {
      *ok = false;
      return {};
    }
    if (c == '%') {
      if (i + 2 >= source.size || !CharIsHex(source.str[i + 1]) || !CharIsHex(source.str[i + 2])) {
        *ok = false;
        return {};
      }
      c = (u8)((HexValue(source.str[i + 1]) << 4) | HexValue(source.str[i + 2]));
      if (c == 0) {
        *ok = false;
        return {};
      }
      i += 2;
    }
    if (!preserve_slash && c == '/') {
      *ok = false;
      return {};
    }
    dst[size++] = c;
  }
  dst[size] = 0;
  *ok = true;
  return String8{dst, size};
}

[[nodiscard]] bool DecodeRequiredString(Arena *arena, const JsonValue *object, String8 key,
                                        String8 *out, String8 *error) {
  JsonValue *value = JsonObjectGet(object, key);
  if (value == nullptr) {
    if (error) *error = PushStr8F(arena, "missing %.*s", (int)key.size, (char *)key.str);
    return false;
  }
  String8 string = {};
  if (!JsonGetString(value, &string)) {
    if (error) *error = PushStr8F(arena, "%.*s must be a string", (int)key.size, (char *)key.str);
    return false;
  }
  *out = PushStr8Copy(arena, string);
  return true;
}

[[nodiscard]] bool DecodeOptionalString(Arena *arena, const JsonValue *object, String8 key,
                                        String8 *out, String8 *error) {
  JsonValue *value = JsonObjectGet(object, key);
  if (value == nullptr) {
    *out = {};
    return true;
  }
  String8 string = {};
  if (!JsonGetString(value, &string)) {
    if (error) *error = PushStr8F(arena, "%.*s must be a string", (int)key.size, (char *)key.str);
    return false;
  }
  *out = PushStr8Copy(arena, string);
  return true;
}

[[nodiscard]] bool DecodeOptionalBool(Arena *arena, const JsonValue *object, String8 key,
                                      bool *out, String8 *error) {
  JsonValue *value = JsonObjectGet(object, key);
  if (value == nullptr) {
    *out = false;
    return true;
  }
  if (!JsonGetBool(value, out)) {
    if (error) *error = PushStr8F(arena, "%.*s must be a bool", (int)key.size, (char *)key.str);
    return false;
  }
  return true;
}

[[nodiscard]] bool DecodeU32Value(Arena *arena, const JsonValue *value, const char *name, u32 *out,
                                  String8 *error) {
  u64 parsed = 0;
  if (!JsonGetU64(value, &parsed)) return FailExpected(arena, error, name, "a non-negative integer");
  if (parsed > (u64)((u32)~0)) return FailExpected(arena, error, name, "a 32-bit integer");
  *out = (u32)parsed;
  return true;
}

[[nodiscard]] bool DecodeOptionalI64(Arena *arena, const JsonValue *object, String8 key,
                                     bool *present, i64 *out, String8 *error) {
  JsonValue *value = JsonObjectGet(object, key);
  if (value == nullptr || value->kind == JsonKind::Null) {
    *present = false;
    *out = 0;
    return true;
  }
  if (!JsonGetI64(value, out)) {
    if (error) *error = PushStr8F(arena, "%.*s must be an integer or null", (int)key.size,
                                  (char *)key.str);
    return false;
  }
  *present = true;
  return true;
}

[[nodiscard]] bool DecodePosition(Arena *arena, const JsonValue *value, LspPosition *position,
                                  String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "position", "an object");
  }
  JsonValue *line = JsonObjectGet(value, Str8Lit("line"));
  JsonValue *character = JsonObjectGet(value, Str8Lit("character"));
  if (line == nullptr || character == nullptr) return Fail(arena, error, "position requires line and character");
  if (!DecodeU32Value(arena, line, "line", &position->line, error)) return false;
  if (!DecodeU32Value(arena, character, "character", &position->character, error)) return false;
  return true;
}

[[nodiscard]] bool DecodeSeverity(Arena *arena, const JsonValue *value,
                                  LspDiagnosticSeverity *severity, String8 *error) {
  u64 parsed = 0;
  if (!JsonGetU64(value, &parsed)) return FailExpected(arena, error, "severity", "an integer");
  if (parsed < (u64)LspDiagnosticSeverity::Error || parsed > (u64)LspDiagnosticSeverity::Hint) {
    return Fail(arena, error, "severity must be between 1 and 4");
  }
  *severity = (LspDiagnosticSeverity)parsed;
  return true;
}

[[nodiscard]] String8 CopyCodeValue(Arena *arena, const JsonValue *value, bool *ok, String8 *error) {
  if (value->kind == JsonKind::String) return PushStr8Copy(arena, value->string);
  i64 signed_value = 0;
  if (JsonGetI64(value, &signed_value)) {
    *ok = true;
    return PushStr8F(arena, "%lld", (long long)signed_value);
  }
  u64 unsigned_value = 0;
  if (JsonGetU64(value, &unsigned_value)) {
    *ok = true;
    return PushStr8F(arena, "%llu", (unsigned long long)unsigned_value);
  }
  *ok = false;
  if (error) *error = PushStr8F(arena, "code must be a string or integer");
  return {};
}

[[nodiscard]] bool DecodeTextEditValue(Arena *arena, const JsonValue *value, LspTextEdit *edit,
                                       String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "textEdit", "an object");
  }
  if (JsonObjectGet(value, Str8Lit("insert")) != nullptr ||
      JsonObjectGet(value, Str8Lit("replace")) != nullptr) {
    return Fail(arena, error, "generic text edits do not accept insert/replace edits");
  }
  JsonValue *range = JsonObjectGet(value, Str8Lit("range"));
  JsonValue *new_text = JsonObjectGet(value, Str8Lit("newText"));
  if (range == nullptr || new_text == nullptr) {
    return Fail(arena, error, "textEdit requires range and newText");
  }
  if (!LspDecodeRange(arena, range, &edit->range, error)) return false;
  String8 string = {};
  if (!JsonGetString(new_text, &string)) return Fail(arena, error, "newText must be a string");
  edit->new_text = PushStr8Copy(arena, string);
  return true;
}

[[nodiscard]] bool DecodeInsertReplaceEditValue(Arena *arena, const JsonValue *value,
                                                LspInsertReplaceEdit *edit, String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "insertReplaceEdit", "an object");
  }
  JsonValue *insert = JsonObjectGet(value, Str8Lit("insert"));
  JsonValue *replace = JsonObjectGet(value, Str8Lit("replace"));
  JsonValue *new_text = JsonObjectGet(value, Str8Lit("newText"));
  if (insert == nullptr || replace == nullptr || new_text == nullptr) {
    return Fail(arena, error, "insert/replace edit requires insert, replace and newText");
  }
  if (!LspDecodeRange(arena, insert, &edit->insert, error)) return false;
  if (!LspDecodeRange(arena, replace, &edit->replace, error)) return false;
  String8 string = {};
  if (!JsonGetString(new_text, &string)) return Fail(arena, error, "newText must be a string");
  edit->new_text = PushStr8Copy(arena, string);
  return true;
}

[[nodiscard]] bool NormalizeLocationResultEntry(Arena *arena, const JsonValue *value,
                                                LspLocation *location, String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "location result entry", "an object");
  }
  if (JsonObjectGet(value, Str8Lit("uri")) != nullptr) {
    return LspDecodeLocation(arena, value, location, error);
  }
  LspLocationLink link = {};
  if (!LspDecodeLocationLink(arena, value, &link, error)) return false;
  location->uri = link.target_uri;
  location->range = link.has_target_selection_range ? link.target_selection_range : link.target_range;
  return true;
}

[[nodiscard]] u32 EncodingUnits(u32 codepoint, u32 utf8_bytes, LspPositionEncoding encoding) {
  switch (encoding) {
    case LspPositionEncoding::Utf8: return utf8_bytes;
    case LspPositionEncoding::Utf16: return (codepoint > 0xFFFF) ? 2u : 1u;
    case LspPositionEncoding::Utf32: return 1;
  }
  return 1;
}

[[nodiscard]] u32 ClampToU32(u64 value) {
  u64 max = (u64)((u32)~0);
  return (u32)Min(value, max);
}

[[nodiscard]] u64 ClampOffsetToCodepointStart(const Buffer *buffer, u64 offset) {
  u64 clamped = Min(offset, BufferSize(buffer));
  while (clamped > 0 && clamped < BufferSize(buffer) && Utf8IsContinuation(BufferByteAt(buffer, clamped))) {
    clamped -= 1;
  }
  return clamped;
}

[[nodiscard]] LspPositionEncoding DecodePositionEncoding(String8 value) {
  if (Str8Match(value, Str8Lit("utf-8"))) return LspPositionEncoding::Utf8;
  if (Str8Match(value, Str8Lit("utf-16"))) return LspPositionEncoding::Utf16;
  if (Str8Match(value, Str8Lit("utf-32"))) return LspPositionEncoding::Utf32;
  return LspPositionEncoding::Utf16;
}

[[nodiscard]] bool DecodeBoolOrObjectCapability(Arena *arena, const JsonValue *value, const char *name,
                                                bool *supported, String8 *error) {
  if (value == nullptr) {
    *supported = false;
    return true;
  }
  if (value->kind == JsonKind::Bool) {
    *supported = value->boolean;
    return true;
  }
  if (value->kind == JsonKind::Object) {
    *supported = true;
    return true;
  }
  return FailExpected(arena, error, name, "a bool or object");
}

[[nodiscard]] bool DecodeStringArray(Arena *arena, const JsonValue *value, String8 **strings,
                                     u64 *count, String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Array) return FailExpected(arena, error, "array", "an array");
  *count = value->array.count;
  *strings = nullptr;
  if (*count == 0) return true;
  *strings = PushArray(arena, String8, *count);
  for (u64 i = 0; i < *count; i += 1) {
    String8 string = {};
    if (!JsonGetString(value->array.items[i], &string)) return Fail(arena, error, "array item must be a string");
    (*strings)[i] = PushStr8Copy(arena, string);
  }
  return true;
}

[[nodiscard]] bool DecodeTextDocumentSync(Arena *arena, const JsonValue *value,
                                          LspTextDocumentSync *sync, String8 *error) {
  *sync = {};
  if (value == nullptr) return true;
  if (value->kind == JsonKind::Number) {
    u64 kind = 0;
    if (!JsonGetU64(value, &kind) || kind > 2) {
      return Fail(arena, error, "textDocumentSync kind must be 0, 1 or 2");
    }
    sync->kind = (LspTextDocumentSyncKind)kind;
    return true;
  }
  if (value->kind != JsonKind::Object) return FailExpected(arena, error, "textDocumentSync", "an integer or object");

  JsonValue *change = JsonObjectGet(value, Str8Lit("change"));
  if (change != nullptr) {
    u64 kind = 0;
    if (!JsonGetU64(change, &kind) || kind > 2) {
      return Fail(arena, error, "textDocumentSync.change must be 0, 1 or 2");
    }
    sync->kind = (LspTextDocumentSyncKind)kind;
  }
  if (!DecodeOptionalBool(arena, value, Str8Lit("openClose"), &sync->open_close, error)) return false;

  JsonValue *save = JsonObjectGet(value, Str8Lit("save"));
  if (save == nullptr) return true;
  if (save->kind == JsonKind::Bool) {
    sync->save = save->boolean;
    return true;
  }
  if (save->kind != JsonKind::Object) return FailExpected(arena, error, "textDocumentSync.save", "a bool or object");
  sync->save = true;
  return DecodeOptionalBool(arena, save, Str8Lit("includeText"), &sync->save_include_text, error);
}

[[nodiscard]] bool DecodeWorkspaceFolders(Arena *arena, const JsonValue *value,
                                          LspWorkspaceFoldersCapabilities *folders,
                                          String8 *error) {
  *folders = {};
  if (value == nullptr) return true;
  if (value->kind == JsonKind::Bool) {
    folders->supported = value->boolean;
    return true;
  }
  if (value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "workspaceFolders", "a bool or object");
  }
  if (!DecodeOptionalBool(arena, value, Str8Lit("supported"), &folders->supported, error)) return false;

  JsonValue *change = JsonObjectGet(value, Str8Lit("changeNotifications"));
  if (change == nullptr) return true;
  if (change->kind == JsonKind::Bool) {
    folders->change_notifications = change->boolean;
    return true;
  }
  if (change->kind != JsonKind::String) {
    return FailExpected(arena, error, "workspaceFolders.changeNotifications", "a bool or string");
  }
  folders->change_notifications = true;
  folders->change_notifications_id = PushStr8Copy(arena, change->string);
  return true;
}

[[nodiscard]] bool DecodeWorkspaceEditCapabilities(Arena *arena, const JsonValue *value,
                                                   LspWorkspaceEditCapabilities *capabilities,
                                                   String8 *error) {
  *capabilities = {};
  if (value == nullptr) return true;
  if (value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "workspaceEdit", "an object");
  }
  if (!DecodeOptionalBool(arena, value, Str8Lit("documentChanges"), &capabilities->document_changes, error)) {
    return false;
  }
  JsonValue *resource_ops = JsonObjectGet(value, Str8Lit("resourceOperations"));
  if (resource_ops == nullptr) return true;
  return DecodeStringArray(arena, resource_ops, &capabilities->resource_operations,
                           &capabilities->resource_operation_count, error);
}

[[nodiscard]] bool DecodeCompletionItem(Arena *arena, const JsonValue *value,
                                        LspCompletionItem *item, String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "completion item", "an object");
  }
  *item = {};
  item->insert_text_format = LspInsertTextFormat::PlainText;
  if (!DecodeRequiredString(arena, value, Str8Lit("label"), &item->label, error)) return false;
  if (!DecodeOptionalString(arena, value, Str8Lit("detail"), &item->detail, error)) return false;
  if (!DecodeOptionalString(arena, value, Str8Lit("filterText"), &item->filter_text, error)) return false;
  if (!DecodeOptionalString(arena, value, Str8Lit("sortText"), &item->sort_text, error)) return false;
  if (!DecodeOptionalString(arena, value, Str8Lit("insertText"), &item->insert_text, error)) return false;

  JsonValue *insert_text_format = JsonObjectGet(value, Str8Lit("insertTextFormat"));
  if (insert_text_format != nullptr) {
    u64 format = 0;
    if (!JsonGetU64(insert_text_format, &format) || (format != 1 && format != 2)) {
      return Fail(arena, error, "insertTextFormat must be 1 or 2");
    }
    item->insert_text_format = (LspInsertTextFormat)format;
  }

  JsonValue *text_edit = JsonObjectGet(value, Str8Lit("textEdit"));
  if (text_edit != nullptr) {
    if (JsonObjectGet(text_edit, Str8Lit("range")) != nullptr) {
      item->text_edit_kind = LspCompletionTextEditKind::TextEdit;
      if (!DecodeTextEditValue(arena, text_edit, &item->text_edit, error)) return false;
    } else {
      item->text_edit_kind = LspCompletionTextEditKind::InsertReplace;
      if (!DecodeInsertReplaceEditValue(arena, text_edit, &item->insert_replace_edit, error)) {
        return false;
      }
    }
  }

  JsonValue *additional = JsonObjectGet(value, Str8Lit("additionalTextEdits"));
  if (additional != nullptr &&
      !LspDecodeTextEdits(arena, additional, &item->additional_text_edits,
                          &item->additional_text_edit_count, error)) {
    return false;
  }

  return true;
}

[[nodiscard]] bool DecodeHoverFragment(Arena *arena, const JsonValue *value, String8 *text,
                                       String8 *language, String8 *kind, String8 *error) {
  *text = {};
  *language = {};
  *kind = {};
  if (value == nullptr) return FailExpected(arena, error, "hover contents", "a value");
  if (value->kind == JsonKind::String) {
    *text = PushStr8Copy(arena, value->string);
    return true;
  }
  if (value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "hover contents", "a string or object");
  }

  JsonValue *kind_value = JsonObjectGet(value, Str8Lit("kind"));
  JsonValue *language_value = JsonObjectGet(value, Str8Lit("language"));
  JsonValue *text_value = JsonObjectGet(value, Str8Lit("value"));
  if (text_value == nullptr || text_value->kind != JsonKind::String) {
    return Fail(arena, error, "hover contents.value must be a string");
  }
  *text = PushStr8Copy(arena, text_value->string);

  if (kind_value != nullptr) {
    if (kind_value->kind != JsonKind::String) return Fail(arena, error, "hover contents.kind must be a string");
    *kind = PushStr8Copy(arena, kind_value->string);
    return true;
  }
  if (language_value != nullptr) {
    if (language_value->kind != JsonKind::String) {
      return Fail(arena, error, "hover contents.language must be a string");
    }
    *language = PushStr8Copy(arena, language_value->string);
    return true;
  }
  return Fail(arena, error, "hover contents object must be MarkupContent or MarkedString");
}

[[nodiscard]] bool DecodeHoverContents(Arena *arena, const JsonValue *value, String8 *text,
                                       String8 *language, String8 *kind, String8 *error) {
  *text = {};
  *language = {};
  *kind = {};
  if (value->kind != JsonKind::Array) {
    return DecodeHoverFragment(arena, value, text, language, kind, error);
  }

  String8List parts = {};
  for (u64 i = 0; i < value->array.count; i += 1) {
    String8 part = {}, part_language = {}, part_kind = {};
    if (!DecodeHoverFragment(arena, value->array.items[i], &part, &part_language, &part_kind, error)) {
      return false;
    }
    Str8ListPush(arena, &parts, part);
  }
  *text = Str8ListJoin(arena, &parts, Str8Lit("\n\n"));
  return true;
}

[[nodiscard]] bool DecodeDiagnostic(Arena *arena, const JsonValue *value, LspDiagnostic *diagnostic,
                                    String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "diagnostic", "an object");
  }
  *diagnostic = {};
  diagnostic->severity = LspDiagnosticSeverity::Error;

  JsonValue *range = JsonObjectGet(value, Str8Lit("range"));
  JsonValue *message = JsonObjectGet(value, Str8Lit("message"));
  if (range == nullptr || message == nullptr) {
    return Fail(arena, error, "diagnostic requires range and message");
  }
  if (!LspDecodeRange(arena, range, &diagnostic->range, error)) return false;
  if (!JsonGetString(message, &diagnostic->message)) {
    return Fail(arena, error, "diagnostic.message must be a string");
  }
  diagnostic->message = PushStr8Copy(arena, diagnostic->message);

  JsonValue *severity = JsonObjectGet(value, Str8Lit("severity"));
  if (severity != nullptr && !DecodeSeverity(arena, severity, &diagnostic->severity, error)) {
    return false;
  }
  JsonValue *code = JsonObjectGet(value, Str8Lit("code"));
  if (code != nullptr) {
    bool ok = false;
    diagnostic->code = CopyCodeValue(arena, code, &ok, error);
    if (!ok) return false;
  }
  if (!DecodeOptionalString(arena, value, Str8Lit("source"), &diagnostic->source, error)) return false;

  JsonValue *related = JsonObjectGet(value, Str8Lit("relatedInformation"));
  if (related != nullptr) {
    if (related->kind != JsonKind::Array) return FailExpected(arena, error, "relatedInformation", "an array");
    diagnostic->related_information_count = related->array.count;
    if (diagnostic->related_information_count != 0) {
      diagnostic->related_information =
          PushArray(arena, LspDiagnosticRelatedInformation, diagnostic->related_information_count);
    }
    for (u64 i = 0; i < diagnostic->related_information_count; i += 1) {
      JsonValue *item = related->array.items[i];
      if (item == nullptr || item->kind != JsonKind::Object) {
        return FailExpected(arena, error, "relatedInformation item", "an object");
      }
      JsonValue *location = JsonObjectGet(item, Str8Lit("location"));
      JsonValue *related_message = JsonObjectGet(item, Str8Lit("message"));
      if (location == nullptr || related_message == nullptr) {
        return Fail(arena, error, "relatedInformation item requires location and message");
      }
      if (!LspDecodeLocation(arena, location, &diagnostic->related_information[i].location, error)) {
        return false;
      }
      String8 message_string = {};
      if (!JsonGetString(related_message, &message_string)) {
        return Fail(arena, error, "relatedInformation.message must be a string");
      }
      diagnostic->related_information[i].message = PushStr8Copy(arena, message_string);
    }
  }

  JsonValue *tags = JsonObjectGet(value, Str8Lit("tags"));
  if (tags != nullptr) {
    if (tags->kind != JsonKind::Array) return FailExpected(arena, error, "tags", "an array");
    diagnostic->tag_count = tags->array.count;
    if (diagnostic->tag_count != 0) diagnostic->tags = PushArray(arena, u64, diagnostic->tag_count);
    for (u64 i = 0; i < diagnostic->tag_count; i += 1) {
      if (!JsonGetU64(tags->array.items[i], &diagnostic->tags[i])) {
        return Fail(arena, error, "diagnostic tag must be an integer");
      }
    }
  }

  return true;
}

[[nodiscard]] bool DecodeWorkspaceEditEntry(Arena *arena, const JsonValue *value,
                                            LspWorkspaceEditEntry *entry, String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "documentChanges entry", "an object");
  }
  *entry = {};

  JsonValue *kind = JsonObjectGet(value, Str8Lit("kind"));
  if (kind != nullptr) {
    String8 kind_string = {};
    if (!JsonGetString(kind, &kind_string)) return Fail(arena, error, "documentChanges.kind must be a string");
    if (Str8Match(kind_string, Str8Lit("create"))) {
      entry->kind = LspWorkspaceEditEntryKind::CreateFile;
      return DecodeRequiredString(arena, value, Str8Lit("uri"), &entry->uri, error);
    }
    if (Str8Match(kind_string, Str8Lit("rename"))) {
      entry->kind = LspWorkspaceEditEntryKind::RenameFile;
      if (!DecodeRequiredString(arena, value, Str8Lit("oldUri"), &entry->old_uri, error)) return false;
      return DecodeRequiredString(arena, value, Str8Lit("newUri"), &entry->new_uri, error);
    }
    if (Str8Match(kind_string, Str8Lit("delete"))) {
      entry->kind = LspWorkspaceEditEntryKind::DeleteFile;
      return DecodeRequiredString(arena, value, Str8Lit("uri"), &entry->uri, error);
    }
    return Fail(arena, error, "unsupported documentChanges.kind");
  }

  JsonValue *text_document = JsonObjectGet(value, Str8Lit("textDocument"));
  JsonValue *edits = JsonObjectGet(value, Str8Lit("edits"));
  if (text_document == nullptr || edits == nullptr) {
    return Fail(arena, error, "text document edit requires textDocument and edits");
  }
  if (text_document->kind != JsonKind::Object) return FailExpected(arena, error, "textDocument", "an object");
  entry->kind = LspWorkspaceEditEntryKind::TextDocumentEdits;
  if (!DecodeRequiredString(arena, text_document, Str8Lit("uri"), &entry->uri, error)) return false;
  if (!DecodeOptionalI64(arena, text_document, Str8Lit("version"), &entry->has_version, &entry->version,
                         error)) {
    return false;
  }
  return LspDecodeTextEdits(arena, edits, &entry->edits, &entry->edit_count, error);
}

}  // namespace

String8 LspPathToUri(Arena *arena, String8 absolute_path) {
  if (absolute_path.size == 0 || HasEmbeddedNul(absolute_path)) return {};

  bool windows_drive = IsWindowsDrivePath(absolute_path);
  bool unc = IsUncPath(absolute_path);
  bool posix = absolute_path.str[0] == '/';
  if (!windows_drive && !unc && !posix) return {};

  u8 *dst = PushArrayNoZero(arena, u8, 8 + absolute_path.size * 3 + 8);
  u64 size = 0;
  memcpy(dst, "file://", 7);
  size = 7;

  if (unc) {
    u64 i = 2;
    while (i < absolute_path.size &&
           (absolute_path.str[i] == '/' || absolute_path.str[i] == '\\')) i += 1;
    u64 host_start = i;
    while (i < absolute_path.size &&
           absolute_path.str[i] != '/' && absolute_path.str[i] != '\\') i += 1;
    if (i == host_start) return {};
    for (u64 j = host_start; j < i; j += 1) {
      u8 c = absolute_path.str[j];
      if (IsUnreserved(c)) {
        AppendByte(dst, &size, c);
      } else {
        AppendEncoded(dst, &size, c);
      }
    }
    if (i == absolute_path.size) {
      AppendByte(dst, &size, '/');
    }
    for (; i < absolute_path.size; i += 1) {
      u8 c = absolute_path.str[i];
      if (c == '/' || c == '\\') {
        AppendByte(dst, &size, '/');
      } else if (IsUnreserved(c)) {
        AppendByte(dst, &size, c);
      } else {
        AppendEncoded(dst, &size, c);
      }
    }
  } else {
    if (windows_drive) AppendByte(dst, &size, '/');
    for (u64 i = 0; i < absolute_path.size; i += 1) {
      u8 c = absolute_path.str[i];
      if (c == '/' || c == '\\') {
        AppendByte(dst, &size, '/');
      } else if (windows_drive && i == 1 && c == ':') {
        AppendByte(dst, &size, ':');
      } else if (IsUnreserved(c)) {
        AppendByte(dst, &size, c);
      } else {
        AppendEncoded(dst, &size, c);
      }
    }
  }

  dst[size] = 0;
  return String8{dst, size};
}

String8 LspUriToPath(Arena *arena, String8 uri) {
  if (uri.size == 0 || HasEmbeddedNul(uri) ||
      !Str8StartsWith(uri, Str8Lit("file://"), StringMatch::CaseInsensitive)) {
    return {};
  }

  String8 rest = Str8Skip(uri, 7);
  u64 slash = Str8FindFirstChar(rest, '/');
  if (slash == rest.size) return {};

  String8 authority = Str8Prefix(rest, slash);
  String8 path = Str8Skip(rest, slash);
  if (path.size == 0 || path.str[0] != '/') return {};

  bool ok = false;
  if (authority.size == 0 || Str8Match(authority, Str8Lit("localhost"), StringMatch::CaseInsensitive)) {
    String8 decoded = CopyDecodedUriBytes(arena, path, true, &ok);
    if (!ok || decoded.size == 0 || decoded.str[0] != '/') return {};
    if (decoded.size >= 3 && decoded.str[0] == '/' && CharIsAlpha(decoded.str[1]) &&
        decoded.str[2] == ':') {
      if (decoded.size == 3 || decoded.str[3] == '/') return Str8Skip(decoded, 1);
      return {};
    }
    return decoded;
  }

  if (authority.size == 2 && CharIsAlpha(authority.str[0]) && authority.str[1] == ':') return {};

  String8 decoded_authority = CopyDecodedUriBytes(arena, authority, false, &ok);
  if (!ok || decoded_authority.size == 0) return {};
  String8 decoded_path = CopyDecodedUriBytes(arena, path, true, &ok);
  if (!ok || decoded_path.size == 0 || decoded_path.str[0] != '/') return {};
  return PushStr8Cat(arena, PushStr8Cat(arena, Str8Lit("//"), decoded_authority), decoded_path);
}

LspPosition LspPositionFromOffset(const Buffer *buffer, u64 offset, LspPositionEncoding encoding) {
  u64 clamped = ClampOffsetToCodepointStart(buffer, offset);
  u64 line = BufferLineFromOffset(buffer, clamped);
  u64 line_start = BufferOffsetFromLine(buffer, line);

  u64 character = 0;
  for (u64 pos = line_start; pos < clamped;) {
    DecodedCodepoint decoded = BufferDecodeAt(buffer, pos);
    character += EncodingUnits(decoded.codepoint, decoded.advance, encoding);
    pos += decoded.advance;
  }

  return LspPosition{ClampToU32(line), ClampToU32(character)};
}

u64 LspOffsetFromPosition(const Buffer *buffer, LspPosition position, LspPositionEncoding encoding) {
  if (position.line >= BufferLineCount(buffer)) return BufferSize(buffer);

  u64 line = position.line;
  u64 line_start = BufferOffsetFromLine(buffer, line);
  u64 line_end = BufferLineEnd(buffer, line);
  u64 consumed = 0;
  u64 pos = line_start;

  while (pos < line_end) {
    if (position.character <= consumed) return pos;
    DecodedCodepoint decoded = BufferDecodeAt(buffer, pos);
    u32 units = EncodingUnits(decoded.codepoint, decoded.advance, encoding);
    if (position.character < consumed + units) return pos;
    pos += decoded.advance;
    consumed += units;
  }

  return line_end;
}

bool LspDecodeRange(Arena *arena, const JsonValue *value, LspRange *range, String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "range", "an object");
  }
  JsonValue *start = JsonObjectGet(value, Str8Lit("start"));
  JsonValue *end = JsonObjectGet(value, Str8Lit("end"));
  if (start == nullptr || end == nullptr) return Fail(arena, error, "range requires start and end");
  if (!DecodePosition(arena, start, &range->start, error)) return false;
  if (!DecodePosition(arena, end, &range->end, error)) return false;
  return true;
}

bool LspDecodeLocation(Arena *arena, const JsonValue *value, LspLocation *location, String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "location", "an object");
  }
  JsonValue *range = JsonObjectGet(value, Str8Lit("range"));
  if (!DecodeRequiredString(arena, value, Str8Lit("uri"), &location->uri, error)) return false;
  if (range == nullptr) return Fail(arena, error, "location requires range");
  return LspDecodeRange(arena, range, &location->range, error);
}

bool LspDecodeLocationLink(Arena *arena, const JsonValue *value, LspLocationLink *location_link,
                           String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "locationLink", "an object");
  }
  *location_link = {};
  if (!DecodeRequiredString(arena, value, Str8Lit("targetUri"), &location_link->target_uri, error)) {
    return false;
  }
  JsonValue *target_range = JsonObjectGet(value, Str8Lit("targetRange"));
  if (target_range == nullptr) return Fail(arena, error, "locationLink requires targetRange");
  if (!LspDecodeRange(arena, target_range, &location_link->target_range, error)) return false;

  JsonValue *origin = JsonObjectGet(value, Str8Lit("originSelectionRange"));
  if (origin != nullptr) {
    location_link->has_origin_selection_range = true;
    if (!LspDecodeRange(arena, origin, &location_link->origin_selection_range, error)) return false;
  }

  JsonValue *target_selection = JsonObjectGet(value, Str8Lit("targetSelectionRange"));
  if (target_selection != nullptr) {
    location_link->has_target_selection_range = true;
    if (!LspDecodeRange(arena, target_selection, &location_link->target_selection_range, error)) {
      return false;
    }
  }
  return true;
}

bool LspDecodeLocationResult(Arena *arena, const JsonValue *value, LspLocation **locations, u64 *count,
                             String8 *error) {
  *locations = nullptr;
  *count = 0;
  if (value == nullptr || value->kind == JsonKind::Null) return true;
  if (value->kind == JsonKind::Array) {
    *count = value->array.count;
    if (*count == 0) return true;
    *locations = PushArray(arena, LspLocation, *count);
    for (u64 i = 0; i < *count; i += 1) {
      if (!NormalizeLocationResultEntry(arena, value->array.items[i], &(*locations)[i], error)) {
        return false;
      }
    }
    return true;
  }
  *count = 1;
  *locations = PushArray(arena, LspLocation, 1);
  return NormalizeLocationResultEntry(arena, value, &(*locations)[0], error);
}

bool LspDecodeInitializeResult(Arena *arena, const JsonValue *value, LspInitializeResult *result,
                               String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "initialize result", "an object");
  }
  *result = {};
  result->capabilities.position_encoding = LspPositionEncoding::Utf16;

  JsonValue *capabilities = JsonObjectGet(value, Str8Lit("capabilities"));
  if (capabilities == nullptr || capabilities->kind != JsonKind::Object) {
    return Fail(arena, error, "initialize result requires capabilities");
  }

  JsonValue *position_encoding = JsonObjectGet(capabilities, Str8Lit("positionEncoding"));
  if (position_encoding != nullptr) {
    String8 encoding = {};
    if (!JsonGetString(position_encoding, &encoding)) {
      return Fail(arena, error, "positionEncoding must be a string");
    }
    result->capabilities.position_encoding = DecodePositionEncoding(encoding);
  }

  if (!DecodeTextDocumentSync(arena, JsonObjectGet(capabilities, Str8Lit("textDocumentSync")),
                              &result->capabilities.text_document_sync, error)) {
    return false;
  }

  JsonValue *completion_provider = JsonObjectGet(capabilities, Str8Lit("completionProvider"));
  if (completion_provider != nullptr) {
    if (completion_provider->kind != JsonKind::Object) {
      return FailExpected(arena, error, "completionProvider", "an object");
    }
    result->capabilities.completion_provider = true;
  }

  if (!DecodeBoolOrObjectCapability(arena, JsonObjectGet(capabilities, Str8Lit("hoverProvider")),
                                    "hoverProvider", &result->capabilities.hover_provider, error)) {
    return false;
  }
  if (!DecodeBoolOrObjectCapability(arena, JsonObjectGet(capabilities, Str8Lit("declarationProvider")),
                                    "declarationProvider", &result->capabilities.declaration_provider,
                                    error)) {
    return false;
  }
  if (!DecodeBoolOrObjectCapability(arena, JsonObjectGet(capabilities, Str8Lit("definitionProvider")),
                                    "definitionProvider", &result->capabilities.definition_provider,
                                    error)) {
    return false;
  }
  if (!DecodeBoolOrObjectCapability(arena,
                                    JsonObjectGet(capabilities, Str8Lit("implementationProvider")),
                                    "implementationProvider",
                                    &result->capabilities.implementation_provider, error)) {
    return false;
  }
  if (!DecodeBoolOrObjectCapability(arena, JsonObjectGet(capabilities, Str8Lit("typeDefinitionProvider")),
                                    "typeDefinitionProvider",
                                    &result->capabilities.type_definition_provider, error)) {
    return false;
  }
  if (!DecodeBoolOrObjectCapability(arena,
                                    JsonObjectGet(capabilities, Str8Lit("documentFormattingProvider")),
                                    "documentFormattingProvider",
                                    &result->capabilities.document_formatting_provider, error)) {
    return false;
  }

  JsonValue *rename_provider = JsonObjectGet(capabilities, Str8Lit("renameProvider"));
  if (rename_provider != nullptr) {
    if (rename_provider->kind == JsonKind::Bool) {
      result->capabilities.rename_provider = rename_provider->boolean;
    } else if (rename_provider->kind == JsonKind::Object) {
      result->capabilities.rename_provider = true;
      if (!DecodeOptionalBool(arena, rename_provider, Str8Lit("prepareProvider"),
                              &result->capabilities.rename_prepare_provider, error)) {
        return false;
      }
    } else {
      return FailExpected(arena, error, "renameProvider", "a bool or object");
    }
  }

  JsonValue *workspace = JsonObjectGet(capabilities, Str8Lit("workspace"));
  if (workspace != nullptr) {
    if (workspace->kind != JsonKind::Object) return FailExpected(arena, error, "workspace", "an object");
    if (!DecodeWorkspaceFolders(arena, JsonObjectGet(workspace, Str8Lit("workspaceFolders")),
                                &result->capabilities.workspace_folders, error)) {
      return false;
    }
    if (!DecodeWorkspaceEditCapabilities(arena, JsonObjectGet(workspace, Str8Lit("workspaceEdit")),
                                         &result->capabilities.workspace_edit, error)) {
      return false;
    }
  }

  return true;
}

bool LspDecodeCompletionResult(Arena *arena, const JsonValue *value, LspCompletionResult *result,
                               String8 *error) {
  *result = {};
  if (value == nullptr || value->kind == JsonKind::Null) return true;
  result->present = true;

  const JsonValue *items = value;
  if (value->kind == JsonKind::Object) {
    items = JsonObjectGet(value, Str8Lit("items"));
    if (items == nullptr || items->kind != JsonKind::Array) {
      return Fail(arena, error, "completion list requires items");
    }
    if (!DecodeOptionalBool(arena, value, Str8Lit("isIncomplete"), &result->is_incomplete, error)) {
      return false;
    }
  } else if (value->kind != JsonKind::Array) {
    return FailExpected(arena, error, "completion result", "null, an array or an object");
  }

  result->count = items->array.count;
  if (result->count == 0) return true;
  result->items = PushArray(arena, LspCompletionItem, result->count);
  for (u64 i = 0; i < result->count; i += 1) {
    if (!DecodeCompletionItem(arena, items->array.items[i], &result->items[i], error)) return false;
  }
  return true;
}

bool LspDecodeHover(Arena *arena, const JsonValue *value, LspHover *hover, String8 *error) {
  *hover = {};
  if (value == nullptr || value->kind == JsonKind::Null) return true;
  if (value->kind != JsonKind::Object) return FailExpected(arena, error, "hover", "an object");
  hover->present = true;

  JsonValue *contents = JsonObjectGet(value, Str8Lit("contents"));
  if (contents == nullptr) return Fail(arena, error, "hover requires contents");
  if (!DecodeHoverContents(arena, contents, &hover->text, &hover->language, &hover->kind, error)) {
    return false;
  }

  JsonValue *range = JsonObjectGet(value, Str8Lit("range"));
  if (range != nullptr) {
    hover->has_range = true;
    if (!LspDecodeRange(arena, range, &hover->range, error)) return false;
  }
  return true;
}

bool LspDecodePublishDiagnostics(Arena *arena, const JsonValue *value, LspPublishDiagnostics *payload,
                                 String8 *error) {
  if (value == nullptr || value->kind != JsonKind::Object) {
    return FailExpected(arena, error, "publishDiagnostics", "an object");
  }
  *payload = {};
  if (!DecodeRequiredString(arena, value, Str8Lit("uri"), &payload->uri, error)) return false;
  if (!DecodeOptionalI64(arena, value, Str8Lit("version"), &payload->has_version, &payload->version,
                         error)) {
    return false;
  }
  JsonValue *diagnostics = JsonObjectGet(value, Str8Lit("diagnostics"));
  if (diagnostics == nullptr || diagnostics->kind != JsonKind::Array) {
    return Fail(arena, error, "publishDiagnostics requires diagnostics");
  }
  payload->diagnostic_count = diagnostics->array.count;
  if (payload->diagnostic_count != 0) {
    payload->diagnostics = PushArray(arena, LspDiagnostic, payload->diagnostic_count);
  }
  for (u64 i = 0; i < payload->diagnostic_count; i += 1) {
    if (!DecodeDiagnostic(arena, diagnostics->array.items[i], &payload->diagnostics[i], error)) {
      return false;
    }
  }
  return true;
}

bool LspDecodeWorkspaceEdit(Arena *arena, const JsonValue *value, LspWorkspaceEdit *edit,
                            String8 *error) {
  *edit = {};
  if (value == nullptr || value->kind == JsonKind::Null) return true;
  if (value->kind != JsonKind::Object) return FailExpected(arena, error, "workspaceEdit", "an object");
  edit->present = true;

  JsonValue *changes = JsonObjectGet(value, Str8Lit("changes"));
  JsonValue *document_changes = JsonObjectGet(value, Str8Lit("documentChanges"));
  if (changes != nullptr && document_changes != nullptr) {
    return Fail(arena, error, "workspaceEdit cannot contain both changes and documentChanges");
  }

  if (changes != nullptr) {
    if (changes->kind != JsonKind::Object) return FailExpected(arena, error, "changes", "an object");
    edit->count = CountObjectMembers(changes);
    if (edit->count == 0) return true;
    edit->entries = PushArray(arena, LspWorkspaceEditEntry, edit->count);
    u64 index = 0;
    for (JsonMember *member = changes->object; member; member = member->next, index += 1) {
      edit->entries[index].kind = LspWorkspaceEditEntryKind::TextDocumentEdits;
      edit->entries[index].uri = PushStr8Copy(arena, member->name);
      if (!LspDecodeTextEdits(arena, member->value, &edit->entries[index].edits,
                              &edit->entries[index].edit_count, error)) {
        return false;
      }
    }
    return true;
  }

  if (document_changes != nullptr) {
    if (document_changes->kind != JsonKind::Array) {
      return FailExpected(arena, error, "documentChanges", "an array");
    }
    edit->count = document_changes->array.count;
    if (edit->count == 0) return true;
    edit->entries = PushArray(arena, LspWorkspaceEditEntry, edit->count);
    for (u64 i = 0; i < edit->count; i += 1) {
      if (!DecodeWorkspaceEditEntry(arena, document_changes->array.items[i], &edit->entries[i],
                                    error)) {
        return false;
      }
    }
  }

  return true;
}

bool LspDecodeTextEdits(Arena *arena, const JsonValue *value, LspTextEdit **edits, u64 *count,
                        String8 *error) {
  *edits = nullptr;
  *count = 0;
  if (value == nullptr || value->kind != JsonKind::Array) {
    return FailExpected(arena, error, "text edits", "an array");
  }
  *count = value->array.count;
  if (*count == 0) return true;
  *edits = PushArray(arena, LspTextEdit, *count);
  for (u64 i = 0; i < *count; i += 1) {
    if (!DecodeTextEditValue(arena, value->array.items[i], &(*edits)[i], error)) return false;
  }
  return true;
}
