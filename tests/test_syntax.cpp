#include "editor/buffer_registry.h"
#include "test.h"
#include "text/syntax.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const LanguageDefinition *Lang(const char *path) {
  return SyntaxLanguageForPath(Str8C(path));
}

// ---------------------------------------------------------------------------
// C / C++ extensions
// ---------------------------------------------------------------------------

TEST(syntax_cpp_c) { CHECK_EQ((int)Lang("foo.c")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_h) { CHECK_EQ((int)Lang("foo.h")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_cc) { CHECK_EQ((int)Lang("foo.cc")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_cpp) { CHECK_EQ((int)Lang("foo.cpp")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_cxx) { CHECK_EQ((int)Lang("foo.cxx")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_hh) { CHECK_EQ((int)Lang("foo.hh")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_hpp) { CHECK_EQ((int)Lang("foo.hpp")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_hxx) { CHECK_EQ((int)Lang("foo.hxx")->id, (int)LanguageId::Cpp); }

// Upper-case variants
TEST(syntax_cpp_upper_C) { CHECK_EQ((int)Lang("foo.C")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_upper_CPP) { CHECK_EQ((int)Lang("foo.CPP")->id, (int)LanguageId::Cpp); }
TEST(syntax_cpp_upper_HPP) { CHECK_EQ((int)Lang("foo.HPP")->id, (int)LanguageId::Cpp); }

// C/C++ flags
TEST(syntax_cpp_flags) {
  const LanguageDefinition *d = Lang("main.cpp");
  CHECK(d->preprocessor_directives);
  CHECK(d->single_quote_is_character);
  CHECK(!d->triple_quoted_strings);
  CHECK(!d->backtick_strings);
  CHECK_EQ(d->line_comment_count, (u64)1);
  CHECK(Str8Match(d->line_comments[0], Str8Lit("//")));
  CHECK(Str8Match(d->block_comment_open, Str8Lit("/*")));
  CHECK(Str8Match(d->block_comment_close, Str8Lit("*/")));
}

// ---------------------------------------------------------------------------
// JavaScript extensions
// ---------------------------------------------------------------------------

TEST(syntax_js_js) { CHECK_EQ((int)Lang("foo.js")->id, (int)LanguageId::JavaScript); }
TEST(syntax_js_jsx) { CHECK_EQ((int)Lang("foo.jsx")->id, (int)LanguageId::JavaScript); }
TEST(syntax_js_mjs) { CHECK_EQ((int)Lang("foo.mjs")->id, (int)LanguageId::JavaScript); }
TEST(syntax_js_cjs) { CHECK_EQ((int)Lang("foo.cjs")->id, (int)LanguageId::JavaScript); }
TEST(syntax_js_upper_JS) { CHECK_EQ((int)Lang("foo.JS")->id, (int)LanguageId::JavaScript); }

TEST(syntax_js_flags) {
  const LanguageDefinition *d = Lang("app.js");
  CHECK(!d->preprocessor_directives);
  CHECK(!d->single_quote_is_character);
  CHECK(!d->triple_quoted_strings);
  CHECK(d->backtick_strings);
  CHECK_EQ(d->line_comment_count, (u64)1);
  CHECK(Str8Match(d->line_comments[0], Str8Lit("//")));
}

// ---------------------------------------------------------------------------
// TypeScript extensions
// ---------------------------------------------------------------------------

TEST(syntax_ts_ts) { CHECK_EQ((int)Lang("foo.ts")->id, (int)LanguageId::TypeScript); }
TEST(syntax_ts_tsx) { CHECK_EQ((int)Lang("foo.tsx")->id, (int)LanguageId::TypeScript); }
TEST(syntax_ts_mts) { CHECK_EQ((int)Lang("foo.mts")->id, (int)LanguageId::TypeScript); }
TEST(syntax_ts_cts) { CHECK_EQ((int)Lang("foo.cts")->id, (int)LanguageId::TypeScript); }
TEST(syntax_ts_upper_TS) { CHECK_EQ((int)Lang("foo.TS")->id, (int)LanguageId::TypeScript); }

TEST(syntax_ts_flags) {
  const LanguageDefinition *d = Lang("app.ts");
  CHECK(d->backtick_strings);
  CHECK(!d->single_quote_is_character);
}

// ---------------------------------------------------------------------------
// Python extensions
// ---------------------------------------------------------------------------

TEST(syntax_py_py) { CHECK_EQ((int)Lang("foo.py")->id, (int)LanguageId::Python); }
TEST(syntax_py_pyi) { CHECK_EQ((int)Lang("foo.pyi")->id, (int)LanguageId::Python); }
TEST(syntax_py_upper_PY) { CHECK_EQ((int)Lang("foo.PY")->id, (int)LanguageId::Python); }

TEST(syntax_py_flags) {
  const LanguageDefinition *d = Lang("script.py");
  CHECK(d->triple_quoted_strings);
  CHECK(!d->backtick_strings);
  CHECK(!d->single_quote_is_character);
  CHECK(!d->preprocessor_directives);
  CHECK_EQ(d->line_comment_count, (u64)1);
  CHECK(Str8Match(d->line_comments[0], Str8Lit("#")));
  CHECK_EQ(d->block_comment_open.size, (u64)0);
}

// ---------------------------------------------------------------------------
// Rust extensions
// ---------------------------------------------------------------------------

TEST(syntax_rust_rs) { CHECK_EQ((int)Lang("foo.rs")->id, (int)LanguageId::Rust); }
TEST(syntax_rust_upper_RS) { CHECK_EQ((int)Lang("foo.RS")->id, (int)LanguageId::Rust); }

TEST(syntax_rust_flags) {
  const LanguageDefinition *d = Lang("lib.rs");
  CHECK(d->single_quote_is_character);
  CHECK(!d->backtick_strings);
  CHECK(!d->preprocessor_directives);
}

// ---------------------------------------------------------------------------
// Go extensions
// ---------------------------------------------------------------------------

TEST(syntax_go_go) { CHECK_EQ((int)Lang("foo.go")->id, (int)LanguageId::Go); }
TEST(syntax_go_upper_GO) { CHECK_EQ((int)Lang("foo.GO")->id, (int)LanguageId::Go); }

TEST(syntax_go_flags) {
  const LanguageDefinition *d = Lang("main.go");
  CHECK(d->backtick_strings);
  CHECK(!d->single_quote_is_character);
  CHECK(!d->preprocessor_directives);
}

// ---------------------------------------------------------------------------
// TOML extensions
// ---------------------------------------------------------------------------

TEST(syntax_toml_toml) { CHECK_EQ((int)Lang("foo.toml")->id, (int)LanguageId::Toml); }
TEST(syntax_toml_upper_TOML) { CHECK_EQ((int)Lang("foo.TOML")->id, (int)LanguageId::Toml); }

TEST(syntax_toml_flags) {
  const LanguageDefinition *d = Lang("Cargo.toml");
  CHECK_EQ(d->line_comment_count, (u64)1);
  CHECK(Str8Match(d->line_comments[0], Str8Lit("#")));
  CHECK_EQ(d->block_comment_open.size, (u64)0);
}

// ---------------------------------------------------------------------------
// JSON extensions
// ---------------------------------------------------------------------------

TEST(syntax_json_json) { CHECK_EQ((int)Lang("foo.json")->id, (int)LanguageId::Json); }
TEST(syntax_json_upper_JSON) { CHECK_EQ((int)Lang("foo.JSON")->id, (int)LanguageId::Json); }

TEST(syntax_json_flags) {
  const LanguageDefinition *d = Lang("package.json");
  CHECK_EQ(d->line_comment_count, (u64)0);
  CHECK_EQ(d->block_comment_open.size, (u64)0);
}

// ---------------------------------------------------------------------------
// Fallback: extensionless, unknown extension, hidden file
// ---------------------------------------------------------------------------

TEST(syntax_fallback_no_extension) {
  CHECK_EQ((int)Lang("Makefile")->id, (int)LanguageId::Fallback);
}

TEST(syntax_fallback_empty_path) {
  CHECK_EQ((int)Lang("")->id, (int)LanguageId::Fallback);
}

TEST(syntax_fallback_unknown_ext) {
  CHECK_EQ((int)Lang("foo.xyz")->id, (int)LanguageId::Fallback);
}

TEST(syntax_fallback_hidden_file) {
  // .bashrc: leading dot is a hidden file marker, not an extension.
  CHECK_EQ((int)Lang(".bashrc")->id, (int)LanguageId::Fallback);
}

TEST(syntax_fallback_flags) {
  const LanguageDefinition *d = Lang("Makefile");
  CHECK_EQ((int)d->id, (int)LanguageId::Fallback);
  // Fallback supports both // and # line comments.
  CHECK_EQ(d->line_comment_count, (u64)2);
  CHECK_EQ(d->block_comment_open.size, (u64)0);
}

// ---------------------------------------------------------------------------
// Keyword group structure sanity
// ---------------------------------------------------------------------------

TEST(syntax_cpp_has_keyword_groups) {
  const LanguageDefinition *d = Lang("main.cpp");
  CHECK(d->keyword_group_count > 0);
  // First group must be keywords.
  CHECK_EQ((int)d->keyword_groups[0].kind, (int)TokenKind::Keyword);
  CHECK(d->keyword_groups[0].count > 0);
}

TEST(syntax_py_has_type_group) {
  const LanguageDefinition *d = Lang("script.py");
  bool found_type = false;
  for (u64 i = 0; i < d->keyword_group_count; i += 1) {
    if (d->keyword_groups[i].kind == TokenKind::Type) {
      found_type = true;
    }
  }
  CHECK(found_type);
}

// Path with directory component still resolves correctly.
TEST(syntax_path_with_dir) {
  CHECK_EQ((int)Lang("src/main.rs")->id, (int)LanguageId::Rust);
  CHECK_EQ((int)Lang("a/b/c/file.py")->id, (int)LanguageId::Python);
}

// ---------------------------------------------------------------------------
// Full-buffer lexer (Task 2)
//
// Every expected offset below is computed from the source text itself via
// Str8FindFirst/Str8FindFirstChar rather than hand-counted, so a change to a
// fixture string can never silently desynchronise from its assertions.
// ---------------------------------------------------------------------------

namespace {

struct Fixture {
  Arena *arena;
  BufferRegistry reg;
};

Fixture MakeFixture() {
  Fixture f = {};
  f.arena = ArenaAlloc(MB(16));
  BufferRegistryInit(&f.reg, f.arena, 16);
  return f;
}

void Destroy(Fixture *f) {
  BufferRegistryDestroy(&f->reg);
  ArenaRelease(f->arena);
}

// Opens a scratch buffer, loads `text`, and attaches syntax highlighting as
// if it lived at `path` -- so extension-based language selection runs the
// same way it would for a real file.
Buffer *OpenAttached(Fixture *f, const char *path, const char *text) {
  BufferHandle handle = BufferOpen(&f->reg, BufferKind::Scratch, Str8Lit("test"));
  Buffer *buffer = BufferFromHandle(&f->reg, handle);
  BufferSetText(nullptr, buffer, Str8C(text));
  SyntaxAttach(buffer, Str8C(path));
  return buffer;
}

// Byte offset of `needle` within `text`. Fails the calling test loudly (via
// an out-of-range sentinel that every subsequent CHECK_EQ will catch) rather
// than silently testing against 0 when a fixture string is edited and a
// needle no longer appears.
u64 Off(const char *text, const char *needle) {
  return Str8FindFirst(Str8C(text), Str8C(needle));
}

u64 LineEndOf(const char *text, u64 from) {
  u64 nl = Str8FindFirstChar(Str8C(text), '\n', from);
  return nl;
}

// The single token starting exactly at `start`, or nullptr when no token
// does -- e.g. because the byte at `start` was left Default.
const Token *TokenAt(const Buffer *buffer, u64 start) {
  for (u64 i = 0; i < buffer->tokens.count; i += 1) {
    if (buffer->tokens.tokens[i].start == start) return &buffer->tokens.tokens[i];
  }
  return nullptr;
}

}  // namespace

// --- C/C++: preprocessor, type, function, keyword, numbers, comments -------

TEST(syntax_scan_cpp_preprocessor_directive) {
  Fixture f = MakeFixture();
  const char *text = "#include <stdio.h>\nint main() {}\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 start = Off(text, "#include");
  u64 end = LineEndOf(text, start);
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Preprocessor);
  CHECK_EQ(tok->end, end);

  Destroy(&f);
}

TEST(syntax_scan_cpp_type_function_keyword_operator_punctuation) {
  Fixture f = MakeFixture();
  const char *text = "int add(int a, int b) {\n  return a + b;\n}\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  // "int" (the return type) is a Type token.
  u64 int_start = Off(text, "int");
  const Token *int_tok = TokenAt(buffer, int_start);
  CHECK(int_tok != nullptr);
  CHECK_EQ((int)int_tok->kind, (int)TokenKind::Type);
  CHECK_EQ(int_tok->end, int_start + 3);

  // "add" is a non-keyword identifier immediately followed by '(', so it is
  // classified as a function call/definition site.
  u64 add_start = Off(text, "add");
  const Token *add_tok = TokenAt(buffer, add_start);
  CHECK(add_tok != nullptr);
  CHECK_EQ((int)add_tok->kind, (int)TokenKind::Function);
  CHECK_EQ(add_tok->end, add_start + 3);

  // "return" is a keyword.
  u64 return_start = Off(text, "return");
  const Token *return_tok = TokenAt(buffer, return_start);
  CHECK(return_tok != nullptr);
  CHECK_EQ((int)return_tok->kind, (int)TokenKind::Keyword);
  CHECK_EQ(return_tok->end, return_start + 6);

  // "a" and "b" are plain identifiers: not keywords, not followed by '(', so
  // they get no token at all (Default).
  u64 a_in_return = Off(text, "a + b");
  CHECK(TokenAt(buffer, a_in_return) == nullptr);

  // '+' is an operator.
  u64 plus = Off(text, "+");
  const Token *plus_tok = TokenAt(buffer, plus);
  CHECK(plus_tok != nullptr);
  CHECK_EQ((int)plus_tok->kind, (int)TokenKind::Operator);
  CHECK_EQ(plus_tok->end, plus + 1);

  // '(' ')' '{' '}' ',' ';' are punctuation.
  u64 paren = Off(text, "(");
  const Token *paren_tok = TokenAt(buffer, paren);
  CHECK(paren_tok != nullptr);
  CHECK_EQ((int)paren_tok->kind, (int)TokenKind::Punctuation);

  u64 comma = Off(text, ",");
  const Token *comma_tok = TokenAt(buffer, comma);
  CHECK(comma_tok != nullptr);
  CHECK_EQ((int)comma_tok->kind, (int)TokenKind::Punctuation);

  u64 semi = Off(text, ";");
  const Token *semi_tok = TokenAt(buffer, semi);
  CHECK(semi_tok != nullptr);
  CHECK_EQ((int)semi_tok->kind, (int)TokenKind::Punctuation);

  Destroy(&f);
}

TEST(syntax_scan_cpp_block_and_line_comments) {
  Fixture f = MakeFixture();
  const char *text = "int a; /* block */ // line\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 block_start = Off(text, "/*");
  u64 block_close_end = Off(text, "*/") + 2;
  const Token *block_tok = TokenAt(buffer, block_start);
  CHECK(block_tok != nullptr);
  CHECK_EQ((int)block_tok->kind, (int)TokenKind::Comment);
  CHECK_EQ(block_tok->end, block_close_end);

  u64 line_start = Off(text, "// line");
  u64 line_end = LineEndOf(text, line_start);
  const Token *line_tok = TokenAt(buffer, line_start);
  CHECK(line_tok != nullptr);
  CHECK_EQ((int)line_tok->kind, (int)TokenKind::Comment);
  CHECK_EQ(line_tok->end, line_end);

  Destroy(&f);
}

// --- Keyword boundary: substrings of keywords are not falsely classified ---

TEST(syntax_scan_keyword_boundary_identifier) {
  Fixture f = MakeFixture();
  // "returning" contains "return" as a prefix but is a distinct identifier;
  // it must not be highlighted as the keyword.
  const char *text = "int returning = 1;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 ident_start = Off(text, "returning");
  CHECK(TokenAt(buffer, ident_start) == nullptr);

  Destroy(&f);
}

TEST(syntax_scan_keyword_inside_string_stays_string) {
  Fixture f = MakeFixture();
  const char *text = "const char *s = \"return\";\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 quote_start = Off(text, "\"return\"");
  const Token *tok = TokenAt(buffer, quote_start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::String);
  CHECK_EQ(tok->end, quote_start + 8);  // "return" plus both quotes

  Destroy(&f);
}

TEST(syntax_scan_keyword_inside_comment_stays_comment) {
  Fixture f = MakeFixture();
  const char *text = "// return this\nint x;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 comment_start = Off(text, "// return this");
  u64 comment_end = LineEndOf(text, comment_start);
  const Token *tok = TokenAt(buffer, comment_start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Comment);
  CHECK_EQ(tok->end, comment_end);

  Destroy(&f);
}

// --- Numbers: decimal, hex, binary, octal, float, exponent -----------------

TEST(syntax_scan_number_decimal) {
  Fixture f = MakeFixture();
  const char *text = "int x = 42;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 start = Off(text, "42");
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Number);
  CHECK_EQ(tok->end, start + 2);

  Destroy(&f);
}

TEST(syntax_scan_number_hex) {
  Fixture f = MakeFixture();
  const char *text = "int x = 0x1F;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 start = Off(text, "0x1F");
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Number);
  CHECK_EQ(tok->end, start + 4);

  Destroy(&f);
}

TEST(syntax_scan_number_binary) {
  Fixture f = MakeFixture();
  const char *text = "int x = 0b1010;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 start = Off(text, "0b1010");
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Number);
  CHECK_EQ(tok->end, start + 6);

  Destroy(&f);
}

TEST(syntax_scan_number_octal) {
  Fixture f = MakeFixture();
  const char *text = "int x = 0o17;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 start = Off(text, "0o17");
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Number);
  CHECK_EQ(tok->end, start + 4);

  Destroy(&f);
}

TEST(syntax_scan_number_float_exponent_and_underscores) {
  Fixture f = MakeFixture();
  const char *text = "double x = 6.02e23;\ndouble y = 1_000.5;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 exp_start = Off(text, "6.02e23");
  const Token *exp_tok = TokenAt(buffer, exp_start);
  CHECK(exp_tok != nullptr);
  CHECK_EQ((int)exp_tok->kind, (int)TokenKind::Number);
  CHECK_EQ(exp_tok->end, exp_start + 7);

  u64 underscore_start = Off(text, "1_000.5");
  const Token *underscore_tok = TokenAt(buffer, underscore_start);
  CHECK(underscore_tok != nullptr);
  CHECK_EQ((int)underscore_tok->kind, (int)TokenKind::Number);
  CHECK_EQ(underscore_tok->end, underscore_start + 7);

  Destroy(&f);
}

// --- Escaped quotes and character literals ----------------------------------

TEST(syntax_scan_string_with_escaped_quote) {
  Fixture f = MakeFixture();
  const char *text = "const char *s = \"a\\\"b\";\n";  // "a\"b"
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 start = Off(text, "\"a\\\"b\"");
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::String);
  // The escaped quote must not end the string early: "a\"b" is 6 bytes.
  CHECK_EQ(tok->end, start + 6);

  Destroy(&f);
}

TEST(syntax_scan_character_literal) {
  Fixture f = MakeFixture();
  const char *text = "char c = '\\'';\n";  // '\''
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 start = Off(text, "'\\''");
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Character);
  CHECK_EQ(tok->end, start + 4);

  Destroy(&f);
}

// --- JSON has no comments; TOML uses '#'; fallback shares keywords ----------

TEST(syntax_scan_json_does_not_treat_slashes_as_comment) {
  Fixture f = MakeFixture();
  const char *text = "{\"a\": 1} // not a comment\n";
  Buffer *buffer = OpenAttached(&f, "data.json", text);

  u64 slashes = Off(text, "//");
  const Token *tok = TokenAt(buffer, slashes);
  // JSON has no configured line comment, so a bare "//" is not swallowed as
  // one -- whatever it becomes, it must not be Comment.
  if (tok != nullptr) {
    CHECK(tok->kind != TokenKind::Comment);
  }

  // The JSON constant is still classified normally, proving the language
  // table (not just the comment rule) took effect.
  u64 one = Off(text, "1}");
  const Token *num_tok = TokenAt(buffer, one);
  CHECK(num_tok != nullptr);
  CHECK_EQ((int)num_tok->kind, (int)TokenKind::Number);

  Destroy(&f);
}

TEST(syntax_scan_json_constant) {
  Fixture f = MakeFixture();
  const char *text = "{\"ok\": true}\n";
  Buffer *buffer = OpenAttached(&f, "data.json", text);

  u64 start = Off(text, "true");
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Constant);

  Destroy(&f);
}

TEST(syntax_scan_toml_hash_comment) {
  Fixture f = MakeFixture();
  const char *text = "name = \"x\" # a comment\n";
  Buffer *buffer = OpenAttached(&f, "Cargo.toml", text);

  u64 start = Off(text, "# a comment");
  u64 end = LineEndOf(text, start);
  const Token *tok = TokenAt(buffer, start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Comment);
  CHECK_EQ(tok->end, end);

  Destroy(&f);
}

TEST(syntax_scan_fallback_shared_keywords_and_both_comment_styles) {
  Fixture f = MakeFixture();
  const char *text = "if true\n// c-style\n# shell-style\n";
  Buffer *buffer = OpenAttached(&f, "Makefile", text);

  u64 if_start = Off(text, "if");
  const Token *if_tok = TokenAt(buffer, if_start);
  CHECK(if_tok != nullptr);
  CHECK_EQ((int)if_tok->kind, (int)TokenKind::Keyword);

  u64 true_start = Off(text, "true");
  const Token *true_tok = TokenAt(buffer, true_start);
  CHECK(true_tok != nullptr);
  CHECK_EQ((int)true_tok->kind, (int)TokenKind::Constant);

  u64 slash_start = Off(text, "// c-style");
  u64 slash_end = LineEndOf(text, slash_start);
  const Token *slash_tok = TokenAt(buffer, slash_start);
  CHECK(slash_tok != nullptr);
  CHECK_EQ((int)slash_tok->kind, (int)TokenKind::Comment);
  CHECK_EQ(slash_tok->end, slash_end);

  u64 hash_start = Off(text, "# shell-style");
  const Token *hash_tok = TokenAt(buffer, hash_start);
  CHECK(hash_tok != nullptr);
  CHECK_EQ((int)hash_tok->kind, (int)TokenKind::Comment);

  Destroy(&f);
}

// --- Multiline state carried across lines, and its line-cache bookkeeping --

TEST(syntax_scan_python_triple_quote_state_across_lines) {
  Fixture f = MakeFixture();
  const char *text = "x = '''line one\nline two'''\ny = 1\n";
  Buffer *buffer = OpenAttached(&f, "script.py", text);

  CHECK_EQ(BufferLineCount(buffer), (u64)4);  // trailing '\n' yields an empty 4th line
  CHECK_EQ(buffer->syntax.line_count, (u64)4);

  const SyntaxLineCache *l0 = &buffer->syntax.lines[0];
  const SyntaxLineCache *l1 = &buffer->syntax.lines[1];
  const SyntaxLineCache *l2 = &buffer->syntax.lines[2];

  CHECK_EQ((int)l0->incoming.mode, (int)SyntaxMode::Default);
  CHECK_EQ((int)l0->outgoing.mode, (int)SyntaxMode::TripleSingleString);

  CHECK_EQ((int)l1->incoming.mode, (int)SyntaxMode::TripleSingleString);
  CHECK_EQ((int)l1->outgoing.mode, (int)SyntaxMode::Default);

  CHECK_EQ((int)l2->incoming.mode, (int)SyntaxMode::Default);
  CHECK_EQ((int)l2->outgoing.mode, (int)SyntaxMode::Default);

  // Both lines contributed a String token covering their triple-quote text.
  u64 line0_start = Off(text, "'''line one");
  const Token *tok0 = TokenAt(buffer, line0_start);
  CHECK(tok0 != nullptr);
  CHECK_EQ((int)tok0->kind, (int)TokenKind::String);
  CHECK_EQ(tok0->end, LineEndOf(text, line0_start));

  u64 line1_start = Off(text, "line two'''");
  const Token *tok1 = TokenAt(buffer, line1_start);
  CHECK(tok1 != nullptr);
  CHECK_EQ((int)tok1->kind, (int)TokenKind::String);
  CHECK_EQ(tok1->end, line1_start + Str8C("line two'''").size);

  Destroy(&f);
}

TEST(syntax_scan_c_block_comment_state_across_lines) {
  Fixture f = MakeFixture();
  const char *text = "int a; /* start\nmiddle\nend */ int b;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  const SyntaxLineCache *l0 = &buffer->syntax.lines[0];
  const SyntaxLineCache *l1 = &buffer->syntax.lines[1];
  const SyntaxLineCache *l2 = &buffer->syntax.lines[2];

  CHECK_EQ((int)l0->incoming.mode, (int)SyntaxMode::Default);
  CHECK_EQ((int)l0->outgoing.mode, (int)SyntaxMode::BlockComment);
  CHECK_EQ((int)l1->incoming.mode, (int)SyntaxMode::BlockComment);
  CHECK_EQ((int)l1->outgoing.mode, (int)SyntaxMode::BlockComment);
  CHECK_EQ((int)l2->incoming.mode, (int)SyntaxMode::BlockComment);
  CHECK_EQ((int)l2->outgoing.mode, (int)SyntaxMode::Default);

  // The middle line is entirely comment.
  u64 middle_start = Off(text, "middle");
  const Token *middle_tok = TokenAt(buffer, middle_start);
  CHECK(middle_tok != nullptr);
  CHECK_EQ((int)middle_tok->kind, (int)TokenKind::Comment);
  CHECK_EQ(middle_tok->end, LineEndOf(text, middle_start));

  // After the closing "*/" on line 2, ordinary scanning resumes: "int" is a
  // Type token again.
  u64 second_int = Str8FindFirst(Str8C(text), Str8C("int"), Off(text, "*/"));
  const Token *second_int_tok = TokenAt(buffer, second_int);
  CHECK(second_int_tok != nullptr);
  CHECK_EQ((int)second_int_tok->kind, (int)TokenKind::Type);

  Destroy(&f);
}

TEST(syntax_scan_js_backtick_state_across_lines) {
  Fixture f = MakeFixture();
  const char *text = "let s = `line1\nline2`;\n";
  Buffer *buffer = OpenAttached(&f, "app.js", text);

  const SyntaxLineCache *l0 = &buffer->syntax.lines[0];
  const SyntaxLineCache *l1 = &buffer->syntax.lines[1];

  CHECK_EQ((int)l0->outgoing.mode, (int)SyntaxMode::BacktickString);
  CHECK_EQ((int)l1->incoming.mode, (int)SyntaxMode::BacktickString);
  CHECK_EQ((int)l1->outgoing.mode, (int)SyntaxMode::Default);

  u64 line1_start = Off(text, "line2`;");
  const Token *tok = TokenAt(buffer, line1_start);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::String);
  // Token covers "line2`" (up to and including the closing backtick), not
  // the trailing ';'.
  CHECK_EQ(tok->end, line1_start + Str8C("line2`").size);

  // The trailing ';' after the closing backtick is ordinary punctuation.
  u64 semi = line1_start + Str8C("line2`").size;
  const Token *semi_tok = TokenAt(buffer, semi);
  CHECK(semi_tok != nullptr);
  CHECK_EQ((int)semi_tok->kind, (int)TokenKind::Punctuation);

  Destroy(&f);
}

// --- Empty buffer: zero tokens, one line-cache entry ------------------------

TEST(syntax_scan_empty_buffer) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenAttached(&f, "main.cpp", "");

  CHECK_EQ(BufferLineCount(buffer), (u64)1);
  CHECK_EQ(buffer->tokens.count, (u64)0);
  CHECK_EQ(buffer->syntax.line_count, (u64)1);

  const SyntaxLineCache *l0 = &buffer->syntax.lines[0];
  CHECK_EQ((int)l0->incoming.mode, (int)SyntaxMode::Default);
  CHECK_EQ((int)l0->outgoing.mode, (int)SyntaxMode::Default);
  CHECK_EQ(l0->first_token, (u64)0);
  CHECK_EQ(l0->token_count, (u64)0);

  Destroy(&f);
}

// --- UTF-8 prefix proves offsets are byte-based, not codepoint-based -------

TEST(syntax_scan_utf8_prefix_byte_offsets) {
  Fixture f = MakeFixture();
  // "café" (with a 2-byte UTF-8 'é') inside a comment on line 0, then a
  // keyword on line 1. If offsets were codepoint-based rather than
  // byte-based, the keyword's reported position would be off by one.
  const char *text = "// caf\xC3\xA9\nif (x) {}\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 comment_start = Off(text, "//");
  u64 comment_end = LineEndOf(text, comment_start);
  const Token *comment_tok = TokenAt(buffer, comment_start);
  CHECK(comment_tok != nullptr);
  CHECK_EQ((int)comment_tok->kind, (int)TokenKind::Comment);
  CHECK_EQ(comment_tok->end, comment_end);

  u64 if_start = Off(text, "if");
  const Token *if_tok = TokenAt(buffer, if_start);
  CHECK(if_tok != nullptr);
  CHECK_EQ((int)if_tok->kind, (int)TokenKind::Keyword);
  CHECK_EQ(if_tok->end, if_start + 2);

  Destroy(&f);
}

// --- BufferSetText on an already-attached buffer rebuilds tokens -----------

TEST(syntax_scan_buffer_set_text_rebuilds_tokens) {
  Fixture f = MakeFixture();
  const char *first = "int a;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", first);

  u64 first_a = Off(first, "a");
  CHECK(TokenAt(buffer, Off(first, "int")) != nullptr);
  (void)first_a;

  const char *second = "return 1;\nfloat g;\n";
  BufferSetText(nullptr, buffer, Str8C(second));

  // Stale tokens from the first text must be gone: line count and token
  // layout now reflect `second`.
  CHECK_EQ(BufferLineCount(buffer), (u64)3);
  CHECK_EQ(buffer->syntax.line_count, (u64)3);

  u64 return_start = Off(second, "return");
  const Token *return_tok = TokenAt(buffer, return_start);
  CHECK(return_tok != nullptr);
  CHECK_EQ((int)return_tok->kind, (int)TokenKind::Keyword);

  u64 float_start = Off(second, "float");
  const Token *float_tok = TokenAt(buffer, float_start);
  CHECK(float_tok != nullptr);
  CHECK_EQ((int)float_tok->kind, (int)TokenKind::Type);

  Destroy(&f);
}

// --- Reattaching the same resolved language does not corrupt the cache -----

TEST(syntax_scan_reattach_same_language_is_stable) {
  Fixture f = MakeFixture();
  const char *text = "int a;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 before_count = buffer->tokens.count;
  SyntaxAttach(buffer, Str8C("main.cpp"));  // same resolved language

  CHECK_EQ(buffer->tokens.count, before_count);
  const Token *tok = TokenAt(buffer, Off(text, "int"));
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Type);

  Destroy(&f);
}

// --- Regression: token growth mid-rebuild must not lose earlier tokens -----
//
// EnsureTokenCapacity used to copy `buffer->tokens.count` old entries when
// growing, but during SyntaxRebuild the live count lives in a local counter
// threaded through ScanLine/AppendToken -- buffer->tokens.count stays 0 until
// the whole rebuild finishes. Any growth past the initial 256-token capacity
// therefore "copied" zero old tokens, silently corrupting everything below
// the new capacity's old boundary with uninitialized memory. This fixture
// forces multiple growths (256 -> 512) and checks tokens at the very start,
// straddling the old boundary, and at the very end are all intact.
TEST(syntax_scan_growth_preserves_earlier_tokens) {
  Fixture f = MakeFixture();

  // Each repeat of "x=1;" yields exactly 3 tokens (Operator, Number,
  // Punctuation); "x" itself stays Default/untokenized. 100 repeats is 300
  // tokens, comfortably past the 256-token initial capacity.
  constexpr int kRepeats = 100;
  constexpr int kUnitLen = 4;
  char text[kRepeats * kUnitLen + 1];
  for (int r = 0; r < kRepeats; r += 1) {
    text[r * kUnitLen + 0] = 'x';
    text[r * kUnitLen + 1] = '=';
    text[r * kUnitLen + 2] = '1';
    text[r * kUnitLen + 3] = ';';
  }
  text[kRepeats * kUnitLen] = '\0';

  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  CHECK_EQ(buffer->tokens.count, (u64)(kRepeats * 3));
  CHECK(buffer->syntax.token_capacity >= buffer->tokens.count);

  // Sorted, non-overlapping, non-empty across the entire array -- exactly
  // the invariant that silently broke when growth clobbered earlier tokens
  // with uninitialized entries.
  for (u64 i = 0; i < buffer->tokens.count; i += 1) {
    CHECK(buffer->tokens.tokens[i].end > buffer->tokens.tokens[i].start);
    if (i + 1 < buffer->tokens.count) {
      CHECK(buffer->tokens.tokens[i].end <= buffer->tokens.tokens[i + 1].start);
    }
  }

  auto CheckUnit = [&](int unit) {
    u64 base = (u64)unit * 3;
    u64 unit_start = (u64)unit * kUnitLen;
    const Token &eq = buffer->tokens.tokens[base + 0];
    const Token &num = buffer->tokens.tokens[base + 1];
    const Token &semi = buffer->tokens.tokens[base + 2];

    CHECK_EQ(eq.start, unit_start + 1);
    CHECK_EQ(eq.end, unit_start + 2);
    CHECK_EQ((int)eq.kind, (int)TokenKind::Operator);

    CHECK_EQ(num.start, unit_start + 2);
    CHECK_EQ(num.end, unit_start + 3);
    CHECK_EQ((int)num.kind, (int)TokenKind::Number);

    CHECK_EQ(semi.start, unit_start + 3);
    CHECK_EQ(semi.end, unit_start + 4);
    CHECK_EQ((int)semi.kind, (int)TokenKind::Punctuation);
  };

  CheckUnit(0);               // Early: well inside the original capacity.
  CheckUnit(kRepeats / 2);    // Middle: straddles the old 256-token boundary.
  CheckUnit(kRepeats - 1);    // Late: only reachable after growth.

  Destroy(&f);
}

// --- Regression: preprocessor check must not fire mid-line after a comment
// closes -----------------------------------------------------------------
//
// The C/C++ line-leading '#' check used to run from wherever `i` had reached
// after handling a carried-over multiline construct, without confirming `i`
// was still at the true line start. A block comment opened on a previous
// line and closed mid-line, followed by whitespace and '#', used to be
// misclassified as a whole-line Preprocessor directive even though the '#'
// isn't actually line-leading.
TEST(syntax_scan_preprocessor_not_triggered_after_comment_closes_mid_line) {
  Fixture f = MakeFixture();
  const char *text = "/* start\nend */ #define X 1\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  for (u64 i = 0; i < buffer->tokens.count; i += 1) {
    CHECK((int)buffer->tokens.tokens[i].kind != (int)TokenKind::Preprocessor);
  }

  u64 hash_off = Off(text, "#define");
  CHECK(TokenAt(buffer, hash_off) == nullptr);

  // Sanity: the carried-over comment still correctly closes on line 1.
  u64 close_start = Off(text, "end */");
  const Token *comment_tok = TokenAt(buffer, close_start);
  CHECK(comment_tok != nullptr);
  CHECK_EQ((int)comment_tok->kind, (int)TokenKind::Comment);
  CHECK_EQ(comment_tok->end, close_start + Str8Lit("end */").size);

  Destroy(&f);
}

// A genuine, fresh-line preprocessor directive with leading indentation
// (no carried-over construct) must still be recognised -- the mid-line fix
// must not over-correct and break ordinary indented directives.
TEST(syntax_scan_preprocessor_still_works_with_leading_whitespace) {
  Fixture f = MakeFixture();
  const char *text = "int a;\n  #define X 1\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 hash_off = Off(text, "#define");
  const Token *tok = TokenAt(buffer, hash_off);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Preprocessor);
  CHECK_EQ(tok->end, LineEndOf(text, hash_off));

  Destroy(&f);
}

// ===========================================================================
// Incremental syntax update (Task 3)
//
// These tests verify that BufferReplace, undo, and redo call the incremental
// update path and produce correct tokens without a full rebuild.
// ===========================================================================

// 1. Replace "/*" with spaces: following comment tokens disappear and later
//    keywords recover.
TEST(syntax_incremental_remove_block_comment_opener) {
  Fixture f = MakeFixture();
  const char *text = "int a; /* comment\nstuff */ int b;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  // Before edit: "stuff */" on line 1 is inside a block comment.
  CHECK_EQ((int)buffer->syntax.lines[1].incoming.mode, (int)SyntaxMode::BlockComment);

  // Replace "/*" with "  " (same length, removes comment opener).
  u64 slash_star = Off(text, "/*");
  BufferReplace(nullptr, buffer, RangeU64{slash_star, slash_star + 2}, Str8Lit("  "), 0, 0);

  // After edit: line 1 should no longer be in BlockComment state.
  CHECK_EQ((int)buffer->syntax.lines[1].incoming.mode, (int)SyntaxMode::Default);

  // "int" on line 1 (after "*/") should now be a Type token because the
  // comment is gone -- "stuff */ int b;" is now regular code.
  // Find "int b" in the post-edit buffer.
  TempArena scratch = ScratchBegin();
  String8 all = BufferTextAll(scratch.arena, buffer);
  u64 int_b = Str8FindFirst(all, Str8Lit("int b"));
  const Token *tok = TokenAt(buffer, int_b);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Type);
  ScratchEnd(scratch);

  Destroy(&f);
}

// 2. Insert a block-comment opener so state propagates through later lines;
//    then insert the closer and verify state returns to Default.
TEST(syntax_incremental_insert_block_comment_propagates) {
  Fixture f = MakeFixture();
  const char *text = "int a;\nint b;\nint c;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  // All lines start and end in Default mode.
  CHECK_EQ((int)buffer->syntax.lines[0].outgoing.mode, (int)SyntaxMode::Default);
  CHECK_EQ((int)buffer->syntax.lines[1].incoming.mode, (int)SyntaxMode::Default);
  CHECK_EQ((int)buffer->syntax.lines[2].incoming.mode, (int)SyntaxMode::Default);

  // Insert "/*" at end of line 0 (before the newline).
  u64 first_newline = Str8FindFirstChar(Str8C(text), '\n');
  BufferReplace(nullptr, buffer, RangeU64{first_newline, first_newline}, Str8Lit("/*"), 0, 0);

  // Now line 0 ends in BlockComment, and that propagates.
  CHECK_EQ((int)buffer->syntax.lines[0].outgoing.mode, (int)SyntaxMode::BlockComment);
  CHECK_EQ((int)buffer->syntax.lines[1].incoming.mode, (int)SyntaxMode::BlockComment);
  CHECK_EQ((int)buffer->syntax.lines[2].incoming.mode, (int)SyntaxMode::BlockComment);

  // "int b" on line 1 should now be Comment, not Type.
  TempArena scratch = ScratchBegin();
  String8 all = BufferTextAll(scratch.arena, buffer);
  u64 int_b = Str8FindFirst(all, Str8Lit("int b"));
  const Token *tok = TokenAt(buffer, int_b);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Comment);
  ScratchEnd(scratch);

  // Now insert "*/" at start of line 2 to close the comment.
  u64 line2_start = BufferOffsetFromLine(buffer, 2);
  BufferReplace(nullptr, buffer, RangeU64{line2_start, line2_start}, Str8Lit("*/"), 0, 0);

  // Line 2 should now start in BlockComment (inheriting from line 1) but the
  // close is at the beginning so it exits immediately.
  CHECK_EQ((int)buffer->syntax.lines[2].incoming.mode, (int)SyntaxMode::BlockComment);
  CHECK_EQ((int)buffer->syntax.lines[2].outgoing.mode, (int)SyntaxMode::Default);

  // Line 3 (the 4th line if we count from 0) is now Default.
  if (buffer->syntax.line_count > 3) {
    CHECK_EQ((int)buffer->syntax.lines[3].incoming.mode, (int)SyntaxMode::Default);
  }

  Destroy(&f);
}

// 3. Insert newlines: line cache count aligns with BufferLineCount, suffix
//    tokens shift by exact byte delta, prefix tokens unchanged.
TEST(syntax_incremental_insert_newlines) {
  Fixture f = MakeFixture();
  const char *text = "int a;\nint b;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  u64 orig_line_count = BufferLineCount(buffer);
  CHECK_EQ(buffer->syntax.line_count, orig_line_count);

  // Remember token for "int" on line 1 (at start of "int b;").
  u64 int_b_off = Off(text, "int b");
  const Token *before_tok = TokenAt(buffer, int_b_off);
  CHECK(before_tok != nullptr);
  CHECK_EQ((int)before_tok->kind, (int)TokenKind::Type);
  u64 before_int_a_start = Off(text, "int a");

  // Insert a newline at the start of line 1.
  u64 line1_start = BufferOffsetFromLine(buffer, 1);
  BufferReplace(nullptr, buffer, RangeU64{line1_start, line1_start}, Str8Lit("\n"), 0, 0);

  // Line count increased by 1.
  CHECK_EQ(BufferLineCount(buffer), orig_line_count + 1);
  CHECK_EQ(buffer->syntax.line_count, BufferLineCount(buffer));

  // "int a" token on line 0 is unaffected (prefix).
  const Token *int_a_tok = TokenAt(buffer, before_int_a_start);
  CHECK(int_a_tok != nullptr);
  CHECK_EQ((int)int_a_tok->kind, (int)TokenKind::Type);

  // "int b" token shifted by 1 byte (the inserted newline).
  const Token *shifted_tok = TokenAt(buffer, int_b_off + 1);
  CHECK(shifted_tok != nullptr);
  CHECK_EQ((int)shifted_tok->kind, (int)TokenKind::Type);

  Destroy(&f);
}

// 4. Delete lines: cache realigns and token indexes/ranges remain valid.
TEST(syntax_incremental_delete_lines) {
  Fixture f = MakeFixture();
  const char *text = "int a;\nint b;\nint c;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  CHECK_EQ(BufferLineCount(buffer), (u64)4);
  CHECK_EQ(buffer->syntax.line_count, (u64)4);

  // Delete line 1 entirely ("int b;\n").
  u64 line1_start = BufferOffsetFromLine(buffer, 1);
  u64 line2_start = BufferOffsetFromLine(buffer, 2);
  BufferReplace(nullptr, buffer, RangeU64{line1_start, line2_start}, String8{nullptr, 0}, 0, 0);

  CHECK_EQ(BufferLineCount(buffer), (u64)3);
  CHECK_EQ(buffer->syntax.line_count, (u64)3);

  // "int c" should still be correctly tokenized (now on line 1).
  TempArena scratch = ScratchBegin();
  String8 all = BufferTextAll(scratch.arena, buffer);
  u64 int_c = Str8FindFirst(all, Str8Lit("int c"));
  const Token *tok = TokenAt(buffer, int_c);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Type);
  ScratchEnd(scratch);

  // All tokens are in bounds and sorted.
  u64 buf_size = BufferSize(buffer);
  for (u64 i = 0; i < buffer->tokens.count; i += 1) {
    CHECK(buffer->tokens.tokens[i].start < buffer->tokens.tokens[i].end);
    CHECK(buffer->tokens.tokens[i].end <= buf_size);
    if (i + 1 < buffer->tokens.count) {
      CHECK(buffer->tokens.tokens[i].end <= buffer->tokens.tokens[i + 1].start);
    }
  }

  Destroy(&f);
}

// 5. Edit near the top of a long buffer with no state change: convergence.
TEST(syntax_incremental_convergence_stops_early) {
  Fixture f = MakeFixture();

  // Build a 200-line buffer: line 0 is editable, lines 1-199 are all "int x;\n"
  constexpr int kLines = 200;
  char text[kLines * 8 + 16];
  int pos = 0;
  pos += snprintf(text + pos, sizeof(text) - pos, "int a;\n");
  for (int i = 1; i < kLines; i += 1) {
    pos += snprintf(text + pos, sizeof(text) - pos, "int x;\n");
  }

  Buffer *buffer = OpenAttached(&f, "main.cpp", text);
  CHECK_EQ(BufferLineCount(buffer), (u64)(kLines + 1));  // trailing newline -> empty last line

  // Edit only line 0: replace "a" with "z" (no state change).
  u64 a_pos = 4;  // "int " is 4 bytes, "a" is at 4
  BufferReplace(nullptr, buffer, RangeU64{a_pos, a_pos + 1}, Str8Lit("z"), 0, 0);

  // lines_scanned_last_update should be much smaller than total line count.
  CHECK(buffer->syntax.lines_scanned_last_update < (u64)kLines);
  // It should only need to rescan line 0 and then converge (maybe line 1 too).
  CHECK(buffer->syntax.lines_scanned_last_update <= 3);

  Destroy(&f);
}

// 6. Edit that changes multiline state scans beyond the changed line only
//    until state stabilizes.
TEST(syntax_incremental_state_change_scans_until_stable) {
  Fixture f = MakeFixture();
  // Line 0: "/*", Line 1: "a", Line 2: "*/", Line 3: "int x;\n"
  const char *text = "/*\na\n*/\nint x;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  CHECK_EQ((int)buffer->syntax.lines[0].outgoing.mode, (int)SyntaxMode::BlockComment);
  CHECK_EQ((int)buffer->syntax.lines[2].outgoing.mode, (int)SyntaxMode::Default);
  CHECK_EQ((int)buffer->syntax.lines[3].incoming.mode, (int)SyntaxMode::Default);

  // Delete "/*" from line 0, making all lines start in Default.
  BufferReplace(nullptr, buffer, RangeU64{0, 2}, String8{nullptr, 0}, 0, 0);

  // All lines should now be Default mode.
  for (u64 i = 0; i < buffer->syntax.line_count; i += 1) {
    CHECK_EQ((int)buffer->syntax.lines[i].incoming.mode, (int)SyntaxMode::Default);
  }

  // The update scanned multiple lines (at least lines 0-3) because the state
  // changed propagated, but less than or equal to the total line count.
  CHECK(buffer->syntax.lines_scanned_last_update <= buffer->syntax.line_count);
  CHECK(buffer->syntax.lines_scanned_last_update >= 2);

  Destroy(&f);
}

// 7. UTF-8 before/inside the edit preserves byte offsets.
TEST(syntax_incremental_utf8_preserves_offsets) {
  Fixture f = MakeFixture();
  // "café" uses 2-byte é (0xC3 0xA9), then a keyword on the next line.
  const char *text = "// caf\xC3\xA9\nint x;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  // "int" token should be at byte offset 9 (comment is "// café\n" = 9 bytes).
  u64 int_off = Off(text, "int");
  const Token *tok = TokenAt(buffer, int_off);
  CHECK(tok != nullptr);
  CHECK_EQ((int)tok->kind, (int)TokenKind::Type);

  // Replace "x" with "yy" on line 1 (after the UTF-8 line).
  u64 x_off = Off(text, "x;");
  BufferReplace(nullptr, buffer, RangeU64{x_off, x_off + 1}, Str8Lit("yy"), 0, 0);

  // "int" is still at byte offset `int_off` (unchanged by edit after it on same line).
  const Token *tok2 = TokenAt(buffer, int_off);
  CHECK(tok2 != nullptr);
  CHECK_EQ((int)tok2->kind, (int)TokenKind::Type);

  // The edit was on line 1, so line 0 comment token is also preserved.
  u64 comment_off = 0;  // "//" starts at byte 0
  const Token *comment_tok = TokenAt(buffer, comment_off);
  CHECK(comment_tok != nullptr);
  CHECK_EQ((int)comment_tok->kind, (int)TokenKind::Comment);

  Destroy(&f);
}

// 8. Undo and redo restore the exact token arrays and line states.
TEST(syntax_incremental_undo_redo_restores_tokens) {
  Fixture f = MakeFixture();
  const char *text = "int a;\nint b;\n";
  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  // Snapshot tokens before edit.
  u64 token_count_before = buffer->tokens.count;
  TempArena scratch = ScratchBegin();
  Token *tokens_before = PushArrayNoZero(scratch.arena, Token, token_count_before);
  for (u64 i = 0; i < token_count_before; i += 1) {
    tokens_before[i] = buffer->tokens.tokens[i];
  }

  // Edit: replace "int a" with "float a".
  u64 int_a = Off(text, "int a");
  BufferReplace(nullptr, buffer, RangeU64{int_a, int_a + 3}, Str8Lit("float"), 0, 0);

  // Verify the edit changed tokens.
  CHECK(buffer->tokens.count != token_count_before ||
        buffer->tokens.tokens[0].end != tokens_before[0].end);

  // Undo.
  bool moved = false;
  (void)BufferUndo(nullptr, buffer, &moved);
  CHECK(moved);

  // After undo, tokens must match the original exactly.
  CHECK_EQ(buffer->tokens.count, token_count_before);
  for (u64 i = 0; i < token_count_before; i += 1) {
    CHECK_EQ(buffer->tokens.tokens[i].start, tokens_before[i].start);
    CHECK_EQ(buffer->tokens.tokens[i].end, tokens_before[i].end);
    CHECK_EQ((int)buffer->tokens.tokens[i].kind, (int)tokens_before[i].kind);
  }

  // Redo should produce the same result as the original edit.
  (void)BufferRedo(nullptr, buffer, &moved);
  CHECK(moved);

  // After redo, "float" should be a Type token.
  const Token *float_tok = TokenAt(buffer, int_a);
  CHECK(float_tok != nullptr);
  CHECK_EQ((int)float_tok->kind, (int)TokenKind::Type);
  CHECK_EQ(float_tok->end, int_a + 5);

  ScratchEnd(scratch);
  Destroy(&f);
}

// 9. Existing buffer_hooks_fire_on_edit still observes exactly one callback
//    per direct edit/undo/redo -- the incremental syntax update must not
//    interfere with hook counts.
TEST(syntax_incremental_hooks_still_fire_once) {
  Fixture f = MakeFixture();
  Buffer *buffer = OpenAttached(&f, "main.cpp", "int a;\nint b;\n");

  static u32 edit_count;
  edit_count = 0;

  buffer->hooks.on_edit = [](Editor *, Buffer *, RangeU64, u64) {
    edit_count += 1;
  };

  // Direct edit.
  BufferReplace(nullptr, buffer, RangeU64{0, 3}, Str8Lit("float"), 0, 0);
  CHECK_EQ(edit_count, 1);

  // Undo.
  bool moved = false;
  (void)BufferUndo(nullptr, buffer, &moved);
  CHECK_EQ(edit_count, 2);

  // Redo.
  (void)BufferRedo(nullptr, buffer, &moved);
  CHECK_EQ(edit_count, 3);

  Destroy(&f);
}

// 10. Save-as language reselection: when the path extension changes, syntax
//     rebuilds with the new language. When it stays the same, no rebuild.
TEST(syntax_incremental_save_as_language_reselection) {
  Fixture f = MakeFixture();
  const char *text = "let x = 1;\n";
  Buffer *buffer = OpenAttached(&f, "app.js", text);
  buffer->path = PushStr8Copy(f.arena, Str8Lit("app.js"));

  // Currently JavaScript: "let" is a keyword.
  const Token *let_tok = TokenAt(buffer, 0);
  CHECK(let_tok != nullptr);
  CHECK_EQ((int)let_tok->kind, (int)TokenKind::Keyword);

  // Simulate save-as to a .cpp file: language changes, syntax must rebuild.
  const LanguageDefinition *old_lang = buffer->syntax.language;
  SyntaxAttach(buffer, Str8Lit("output.cpp"));
  buffer->path = PushStr8Copy(f.arena, Str8Lit("output.cpp"));

  // Language changed: "let" is not a C++ keyword, so it should not be
  // highlighted as one.
  CHECK(buffer->syntax.language != old_lang);

  // Verify tokens reflect C++ language now -- "let" is just a plain identifier
  // in C++ (no keyword group contains it), so TokenAt returns nullptr.
  const Token *let_tok2 = TokenAt(buffer, 0);
  CHECK(let_tok2 == nullptr);

  // Save-as to same language is a no-op.
  u64 token_count_before = buffer->tokens.count;
  SyntaxAttach(buffer, Str8Lit("other.cpp"));
  CHECK_EQ(buffer->tokens.count, token_count_before);

  Destroy(&f);
}

// ---------------------------------------------------------------------------
// Regression: incremental token-array growth must preserve suffix tokens.
//
// EnsureTokenCapacity during SyntaxEndEdit used to copy only
// `prefix_token_end` entries, leaving the retained suffix (read from the old
// array indices) pointing into uninitialized memory after a reallocation.
// This test forces a 256->512 growth by building a buffer whose total token
// count is just under 256, then performing a small edit on line 0 that
// converges immediately -- retaining a large suffix.  After the edit the
// suffix tokens must be sorted, non-empty, in-bounds, and shifted by the
// exact byte delta.
// ---------------------------------------------------------------------------
TEST(syntax_incremental_growth_preserves_suffix) {
  Fixture f = MakeFixture();

  // Each "x=1;" yields 3 tokens.  80 repetitions = 240 tokens, just under 256.
  // We put them on 80 separate lines so convergence kicks in after line 0.
  constexpr int kLines = 80;
  constexpr int kUnitLen = 5;  // "x=1;\n"
  char text[kLines * kUnitLen + 1];
  for (int i = 0; i < kLines; i += 1) {
    text[i * kUnitLen + 0] = 'x';
    text[i * kUnitLen + 1] = '=';
    text[i * kUnitLen + 2] = '1';
    text[i * kUnitLen + 3] = ';';
    text[i * kUnitLen + 4] = '\n';
  }
  text[kLines * kUnitLen] = '\0';

  Buffer *buffer = OpenAttached(&f, "main.cpp", text);

  // Precondition: total tokens should be 240, capacity 256 (initial).
  CHECK_EQ(buffer->tokens.count, (u64)(kLines * 3));
  CHECK_EQ(buffer->syntax.token_capacity, (u64)256);

  // Snapshot a suffix token (line 1's first token) for comparison.
  u64 suffix_first_tok_idx = buffer->syntax.lines[1].first_token;
  Token suffix_sample = buffer->tokens.tokens[suffix_first_tok_idx];

  // Replace the first "x=1;" (bytes 0-4) with many tokens to force growth.
  // "1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1;" produces 20 numbers +
  // 19 operators + 1 punctuation = 40 tokens from line 0 alone.
  // Total = 40 + 237 (lines 1-79) = 277 > 256 -> growth to 512.
  const char *big_insert = "1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1;";
  String8 big_str = Str8C(big_insert);
  u64 big_len = big_str.size;
  BufferReplace(nullptr, buffer, RangeU64{0, 4}, big_str, 0, 0);

  // The capacity must have grown past 256.
  CHECK(buffer->syntax.token_capacity > 256);

  // Convergence: only line 0 needed rescanning (no state change).
  CHECK(buffer->syntax.lines_scanned_last_update <= 2);

  // All tokens must be sorted, non-empty, in-bounds.
  u64 buf_size = BufferSize(buffer);
  for (u64 i = 0; i < buffer->tokens.count; i += 1) {
    CHECK(buffer->tokens.tokens[i].start < buffer->tokens.tokens[i].end);
    CHECK(buffer->tokens.tokens[i].end <= buf_size);
    if (i + 1 < buffer->tokens.count) {
      CHECK(buffer->tokens.tokens[i].end <= buffer->tokens.tokens[i + 1].start);
    }
  }

  // Suffix tokens (line 1 onward) must be shifted by the byte delta.
  i64 byte_delta = (i64)big_len - 4;  // inserted big_len bytes, removed 4
  u64 new_suffix_first_tok = buffer->syntax.lines[1].first_token;
  Token shifted_sample = buffer->tokens.tokens[new_suffix_first_tok];
  CHECK_EQ(shifted_sample.start, (u64)((i64)suffix_sample.start + byte_delta));
  CHECK_EQ(shifted_sample.end, (u64)((i64)suffix_sample.end + byte_delta));
  CHECK_EQ((int)shifted_sample.kind, (int)suffix_sample.kind);

  Destroy(&f);
}
