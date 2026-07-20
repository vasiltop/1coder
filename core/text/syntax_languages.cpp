#include "text/syntax.h"

#include "base/base_string.h"

// ---------------------------------------------------------------------------
// Keyword tables
// Each table is a plain C array of String8 pointing into the read-only
// string literal segment.  No arena, no allocation.
// ---------------------------------------------------------------------------

// --- C / C++ ----------------------------------------------------------------

static const String8 kCppKeywords[] = {
    Str8LitComp("if"),        Str8LitComp("else"),     Str8LitComp("for"),
    Str8LitComp("while"),     Str8LitComp("do"),        Str8LitComp("switch"),
    Str8LitComp("case"),      Str8LitComp("default"),   Str8LitComp("break"),
    Str8LitComp("continue"),  Str8LitComp("return"),    Str8LitComp("goto"),
    Str8LitComp("class"),     Str8LitComp("struct"),    Str8LitComp("union"),
    Str8LitComp("enum"),      Str8LitComp("namespace"), Str8LitComp("template"),
    Str8LitComp("typename"),  Str8LitComp("using"),     Str8LitComp("typedef"),
    Str8LitComp("public"),    Str8LitComp("protected"), Str8LitComp("private"),
    Str8LitComp("virtual"),   Str8LitComp("override"),  Str8LitComp("final"),
    Str8LitComp("inline"),    Str8LitComp("static"),    Str8LitComp("extern"),
    Str8LitComp("const"),     Str8LitComp("constexpr"), Str8LitComp("consteval"),
    Str8LitComp("constinit"), Str8LitComp("volatile"),  Str8LitComp("mutable"),
    Str8LitComp("new"),       Str8LitComp("delete"),    Str8LitComp("operator"),
    Str8LitComp("try"),       Str8LitComp("catch"),     Str8LitComp("throw"),
    Str8LitComp("noexcept"),  Str8LitComp("explicit"),  Str8LitComp("friend"),
    Str8LitComp("sizeof"),    Str8LitComp("alignof"),   Str8LitComp("decltype"),
    Str8LitComp("auto"),      Str8LitComp("register"),  Str8LitComp("signed"),
    Str8LitComp("unsigned"),
};

static const String8 kCppTypes[] = {
    Str8LitComp("void"),    Str8LitComp("bool"),    Str8LitComp("char"),
    Str8LitComp("wchar_t"), Str8LitComp("char8_t"), Str8LitComp("char16_t"),
    Str8LitComp("char32_t"),Str8LitComp("short"),   Str8LitComp("int"),
    Str8LitComp("long"),    Str8LitComp("float"),   Str8LitComp("double"),
    Str8LitComp("size_t"),  Str8LitComp("ptrdiff_t"),
};

static const String8 kCppConstants[] = {
    Str8LitComp("true"),    Str8LitComp("false"),   Str8LitComp("nullptr"),
    Str8LitComp("NULL"),    Str8LitComp("this"),
};

static const KeywordGroup kCppGroups[] = {
    {kCppKeywords,  ArrayCount(kCppKeywords),  TokenKind::Keyword},
    {kCppTypes,     ArrayCount(kCppTypes),     TokenKind::Type},
    {kCppConstants, ArrayCount(kCppConstants), TokenKind::Constant},
};

static const String8 kCppExtensions[] = {
    Str8LitComp("c"),   Str8LitComp("h"),
    Str8LitComp("cc"),  Str8LitComp("cpp"), Str8LitComp("cxx"),
    Str8LitComp("hh"),  Str8LitComp("hpp"), Str8LitComp("hxx"),
};

// --- JavaScript -------------------------------------------------------------

static const String8 kJsKeywords[] = {
    Str8LitComp("if"),       Str8LitComp("else"),      Str8LitComp("for"),
    Str8LitComp("while"),    Str8LitComp("do"),         Str8LitComp("switch"),
    Str8LitComp("case"),     Str8LitComp("default"),    Str8LitComp("break"),
    Str8LitComp("continue"), Str8LitComp("return"),     Str8LitComp("throw"),
    Str8LitComp("try"),      Str8LitComp("catch"),      Str8LitComp("finally"),
    Str8LitComp("new"),      Str8LitComp("delete"),     Str8LitComp("typeof"),
    Str8LitComp("instanceof"),Str8LitComp("in"),        Str8LitComp("of"),
    Str8LitComp("var"),      Str8LitComp("let"),        Str8LitComp("const"),
    Str8LitComp("function"), Str8LitComp("class"),      Str8LitComp("extends"),
    Str8LitComp("super"),    Str8LitComp("import"),     Str8LitComp("export"),
    Str8LitComp("from"),     Str8LitComp("as"),         Str8LitComp("async"),
    Str8LitComp("await"),    Str8LitComp("yield"),      Str8LitComp("static"),
    Str8LitComp("get"),      Str8LitComp("set"),        Str8LitComp("with"),
    Str8LitComp("debugger"),
};

static const String8 kJsConstants[] = {
    Str8LitComp("true"),      Str8LitComp("false"),  Str8LitComp("null"),
    Str8LitComp("undefined"), Str8LitComp("NaN"),    Str8LitComp("Infinity"),
    Str8LitComp("this"),      Str8LitComp("void"),
};

static const KeywordGroup kJsGroups[] = {
    {kJsKeywords,  ArrayCount(kJsKeywords),  TokenKind::Keyword},
    {kJsConstants, ArrayCount(kJsConstants), TokenKind::Constant},
};

static const String8 kJsExtensions[] = {
    Str8LitComp("js"),  Str8LitComp("jsx"),
    Str8LitComp("mjs"), Str8LitComp("cjs"),
};

// --- TypeScript -------------------------------------------------------------

static const String8 kTsKeywords[] = {
    Str8LitComp("if"),         Str8LitComp("else"),       Str8LitComp("for"),
    Str8LitComp("while"),      Str8LitComp("do"),          Str8LitComp("switch"),
    Str8LitComp("case"),       Str8LitComp("default"),     Str8LitComp("break"),
    Str8LitComp("continue"),   Str8LitComp("return"),      Str8LitComp("throw"),
    Str8LitComp("try"),        Str8LitComp("catch"),       Str8LitComp("finally"),
    Str8LitComp("new"),        Str8LitComp("delete"),      Str8LitComp("typeof"),
    Str8LitComp("instanceof"), Str8LitComp("in"),          Str8LitComp("of"),
    Str8LitComp("var"),        Str8LitComp("let"),         Str8LitComp("const"),
    Str8LitComp("function"),   Str8LitComp("class"),       Str8LitComp("extends"),
    Str8LitComp("implements"), Str8LitComp("interface"),   Str8LitComp("enum"),
    Str8LitComp("type"),       Str8LitComp("namespace"),   Str8LitComp("module"),
    Str8LitComp("declare"),    Str8LitComp("abstract"),    Str8LitComp("override"),
    Str8LitComp("super"),      Str8LitComp("import"),      Str8LitComp("export"),
    Str8LitComp("from"),       Str8LitComp("as"),          Str8LitComp("async"),
    Str8LitComp("await"),      Str8LitComp("yield"),       Str8LitComp("static"),
    Str8LitComp("readonly"),   Str8LitComp("private"),     Str8LitComp("protected"),
    Str8LitComp("public"),     Str8LitComp("get"),         Str8LitComp("set"),
    Str8LitComp("keyof"),      Str8LitComp("infer"),       Str8LitComp("is"),
    Str8LitComp("asserts"),    Str8LitComp("satisfies"),   Str8LitComp("debugger"),
};

static const String8 kTsTypes[] = {
    Str8LitComp("string"),  Str8LitComp("number"),  Str8LitComp("boolean"),
    Str8LitComp("object"),  Str8LitComp("symbol"),  Str8LitComp("bigint"),
    Str8LitComp("never"),   Str8LitComp("unknown"), Str8LitComp("any"),
    Str8LitComp("void"),
};

static const String8 kTsConstants[] = {
    Str8LitComp("true"),      Str8LitComp("false"),  Str8LitComp("null"),
    Str8LitComp("undefined"), Str8LitComp("NaN"),    Str8LitComp("Infinity"),
    Str8LitComp("this"),
};

static const KeywordGroup kTsGroups[] = {
    {kTsKeywords,  ArrayCount(kTsKeywords),  TokenKind::Keyword},
    {kTsTypes,     ArrayCount(kTsTypes),     TokenKind::Type},
    {kTsConstants, ArrayCount(kTsConstants), TokenKind::Constant},
};

static const String8 kTsExtensions[] = {
    Str8LitComp("ts"),  Str8LitComp("tsx"),
    Str8LitComp("mts"), Str8LitComp("cts"),
};

// --- Python -----------------------------------------------------------------

static const String8 kPyKeywords[] = {
    Str8LitComp("if"),      Str8LitComp("elif"),     Str8LitComp("else"),
    Str8LitComp("for"),     Str8LitComp("while"),    Str8LitComp("break"),
    Str8LitComp("continue"),Str8LitComp("return"),   Str8LitComp("yield"),
    Str8LitComp("pass"),    Str8LitComp("raise"),    Str8LitComp("try"),
    Str8LitComp("except"),  Str8LitComp("finally"),  Str8LitComp("with"),
    Str8LitComp("as"),      Str8LitComp("import"),   Str8LitComp("from"),
    Str8LitComp("def"),     Str8LitComp("class"),    Str8LitComp("lambda"),
    Str8LitComp("del"),     Str8LitComp("global"),   Str8LitComp("nonlocal"),
    Str8LitComp("assert"),  Str8LitComp("in"),       Str8LitComp("not"),
    Str8LitComp("and"),     Str8LitComp("or"),       Str8LitComp("is"),
    Str8LitComp("async"),   Str8LitComp("await"),
};

static const String8 kPyTypes[] = {
    Str8LitComp("int"),     Str8LitComp("float"),   Str8LitComp("complex"),
    Str8LitComp("bool"),    Str8LitComp("str"),     Str8LitComp("bytes"),
    Str8LitComp("list"),    Str8LitComp("tuple"),   Str8LitComp("dict"),
    Str8LitComp("set"),     Str8LitComp("frozenset"),Str8LitComp("type"),
    Str8LitComp("object"),
};

static const String8 kPyConstants[] = {
    Str8LitComp("True"),    Str8LitComp("False"),   Str8LitComp("None"),
    Str8LitComp("self"),    Str8LitComp("cls"),     Str8LitComp("__name__"),
    Str8LitComp("__file__"),
};

static const KeywordGroup kPyGroups[] = {
    {kPyKeywords,  ArrayCount(kPyKeywords),  TokenKind::Keyword},
    {kPyTypes,     ArrayCount(kPyTypes),     TokenKind::Type},
    {kPyConstants, ArrayCount(kPyConstants), TokenKind::Constant},
};

static const String8 kPyExtensions[] = {
    Str8LitComp("py"), Str8LitComp("pyi"),
};

// --- Rust -------------------------------------------------------------------

static const String8 kRustKeywords[] = {
    Str8LitComp("if"),       Str8LitComp("else"),     Str8LitComp("for"),
    Str8LitComp("while"),    Str8LitComp("loop"),     Str8LitComp("break"),
    Str8LitComp("continue"), Str8LitComp("return"),   Str8LitComp("match"),
    Str8LitComp("fn"),       Str8LitComp("let"),      Str8LitComp("mut"),
    Str8LitComp("const"),    Str8LitComp("static"),   Str8LitComp("struct"),
    Str8LitComp("enum"),     Str8LitComp("union"),    Str8LitComp("trait"),
    Str8LitComp("impl"),     Str8LitComp("type"),     Str8LitComp("where"),
    Str8LitComp("mod"),      Str8LitComp("use"),      Str8LitComp("pub"),
    Str8LitComp("crate"),    Str8LitComp("super"),    Str8LitComp("self"),
    Str8LitComp("Self"),     Str8LitComp("async"),    Str8LitComp("await"),
    Str8LitComp("move"),     Str8LitComp("ref"),      Str8LitComp("in"),
    Str8LitComp("dyn"),      Str8LitComp("box"),      Str8LitComp("unsafe"),
    Str8LitComp("extern"),   Str8LitComp("as"),       Str8LitComp("yield"),
};

static const String8 kRustTypes[] = {
    Str8LitComp("i8"),    Str8LitComp("i16"),   Str8LitComp("i32"),
    Str8LitComp("i64"),   Str8LitComp("i128"),  Str8LitComp("isize"),
    Str8LitComp("u8"),    Str8LitComp("u16"),   Str8LitComp("u32"),
    Str8LitComp("u64"),   Str8LitComp("u128"),  Str8LitComp("usize"),
    Str8LitComp("f32"),   Str8LitComp("f64"),   Str8LitComp("bool"),
    Str8LitComp("char"),  Str8LitComp("str"),
};

static const String8 kRustConstants[] = {
    Str8LitComp("true"),    Str8LitComp("false"),   Str8LitComp("None"),
    Str8LitComp("Some"),    Str8LitComp("Ok"),       Str8LitComp("Err"),
};

static const KeywordGroup kRustGroups[] = {
    {kRustKeywords,  ArrayCount(kRustKeywords),  TokenKind::Keyword},
    {kRustTypes,     ArrayCount(kRustTypes),     TokenKind::Type},
    {kRustConstants, ArrayCount(kRustConstants), TokenKind::Constant},
};

static const String8 kRustExtensions[] = {
    Str8LitComp("rs"),
};

// --- Go ---------------------------------------------------------------------

static const String8 kGoKeywords[] = {
    Str8LitComp("if"),      Str8LitComp("else"),    Str8LitComp("for"),
    Str8LitComp("range"),   Str8LitComp("break"),   Str8LitComp("continue"),
    Str8LitComp("return"),  Str8LitComp("goto"),    Str8LitComp("switch"),
    Str8LitComp("case"),    Str8LitComp("default"), Str8LitComp("fallthrough"),
    Str8LitComp("select"),  Str8LitComp("func"),    Str8LitComp("var"),
    Str8LitComp("const"),   Str8LitComp("type"),    Str8LitComp("struct"),
    Str8LitComp("interface"),Str8LitComp("map"),    Str8LitComp("chan"),
    Str8LitComp("package"), Str8LitComp("import"),  Str8LitComp("go"),
    Str8LitComp("defer"),
};

static const String8 kGoTypes[] = {
    Str8LitComp("bool"),     Str8LitComp("byte"),    Str8LitComp("rune"),
    Str8LitComp("string"),   Str8LitComp("error"),
    Str8LitComp("int"),      Str8LitComp("int8"),    Str8LitComp("int16"),
    Str8LitComp("int32"),    Str8LitComp("int64"),
    Str8LitComp("uint"),     Str8LitComp("uint8"),   Str8LitComp("uint16"),
    Str8LitComp("uint32"),   Str8LitComp("uint64"),  Str8LitComp("uintptr"),
    Str8LitComp("float32"),  Str8LitComp("float64"),
    Str8LitComp("complex64"),Str8LitComp("complex128"),
};

static const String8 kGoConstants[] = {
    Str8LitComp("true"),  Str8LitComp("false"), Str8LitComp("nil"),
    Str8LitComp("iota"),
};

static const KeywordGroup kGoGroups[] = {
    {kGoKeywords,  ArrayCount(kGoKeywords),  TokenKind::Keyword},
    {kGoTypes,     ArrayCount(kGoTypes),     TokenKind::Type},
    {kGoConstants, ArrayCount(kGoConstants), TokenKind::Constant},
};

static const String8 kGoExtensions[] = {
    Str8LitComp("go"),
};

// --- TOML -------------------------------------------------------------------

static const String8 kTomlKeywords[] = {
    Str8LitComp("true"),  Str8LitComp("false"),
};

static const KeywordGroup kTomlGroups[] = {
    {kTomlKeywords, ArrayCount(kTomlKeywords), TokenKind::Constant},
};

static const String8 kTomlExtensions[] = {
    Str8LitComp("toml"),
};

// --- JSON -------------------------------------------------------------------

static const String8 kJsonConstants[] = {
    Str8LitComp("true"),  Str8LitComp("false"),  Str8LitComp("null"),
};

static const KeywordGroup kJsonGroups[] = {
    {kJsonConstants, ArrayCount(kJsonConstants), TokenKind::Constant},
};

static const String8 kJsonExtensions[] = {
    Str8LitComp("json"),
};

// --- Fallback ---------------------------------------------------------------

static const String8 kFallbackKeywords[] = {
    Str8LitComp("if"),    Str8LitComp("else"),   Str8LitComp("for"),
    Str8LitComp("while"), Str8LitComp("return"), Str8LitComp("break"),
    Str8LitComp("continue"),
};

static const String8 kFallbackConstants[] = {
    Str8LitComp("true"),  Str8LitComp("false"),  Str8LitComp("null"),
    Str8LitComp("nil"),
};

static const KeywordGroup kFallbackGroups[] = {
    {kFallbackKeywords,  ArrayCount(kFallbackKeywords),  TokenKind::Keyword},
    {kFallbackConstants, ArrayCount(kFallbackConstants), TokenKind::Constant},
};

// ---------------------------------------------------------------------------
// Language table
// ---------------------------------------------------------------------------

static const LanguageDefinition kLanguages[] = {
    // Fallback (index 0)
    {
        LanguageId::Fallback,
        Str8LitComp("Plain"),
        nullptr, 0,
        kFallbackGroups, ArrayCount(kFallbackGroups),
        {Str8LitComp("//"), Str8LitComp("#")}, 2,
        Str8LitComp(""), Str8LitComp(""),
        false, false, false, false,
    },
    // C / C++
    {
        LanguageId::Cpp,
        Str8LitComp("C/C++"),
        kCppExtensions, ArrayCount(kCppExtensions),
        kCppGroups, ArrayCount(kCppGroups),
        {Str8LitComp("//"), Str8LitComp("")}, 1,
        Str8LitComp("/*"), Str8LitComp("*/"),
        true, false, false, true,
    },
    // JavaScript
    {
        LanguageId::JavaScript,
        Str8LitComp("JavaScript"),
        kJsExtensions, ArrayCount(kJsExtensions),
        kJsGroups, ArrayCount(kJsGroups),
        {Str8LitComp("//"), Str8LitComp("")}, 1,
        Str8LitComp("/*"), Str8LitComp("*/"),
        false, false, true, false,
    },
    // TypeScript
    {
        LanguageId::TypeScript,
        Str8LitComp("TypeScript"),
        kTsExtensions, ArrayCount(kTsExtensions),
        kTsGroups, ArrayCount(kTsGroups),
        {Str8LitComp("//"), Str8LitComp("")}, 1,
        Str8LitComp("/*"), Str8LitComp("*/"),
        false, false, true, false,
    },
    // Python
    {
        LanguageId::Python,
        Str8LitComp("Python"),
        kPyExtensions, ArrayCount(kPyExtensions),
        kPyGroups, ArrayCount(kPyGroups),
        {Str8LitComp("#"), Str8LitComp("")}, 1,
        Str8LitComp(""), Str8LitComp(""),
        false, true, false, false,
    },
    // Rust
    {
        LanguageId::Rust,
        Str8LitComp("Rust"),
        kRustExtensions, ArrayCount(kRustExtensions),
        kRustGroups, ArrayCount(kRustGroups),
        {Str8LitComp("//"), Str8LitComp("")}, 1,
        Str8LitComp("/*"), Str8LitComp("*/"),
        true, false, false, false,
    },
    // Go
    {
        LanguageId::Go,
        Str8LitComp("Go"),
        kGoExtensions, ArrayCount(kGoExtensions),
        kGoGroups, ArrayCount(kGoGroups),
        {Str8LitComp("//"), Str8LitComp("")}, 1,
        Str8LitComp("/*"), Str8LitComp("*/"),
        false, false, true, false,
    },
    // TOML
    {
        LanguageId::Toml,
        Str8LitComp("TOML"),
        kTomlExtensions, ArrayCount(kTomlExtensions),
        kTomlGroups, ArrayCount(kTomlGroups),
        {Str8LitComp("#"), Str8LitComp("")}, 1,
        Str8LitComp(""), Str8LitComp(""),
        false, false, false, false,
    },
    // JSON
    {
        LanguageId::Json,
        Str8LitComp("JSON"),
        kJsonExtensions, ArrayCount(kJsonExtensions),
        kJsonGroups, ArrayCount(kJsonGroups),
        {Str8LitComp(""), Str8LitComp("")}, 0,
        Str8LitComp(""), Str8LitComp(""),
        false, false, false, false,
    },
};

// ---------------------------------------------------------------------------

const LanguageDefinition *SyntaxLanguageForPath(String8 path) {
  String8 ext = Str8PathExt(path);
  if (ext.size == 0) {
    return &kLanguages[0];  // fallback
  }

  for (u64 lang = 1; lang < ArrayCount(kLanguages); lang += 1) {
    const LanguageDefinition *def = &kLanguages[lang];
    for (u64 e = 0; e < def->extension_count; e += 1) {
      if (Str8Match(ext, def->extensions[e], StringMatch::CaseInsensitive)) {
        return def;
      }
    }
  }

  return &kLanguages[0];  // unknown extension -> fallback
}
