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
