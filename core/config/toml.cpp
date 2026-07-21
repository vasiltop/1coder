#include "config/toml.h"

#include <stdlib.h>

namespace {

struct Parser {
  Arena *arena;
  String8 text;
  u64 pos;
  u64 line;
  String8 error;
};

[[nodiscard]] bool AtEnd(const Parser *p) { return p->pos >= p->text.size; }

[[nodiscard]] u8 Peek(const Parser *p) {
  if (AtEnd(p)) return 0;
  return p->text.str[p->pos];
}

void Advance(Parser *p) {
  if (AtEnd(p)) return;
  if (p->text.str[p->pos] == '\n') p->line += 1;
  p->pos += 1;
}

void Fail(Parser *p, const char *message) {
  if (p->error.size == 0) p->error = PushStr8Copy(p->arena, Str8C(message));
}

void SkipSpaces(Parser *p) {
  while (!AtEnd(p)) {
    u8 c = Peek(p);
    if (c == ' ' || c == '\t' || c == '\r') {
      Advance(p);
      continue;
    }
    break;
  }
}

void SkipComment(Parser *p) {
  if (Peek(p) != '#') return;
  while (!AtEnd(p) && Peek(p) != '\n') Advance(p);
}

void SkipSpacesAndComments(Parser *p) {
  for (;;) {
    SkipSpaces(p);
    if (Peek(p) == '#') {
      SkipComment(p);
      continue;
    }
    break;
  }
}

[[nodiscard]] bool IsBareKeyChar(u8 c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-';
}

[[nodiscard]] TomlValue *NewValue(Arena *arena, TomlKind kind) {
  TomlValue *value = PushStruct(arena, TomlValue);
  value->kind = kind;
  return value;
}

[[nodiscard]] TomlValue *EnsureTable(Arena *arena, TomlValue *parent, String8 name, bool *created) {
  if (created) *created = false;
  if (!parent || parent->kind != TomlKind::Table) return nullptr;

  for (TomlMember *m = parent->table; m; m = m->next) {
    if (Str8Match(m->name, name)) {
      if (m->value && m->value->kind == TomlKind::Table) return m->value;
      return nullptr;
    }
  }

  TomlValue *child = NewValue(arena, TomlKind::Table);
  TomlMember *member = PushStruct(arena, TomlMember);
  member->name = PushStr8Copy(arena, name);
  member->value = child;
  member->next = parent->table;
  parent->table = member;
  if (created) *created = true;
  return child;
}

[[nodiscard]] bool SetMember(Arena *arena, TomlValue *table, String8 name, TomlValue *value) {
  if (!table || table->kind != TomlKind::Table || !value) return false;
  for (TomlMember *m = table->table; m; m = m->next) {
    if (Str8Match(m->name, name)) {
      m->value = value;
      return true;
    }
  }
  TomlMember *member = PushStruct(arena, TomlMember);
  member->name = PushStr8Copy(arena, name);
  member->value = value;
  member->next = table->table;
  table->table = member;
  return true;
}

[[nodiscard]] String8 ParseBareKey(Parser *p) {
  u64 start = p->pos;
  while (!AtEnd(p) && IsBareKeyChar(Peek(p))) Advance(p);
  if (p->pos == start) {
    Fail(p, "expected key");
    return String8{};
  }
  return Str8Substr(p->text, RangeU64{start, p->pos});
}

[[nodiscard]] String8 ParseQuotedString(Parser *p) {
  if (Peek(p) != '"') {
    Fail(p, "expected string");
    return String8{};
  }
  Advance(p);

  TempArena scratch = ScratchBegin1(p->arena);
  String8List parts = {};
  u64 chunk_start = p->pos;

  while (!AtEnd(p)) {
    u8 c = Peek(p);
    if (c == '"') {
      if (p->pos > chunk_start) {
        Str8ListPush(scratch.arena, &parts, Str8Substr(p->text, RangeU64{chunk_start, p->pos}));
      }
      Advance(p);
      String8 joined = Str8ListJoin(p->arena, &parts, Str8Lit(""));
      ScratchEnd(scratch);
      return joined;
    }
    if (c == '\n') {
      Fail(p, "unterminated string");
      ScratchEnd(scratch);
      return String8{};
    }
    if (c == '\\') {
      if (p->pos > chunk_start) {
        Str8ListPush(scratch.arena, &parts, Str8Substr(p->text, RangeU64{chunk_start, p->pos}));
      }
      Advance(p);
      u8 esc = Peek(p);
      u8 out = 0;
      switch (esc) {
        case 'n': out = '\n'; break;
        case 't': out = '\t'; break;
        case 'r': out = '\r'; break;
        case '"': out = '"'; break;
        case '\\': out = '\\'; break;
        default:
          Fail(p, "invalid escape in string");
          ScratchEnd(scratch);
          return String8{};
      }
      Advance(p);
      u8 *byte = PushArrayNoZero(p->arena, u8, 1);
      *byte = out;
      Str8ListPush(scratch.arena, &parts, String8{byte, 1});
      chunk_start = p->pos;
      continue;
    }
    Advance(p);
  }

  Fail(p, "unterminated string");
  ScratchEnd(scratch);
  return String8{};
}

[[nodiscard]] String8 ParseKey(Parser *p) {
  SkipSpaces(p);
  if (Peek(p) == '"') return ParseQuotedString(p);
  return ParseBareKey(p);
}

TomlValue *ParseValue(Parser *p);

[[nodiscard]] TomlValue *ParseArray(Parser *p) {
  if (Peek(p) != '[') {
    Fail(p, "expected '['");
    return nullptr;
  }
  Advance(p);

  TempArena scratch = ScratchBegin1(p->arena);
  TomlValue **items = nullptr;
  u64 count = 0;
  u64 capacity = 0;

  SkipSpacesAndComments(p);
  while (Peek(p) == '\n') {
    Advance(p);
    SkipSpacesAndComments(p);
  }

  if (Peek(p) != ']') {
    for (;;) {
      TomlValue *item = ParseValue(p);
      if (!item || p->error.size > 0) {
        ScratchEnd(scratch);
        return nullptr;
      }
      if (count == capacity) {
        u64 next = capacity == 0 ? 8 : capacity * 2;
        TomlValue **grown = PushArrayNoZero(scratch.arena, TomlValue *, next);
        for (u64 i = 0; i < count; i += 1) grown[i] = items[i];
        items = grown;
        capacity = next;
      }
      items[count] = item;
      count += 1;

      SkipSpacesAndComments(p);
      while (Peek(p) == '\n') {
        Advance(p);
        SkipSpacesAndComments(p);
      }
      if (Peek(p) == ',') {
        Advance(p);
        SkipSpacesAndComments(p);
        while (Peek(p) == '\n') {
          Advance(p);
          SkipSpacesAndComments(p);
        }
        if (Peek(p) == ']') break;
        continue;
      }
      break;
    }
  }

  if (Peek(p) != ']') {
    Fail(p, "expected ']'");
    ScratchEnd(scratch);
    return nullptr;
  }
  Advance(p);

  TomlValue *value = NewValue(p->arena, TomlKind::Array);
  value->array.count = count;
  if (count > 0) {
    value->array.items = PushArrayNoZero(p->arena, TomlValue *, count);
    for (u64 i = 0; i < count; i += 1) value->array.items[i] = items[i];
  }
  ScratchEnd(scratch);
  return value;
}

[[nodiscard]] TomlValue *ParseNumberOrBool(Parser *p) {
  u64 start = p->pos;
  if (Peek(p) == '-' || Peek(p) == '+') Advance(p);
  bool seen_digit = false;
  while (!AtEnd(p) && Peek(p) >= '0' && Peek(p) <= '9') {
    seen_digit = true;
    Advance(p);
  }
  if (Peek(p) == '.') {
    Advance(p);
    while (!AtEnd(p) && Peek(p) >= '0' && Peek(p) <= '9') {
      seen_digit = true;
      Advance(p);
    }
  }
  if (!seen_digit) {
    p->pos = start;
    String8 ident = ParseBareKey(p);
    if (Str8Match(ident, Str8Lit("true"))) {
      TomlValue *value = NewValue(p->arena, TomlKind::Bool);
      value->boolean = true;
      return value;
    }
    if (Str8Match(ident, Str8Lit("false"))) {
      TomlValue *value = NewValue(p->arena, TomlKind::Bool);
      value->boolean = false;
      return value;
    }
    Fail(p, "expected value");
    return nullptr;
  }

  String8 slice = Str8Substr(p->text, RangeU64{start, p->pos});
  const char *cstr = PushCStr(p->arena, slice);
  char *end = nullptr;
  f64 number = strtod(cstr, &end);
  if (end == cstr) {
    Fail(p, "invalid number");
    return nullptr;
  }
  TomlValue *value = NewValue(p->arena, TomlKind::Number);
  value->number = number;
  return value;
}

TomlValue *ParseValue(Parser *p) {
  SkipSpaces(p);
  u8 c = Peek(p);
  if (c == '"') {
    String8 s = ParseQuotedString(p);
    if (p->error.size > 0) return nullptr;
    TomlValue *value = NewValue(p->arena, TomlKind::String);
    value->string = s;
    return value;
  }
  if (c == '[') return ParseArray(p);
  return ParseNumberOrBool(p);
}

[[nodiscard]] bool ParseKeyPath(Parser *p, String8 *parts, u64 *count, u64 max_parts) {
  *count = 0;
  for (;;) {
    String8 part = ParseKey(p);
    if (p->error.size > 0 || part.size == 0) return false;
    if (*count >= max_parts) {
      Fail(p, "key path too deep");
      return false;
    }
    parts[*count] = part;
    *count += 1;
    SkipSpaces(p);
    if (Peek(p) != '.') break;
    Advance(p);
  }
  return true;
}

[[nodiscard]] TomlValue *NavigateOrCreate(Parser *p, TomlValue *root, String8 *parts, u64 count,
                                          bool for_table_header) {
  TomlValue *current = root;
  for (u64 i = 0; i < count; i += 1) {
    bool last = (i + 1 == count);
    if (for_table_header || !last) {
      bool created = false;
      TomlValue *next = EnsureTable(p->arena, current, parts[i], &created);
      if (!next) {
        Fail(p, "key conflicts with non-table value");
        return nullptr;
      }
      current = next;
    } else {
      return current;
    }
  }
  return current;
}

}  // namespace

TomlParseResult TomlParse(Arena *arena, String8 text) {
  TomlParseResult result = {};
  if (!arena) {
    result.error = Str8Lit("missing arena");
    return result;
  }

  Parser p = {};
  p.arena = arena;
  p.text = text;
  p.line = 1;

  TomlValue *root = NewValue(arena, TomlKind::Table);
  TomlValue *current_table = root;

  while (!AtEnd(&p) && p.error.size == 0) {
    SkipSpacesAndComments(&p);
    if (AtEnd(&p)) break;
    if (Peek(&p) == '\n') {
      Advance(&p);
      continue;
    }

    if (Peek(&p) == '[') {
      Advance(&p);
      SkipSpaces(&p);
      String8 parts[16];
      u64 count = 0;
      if (!ParseKeyPath(&p, parts, &count, ArrayCount(parts))) break;
      SkipSpaces(&p);
      if (Peek(&p) != ']') {
        Fail(&p, "expected ']' after table name");
        break;
      }
      Advance(&p);
      current_table = NavigateOrCreate(&p, root, parts, count, true);
      if (!current_table) break;
      SkipSpacesAndComments(&p);
      if (!AtEnd(&p) && Peek(&p) != '\n') {
        Fail(&p, "unexpected trailing characters after table header");
        break;
      }
      continue;
    }

    String8 parts[16];
    u64 count = 0;
    if (!ParseKeyPath(&p, parts, &count, ArrayCount(parts))) break;
    SkipSpaces(&p);
    if (Peek(&p) != '=') {
      Fail(&p, "expected '='");
      break;
    }
    Advance(&p);
    SkipSpaces(&p);

    TomlValue *parent = current_table;
    if (count > 1) {
      parent = NavigateOrCreate(&p, current_table, parts, count - 1, true);
      if (!parent) break;
    }
    TomlValue *value = ParseValue(&p);
    if (!value || p.error.size > 0) break;
    if (!SetMember(arena, parent, parts[count - 1], value)) {
      Fail(&p, "failed to set key");
      break;
    }

    SkipSpacesAndComments(&p);
    if (!AtEnd(&p) && Peek(&p) != '\n') {
      Fail(&p, "unexpected trailing characters");
      break;
    }
  }

  if (p.error.size > 0) {
    result.error = p.error;
    result.error_line = p.line;
    return result;
  }

  result.root = root;
  return result;
}

TomlValue *TomlTableGet(const TomlValue *table, String8 name) {
  if (!table || table->kind != TomlKind::Table) return nullptr;
  for (TomlMember *m = table->table; m; m = m->next) {
    if (Str8Match(m->name, name)) return m->value;
  }
  return nullptr;
}

TomlValue *TomlArrayItem(const TomlValue *value, u64 index) {
  if (!value || value->kind != TomlKind::Array) return nullptr;
  if (index >= value->array.count) return nullptr;
  return value->array.items[index];
}

u64 TomlArrayCount(const TomlValue *value) {
  if (!value || value->kind != TomlKind::Array) return 0;
  return value->array.count;
}

bool TomlGetString(const TomlValue *value, String8 *out) {
  if (!value || value->kind != TomlKind::String) return false;
  if (out) *out = value->string;
  return true;
}

bool TomlGetBool(const TomlValue *value, bool *out) {
  if (!value || value->kind != TomlKind::Bool) return false;
  if (out) *out = value->boolean;
  return true;
}

bool TomlGetF64(const TomlValue *value, f64 *out) {
  if (!value || value->kind != TomlKind::Number) return false;
  if (out) *out = value->number;
  return true;
}

bool TomlGetI64(const TomlValue *value, i64 *out) {
  if (!value || value->kind != TomlKind::Number) return false;
  if (out) *out = (i64)value->number;
  return true;
}
