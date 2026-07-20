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
