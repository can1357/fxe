// fxe::highlight — built-in tree-sitter highlight queries + tokenizer.
//
// Coverage policy: every grammar baked into fxe_treesitter is recognized
// (i.e. tokenize() never returns std::nullopt for a bundled language). The
// ones we actively highlight ship a hand-written query below; the rest
// fall back to a tiny default that tags `(comment)` nodes — when even
// that fails to compile against the running grammar version, tokenize()
// returns an empty token list (language is still reported), so the caller
// renders the source unstyled instead of silently dropping it.
//
// Capture-name conflict resolution: tree-sitter queries can match the same
// byte range with multiple captures. We resolve by per-byte priority where
// the smallest matching span wins — that is, the most specific capture for
// each byte. Adjacent bytes that end up with the same capture name are
// coalesced into one token.

#include <fxe/highlight.hpp>
#include <fxe/treesitter.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fxe::highlight {
  namespace ts = fxe::treesitter;

  namespace {

    // ----- per-language queries -------------------------------------------
    // Capture names are tree-sitter conventional: comment, string, number,
    // constant, keyword, type, function, property, tag, attribute, operator,
    // variable. The renderer maps these to theme colors.

    constexpr std::string_view kTypescriptHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (template_string) @string
      (number) @number
      (regex) @string
      (true) @constant
      (false) @constant
      (null) @constant
      [
        "const" "let" "var" "function" "return" "if" "else" "for" "while" "do"
        "switch" "case" "break" "continue" "default" "import" "export" "from"
        "as" "class" "extends" "implements" "new" "async" "await" "yield"
        "try" "catch" "finally" "throw" "in" "of" "typeof" "instanceof"
        "void" "delete" "type" "interface" "enum" "namespace" "declare"
        "readonly" "public" "private" "protected" "static" "abstract"
      ] @keyword
      (type_identifier) @type
      (predefined_type) @type
      (call_expression
        function: (identifier) @function)
      (function_declaration
        name: (identifier) @function)
      (method_definition
        name: (property_identifier) @function)
      (call_expression
        function: (member_expression
          property: (property_identifier) @function))
      (property_identifier) @property
    )QUERY";

    constexpr std::string_view kTsxHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (template_string) @string
      (number) @number
      (regex) @string
      (true) @constant
      (false) @constant
      (null) @constant
      [
        "const" "let" "var" "function" "return" "if" "else" "for" "while" "do"
        "switch" "case" "break" "continue" "default" "import" "export" "from"
        "as" "class" "extends" "implements" "new" "async" "await" "yield"
        "try" "catch" "finally" "throw" "in" "of" "typeof" "instanceof"
        "void" "delete" "type" "interface" "enum" "namespace" "declare"
        "readonly" "public" "private" "protected" "static" "abstract"
      ] @keyword
      (type_identifier) @type
      (predefined_type) @type
      (call_expression
        function: (identifier) @function)
      (function_declaration
        name: (identifier) @function)
      (method_definition
        name: (property_identifier) @function)
      (call_expression
        function: (member_expression
          property: (property_identifier) @function))
      (property_identifier) @property
      (jsx_opening_element name: (identifier) @tag)
      (jsx_closing_element name: (identifier) @tag)
      (jsx_self_closing_element name: (identifier) @tag)
      (jsx_attribute (property_identifier) @attribute)
    )QUERY";

    constexpr std::string_view kJavascriptHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (template_string) @string
      (number) @number
      (regex) @string
      (true) @constant
      (false) @constant
      (null) @constant
      (undefined) @constant
      [
        "const" "let" "var" "function" "return" "if" "else" "for" "while" "do"
        "switch" "case" "break" "continue" "default" "import" "export" "from"
        "as" "class" "extends" "new" "async" "await" "yield"
        "try" "catch" "finally" "throw" "in" "of" "typeof" "instanceof"
        "void" "delete" "static"
      ] @keyword
      (call_expression function: (identifier) @function)
      (function_declaration name: (identifier) @function)
      (property_identifier) @property
    )QUERY";

    constexpr std::string_view kJsonHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (number) @number
      (true) @constant
      (false) @constant
      (null) @constant
      (pair key: (string) @property)
    )QUERY";

    constexpr std::string_view kBashHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (raw_string) @string
      (number) @number
      (variable_name) @variable
      [
        "if" "then" "else" "elif" "fi" "for" "in" "do" "done" "while"
        "case" "esac" "function" "return" "exit" "local" "export" "readonly"
      ] @keyword
      (command_name (word) @function)
    )QUERY";

    constexpr std::string_view kPythonHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (integer) @number
      (float) @number
      (true) @constant
      (false) @constant
      (none) @constant
      [
        "def" "class" "return" "if" "elif" "else" "for" "while" "in" "not"
        "and" "or" "is" "import" "from" "as" "pass" "break" "continue"
        "try" "except" "finally" "raise" "with" "yield" "lambda" "global"
        "nonlocal" "async" "await"
      ] @keyword
      (function_definition name: (identifier) @function)
      (class_definition name: (identifier) @type)
      (call function: (identifier) @function)
      (decorator (identifier) @function)
    )QUERY";

    constexpr std::string_view kRustHighlights = R"QUERY(
      (line_comment) @comment
      (block_comment) @comment
      (string_literal) @string
      (raw_string_literal) @string
      (char_literal) @string
      (integer_literal) @number
      (float_literal) @number
      (boolean_literal) @constant
      [
        "fn" "let" "mut" "const" "static" "if" "else" "match" "for" "while"
        "loop" "break" "continue" "return" "in" "where" "as" "impl" "trait"
        "struct" "enum" "union" "type" "mod" "use" "pub" "crate" "self"
        "super" "ref" "move" "async" "await" "dyn" "unsafe" "extern"
      ] @keyword
      (function_item name: (identifier) @function)
      (call_expression function: (identifier) @function)
      (type_identifier) @type
      (primitive_type) @type
      (field_identifier) @property
    )QUERY";

    constexpr std::string_view kGoHighlights = R"QUERY(
      (comment) @comment
      (interpreted_string_literal) @string
      (raw_string_literal) @string
      (rune_literal) @string
      (int_literal) @number
      (float_literal) @number
      (true) @constant
      (false) @constant
      (nil) @constant
      [
        "func" "var" "const" "type" "package" "import" "return" "if" "else"
        "for" "range" "switch" "case" "default" "break" "continue" "go"
        "defer" "select" "chan" "map" "struct" "interface" "fallthrough"
        "goto"
      ] @keyword
      (function_declaration name: (identifier) @function)
      (call_expression function: (identifier) @function)
      (type_identifier) @type
      (field_identifier) @property
    )QUERY";

    constexpr std::string_view kCHighlights = R"QUERY(
      (comment) @comment
      (string_literal) @string
      (system_lib_string) @string
      (char_literal) @string
      (number_literal) @number
      (true) @constant
      (false) @constant
      (null) @constant
      [
        "if" "else" "for" "while" "do" "switch" "case" "default" "return"
        "break" "continue" "goto" "sizeof" "struct" "union" "enum" "typedef"
        "static" "const" "volatile" "register" "extern" "auto" "inline"
      ] @keyword
      (primitive_type) @type
      (type_identifier) @type
      (function_declarator declarator: (identifier) @function)
      (call_expression function: (identifier) @function)
      (field_identifier) @property
    )QUERY";

    constexpr std::string_view kCppHighlights = R"QUERY(
      (comment) @comment
      (string_literal) @string
      (raw_string_literal) @string
      (system_lib_string) @string
      (char_literal) @string
      (number_literal) @number
      (true) @constant
      (false) @constant
      (null) @constant
      (nullptr) @constant
      [
        "if" "else" "for" "while" "do" "switch" "case" "default" "return"
        "break" "continue" "goto" "sizeof" "struct" "union" "enum" "typedef"
        "static" "const" "constexpr" "consteval" "constinit" "volatile"
        "register" "extern" "auto" "inline" "class" "namespace" "template"
        "typename" "using" "public" "private" "protected" "virtual" "override"
        "final" "new" "delete" "this" "try" "catch" "throw" "noexcept"
        "operator" "explicit" "friend" "mutable" "co_await" "co_return"
        "co_yield"
      ] @keyword
      (primitive_type) @type
      (type_identifier) @type
      (function_declarator declarator: (identifier) @function)
      (function_declarator declarator: (field_identifier) @function)
      (call_expression function: (identifier) @function)
      (field_identifier) @property
    )QUERY";

    constexpr std::string_view kJavaHighlights = R"QUERY(
      (line_comment) @comment
      (block_comment) @comment
      (string_literal) @string
      (character_literal) @string
      (decimal_integer_literal) @number
      (hex_integer_literal) @number
      (decimal_floating_point_literal) @number
      (true) @constant
      (false) @constant
      (null_literal) @constant
      [
        "if" "else" "for" "while" "do" "switch" "case" "default" "return"
        "break" "continue" "class" "interface" "extends" "implements" "enum"
        "package" "import" "public" "private" "protected" "static" "final"
        "abstract" "synchronized" "volatile" "transient" "native" "new"
        "this" "super" "try" "catch" "finally" "throw" "throws" "instanceof"
        "void"
      ] @keyword
      (type_identifier) @type
      (method_declaration name: (identifier) @function)
      (method_invocation name: (identifier) @function)
    )QUERY";

    constexpr std::string_view kCSharpHighlights = R"QUERY(
      (comment) @comment
      (string_literal) @string
      (verbatim_string_literal) @string
      (character_literal) @string
      (integer_literal) @number
      (real_literal) @number
      (boolean_literal) @constant
      (null_literal) @constant
      [
        "if" "else" "for" "foreach" "while" "do" "switch" "case" "default"
        "return" "break" "continue" "class" "interface" "struct" "enum"
        "namespace" "using" "public" "private" "protected" "internal"
        "static" "readonly" "const" "sealed" "abstract" "virtual" "override"
        "new" "this" "base" "try" "catch" "finally" "throw" "var" "void"
        "async" "await" "in" "out" "ref" "params"
      ] @keyword
      (predefined_type) @type
      (identifier) @variable
    )QUERY";

    constexpr std::string_view kRubyHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (string_content) @string
      (integer) @number
      (float) @number
      (true) @constant
      (false) @constant
      (nil) @constant
      [
        "def" "end" "class" "module" "if" "elsif" "else" "unless" "while"
        "until" "for" "in" "do" "begin" "rescue" "ensure" "return" "yield"
        "break" "next" "redo" "retry" "case" "when" "then" "and" "or" "not"
        "self" "super" "require" "include" "extend"
      ] @keyword
      (identifier) @variable
      (constant) @type
      (method name: (identifier) @function)
      (call method: (identifier) @function)
    )QUERY";

    constexpr std::string_view kLuaHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (number) @number
      (true) @constant
      (false) @constant
      (nil) @constant
      [
        "and" "break" "do" "else" "elseif" "end" "for" "function" "goto"
        "if" "in" "local" "not" "or" "repeat" "return" "then" "until" "while"
      ] @keyword
      (function_declaration name: (identifier) @function)
      (function_call name: (identifier) @function)
    )QUERY";

    constexpr std::string_view kCssHighlights = R"QUERY(
      (comment) @comment
      (string_value) @string
      (integer_value) @number
      (float_value) @number
      (color_value) @constant
      (tag_name) @tag
      (class_name) @type
      (id_name) @type
      (property_name) @property
      (function_name) @function
      (attribute_name) @attribute
    )QUERY";

    constexpr std::string_view kHtmlHighlights = R"QUERY(
      (comment) @comment
      (tag_name) @tag
      (attribute_name) @attribute
      (attribute_value) @string
      (quoted_attribute_value) @string
      (text) @variable
    )QUERY";

    constexpr std::string_view kYamlHighlights = R"QUERY(
      (comment) @comment
      (string_scalar) @string
      (integer_scalar) @number
      (float_scalar) @number
      (boolean_scalar) @constant
      (null_scalar) @constant
      (block_mapping_pair key: (flow_node) @property)
    )QUERY";

    constexpr std::string_view kTomlHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (integer) @number
      (float) @number
      (boolean) @constant
      (offset_date_time) @constant
      (local_date_time) @constant
      (local_date) @constant
      (local_time) @constant
      (bare_key) @property
      (quoted_key) @property
    )QUERY";

    constexpr std::string_view kIniHighlights = R"QUERY(
      (comment) @comment
      (section_name) @type
      (setting_name) @property
      (setting_value) @string
    )QUERY";

    constexpr std::string_view kDockerfileHighlights = R"QUERY(
      (comment) @comment
      (double_quoted_string) @string
      (single_quoted_string) @string
      (unquoted_string) @string
      (image_name) @type
      (image_tag) @constant
      (env_pair name: (unquoted_string) @property)
    )QUERY";

    constexpr std::string_view kSqlHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (number) @number
      (keyword_select) @keyword
      (keyword_from) @keyword
      (keyword_where) @keyword
      (keyword_join) @keyword
      (keyword_on) @keyword
      (keyword_group) @keyword
      (keyword_by) @keyword
      (keyword_order) @keyword
      (keyword_limit) @keyword
      (keyword_insert) @keyword
      (keyword_into) @keyword
      (keyword_values) @keyword
      (keyword_update) @keyword
      (keyword_set) @keyword
      (keyword_delete) @keyword
      (keyword_create) @keyword
      (keyword_table) @keyword
    )QUERY";

    constexpr std::string_view kCmakeHighlights = R"QUERY(
      (line_comment) @comment
      (bracket_comment) @comment
      (quoted_argument) @string
      (bracket_argument) @string
      (unquoted_argument) @variable
      (variable) @variable
      (normal_command (identifier) @function)
    )QUERY";

    constexpr std::string_view kMakeHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (variable_reference) @variable
      (rule (targets) @function)
    )QUERY";

    constexpr std::string_view kRegexHighlights = R"QUERY(
      (character_class) @constant
      (character_class_escape) @constant
      (class_character) @string
      (any_character) @keyword
    )QUERY";

    constexpr std::string_view kMarkdownHighlights = R"QUERY(
      (atx_heading) @keyword
      (setext_heading) @keyword
      (fenced_code_block) @string
      (indented_code_block) @string
      (link_destination) @string
      (link_label) @property
      (block_quote) @comment
    )QUERY";

    constexpr std::string_view kHaskellHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (char) @string
      (integer) @number
      (float) @number
      [
        "module" "where" "import" "data" "type" "newtype" "class" "instance"
        "do" "if" "then" "else" "case" "of" "let" "in" "deriving" "as"
        "qualified" "hiding"
      ] @keyword
      (constructor) @type
      (variable) @variable
    )QUERY";

    constexpr std::string_view kPhpHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (string_value) @string
      (integer) @number
      (float) @number
      (boolean) @constant
      (null) @constant
      [
        "function" "class" "interface" "trait" "extends" "implements" "new"
        "return" "if" "else" "elseif" "for" "foreach" "while" "do" "switch"
        "case" "default" "break" "continue" "try" "catch" "finally" "throw"
        "use" "namespace" "public" "private" "protected" "static" "abstract"
        "final" "const" "var" "echo" "print" "require" "include"
      ] @keyword
      (function_definition name: (name) @function)
      (variable_name) @variable
    )QUERY";

    constexpr std::string_view kSwiftHighlights = R"QUERY(
      (comment) @comment
      (line_str_text) @string
      (raw_str_part) @string
      (integer_literal) @number
      (real_literal) @number
      (boolean_literal) @constant
      (nil) @constant
      [
        "func" "let" "var" "if" "else" "for" "in" "while" "switch" "case"
        "default" "return" "break" "continue" "class" "struct" "enum"
        "protocol" "extension" "import" "public" "private" "internal"
        "fileprivate" "open" "static" "final" "lazy" "mutating" "init"
        "deinit" "self" "super" "throw" "try" "catch" "do" "guard" "defer"
        "where" "as" "is" "typealias"
      ] @keyword
      (type_identifier) @type
    )QUERY";

    constexpr std::string_view kKotlinHighlights = R"QUERY(
      (line_comment) @comment
      (multiline_comment) @comment
      (string_literal) @string
      (character_literal) @string
      (integer_literal) @number
      (real_literal) @number
      (boolean_literal) @constant
      (null_literal) @constant
      [
        "fun" "val" "var" "if" "else" "for" "while" "do" "when" "return"
        "break" "continue" "class" "interface" "object" "package" "import"
        "public" "private" "protected" "internal" "open" "final" "abstract"
        "override" "data" "sealed" "enum" "annotation" "const" "lateinit"
        "vararg" "inline" "noinline" "crossinline" "reified" "in" "out"
        "by" "is" "as" "this" "super" "try" "catch" "finally" "throw"
      ] @keyword
      (type_identifier) @type
    )QUERY";

    constexpr std::string_view kScalaHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (interpolated_string_expression) @string
      (integer_literal) @number
      (floating_point_literal) @number
      (boolean_literal) @constant
      (null_literal) @constant
      [
        "def" "val" "var" "type" "class" "trait" "object" "extends" "with"
        "if" "else" "for" "while" "do" "match" "case" "return" "yield"
        "import" "package" "private" "protected" "implicit" "lazy" "final"
        "abstract" "sealed" "override" "new" "this" "super" "try" "catch"
        "finally" "throw"
      ] @keyword
      (type_identifier) @type
    )QUERY";

    constexpr std::string_view kElixirHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (charlist) @string
      (integer) @number
      (float) @number
      (atom) @constant
      (boolean) @constant
      (nil) @constant
      (alias) @type
    )QUERY";

    constexpr std::string_view kErlangHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (char) @string
      (integer) @number
      (float) @number
      (atom) @constant
      (variable) @variable
    )QUERY";

    constexpr std::string_view kJuliaHighlights = R"QUERY(
      (line_comment) @comment
      (block_comment) @comment
      (string_literal) @string
      (character_literal) @string
      (integer_literal) @number
      (float_literal) @number
      (boolean_literal) @constant
    )QUERY";

    constexpr std::string_view kZigHighlights = R"QUERY(
      (line_comment) @comment
      (doc_comment) @comment
      (string) @string
      (character) @string
      (integer) @number
      (float) @number
    )QUERY";

    constexpr std::string_view kNixHighlights = R"QUERY(
      (comment) @comment
      (string_expression) @string
      (indented_string_expression) @string
      (integer_expression) @number
      (float_expression) @number
    )QUERY";

    constexpr std::string_view kPowershellHighlights = R"QUERY(
      (comment) @comment
      (string_literal) @string
      (integer_literal) @number
      (real_literal) @number
      (variable) @variable
    )QUERY";

    constexpr std::string_view kPerlHighlights = R"QUERY(
      (comment) @comment
      (string_literal) @string
      (number) @number
      (scalar_variable) @variable
      (array_variable) @variable
      (hash_variable) @variable
    )QUERY";

    constexpr std::string_view kProtoHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      [(int_lit) (float_lit)] @number
      (bool) @constant
      [
        "syntax" "package" "import" "option" "message" "enum" "service"
        "rpc" "returns" "stream" "repeated" "optional" "required" "reserved"
      ] @keyword
      (type) @type
      (message_name) @type
      (enum_name) @type
      (rpc_name) @function
    )QUERY";

    constexpr std::string_view kGraphqlHighlights = R"QUERY(
      (comment) @comment
      (string_value) @string
      (int_value) @number
      (float_value) @number
      (boolean_value) @constant
      (null_value) @constant
      (named_type (name) @type)
      (field (name) @property)
      (directive (name) @function)
    )QUERY";

    constexpr std::string_view kDartHighlights = R"QUERY(
      (comment) @comment
      (documentation_comment) @comment
      (string_literal) @string
      (decimal_integer_literal) @number
      (decimal_floating_point_literal) @number
      (hex_integer_literal) @number
      (true) @constant
      (false) @constant
      (null_literal) @constant
      (type_identifier) @type
    )QUERY";

    constexpr std::string_view kVerilogHighlights = R"QUERY(
      (comment) @comment
      (string_literal) @string
      (integral_number) @number
      (real_number) @number
    )QUERY";

    constexpr std::string_view kSolidityHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (number_literal) @number
      (true) @constant
      (false) @constant
      (type_name) @type
    )QUERY";

    constexpr std::string_view kClojureHighlights = R"QUERY(
      (comment) @comment
      (str_lit) @string
      (num_lit) @number
      (bool_lit) @constant
      (nil_lit) @constant
      (kwd_lit) @constant
      (sym_lit) @variable
    )QUERY";

    constexpr std::string_view kStarlarkHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (integer) @number
      (float) @number
      (true) @constant
      (false) @constant
      (none) @constant
      [
        "def" "return" "if" "elif" "else" "for" "in" "not" "and" "or"
        "load" "pass" "break" "continue" "lambda"
      ] @keyword
      (function_definition name: (identifier) @function)
    )QUERY";

    constexpr std::string_view kHclHighlights = R"QUERY(
      (comment) @comment
      (string_lit) @string
      (numeric_lit) @number
      (bool_lit) @constant
      (null_lit) @constant
      (identifier) @variable
    )QUERY";

    constexpr std::string_view kVueHighlights = R"QUERY(
      (comment) @comment
      (tag_name) @tag
      (attribute_name) @attribute
      (attribute_value) @string
      (quoted_attribute_value) @string
    )QUERY";

    constexpr std::string_view kSvelteHighlights = R"QUERY(
      (comment) @comment
      (tag_name) @tag
      (attribute_name) @attribute
      (attribute_value) @string
      (quoted_attribute_value) @string
    )QUERY";

    constexpr std::string_view kAstroHighlights = R"QUERY(
      (comment) @comment
      (tag_name) @tag
      (attribute_name) @attribute
      (attribute_value) @string
    )QUERY";

    constexpr std::string_view kXmlHighlights = R"QUERY(
      (Comment) @comment
      (Name) @tag
      (AttValue) @string
      (CharData) @variable
    )QUERY";

    constexpr std::string_view kRHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (integer) @number
      (float) @number
      (true) @constant
      (false) @constant
      (null) @constant
      (na) @constant
    )QUERY";

    constexpr std::string_view kJustHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (recipe_header (identifier) @function)
    )QUERY";

    constexpr std::string_view kOcamlHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (number) @number
      (boolean) @constant
    )QUERY";

    constexpr std::string_view kObjcHighlights = R"QUERY(
      (comment) @comment
      (string_literal) @string
      (number_literal) @number
      (true) @constant
      (false) @constant
      (null) @constant
      (type_identifier) @type
    )QUERY";

    constexpr std::string_view kOdinHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (number) @number
      (boolean) @constant
      (nil) @constant
    )QUERY";

    constexpr std::string_view kTlaplusHighlights = R"QUERY(
      (comment) @comment
      (string) @string
      (nat_number) @number
      (boolean) @constant
    )QUERY";

    constexpr std::string_view kDiffHighlights = R"QUERY(
      (commit) @comment
    )QUERY";

    // Comment-only universal fallback. Most grammars expose a `comment`
    // node; for the few that don't, query compilation fails and tokenize()
    // falls back to an empty token list (language still recognized).
    constexpr std::string_view kFallbackHighlights = "(comment) @comment";

    struct lang_def {
      std::string_view canonical; // tree-sitter grammar name
      std::string_view query;     // highlights query source ("" = no tokens)
    };

    // Registry — every bundled grammar appears here. Empty query means
    // "language is recognized but ships no highlights" (used for grammars
    // whose node-types we haven't surveyed). Non-empty queries are
    // best-effort; tokenize() catches compilation failures and falls back.
    constexpr std::array<lang_def, 60> kLangs{{
        {"astro", kAstroHighlights},
        {"bash", kBashHighlights},
        {"c", kCHighlights},
        {"clojure", kClojureHighlights},
        {"cmake", kCmakeHighlights},
        {"c_sharp", kCSharpHighlights},
        {"cpp", kCppHighlights},
        {"css", kCssHighlights},
        {"dart", kDartHighlights},
        {"diff", kDiffHighlights},
        {"dockerfile", kDockerfileHighlights},
        {"elixir", kElixirHighlights},
        {"erlang", kErlangHighlights},
        {"go", kGoHighlights},
        {"graphql", kGraphqlHighlights},
        {"haskell", kHaskellHighlights},
        {"hcl", kHclHighlights},
        {"html", kHtmlHighlights},
        {"ini", kIniHighlights},
        {"java", kJavaHighlights},
        {"javascript", kJavascriptHighlights},
        {"json", kJsonHighlights},
        {"julia", kJuliaHighlights},
        {"just", kJustHighlights},
        {"kotlin", kKotlinHighlights},
        {"lua", kLuaHighlights},
        {"make", kMakeHighlights},
        {"nix", kNixHighlights},
        {"objc", kObjcHighlights},
        {"odin", kOdinHighlights},
        {"perl", kPerlHighlights},
        {"powershell", kPowershellHighlights},
        {"proto", kProtoHighlights},
        {"python", kPythonHighlights},
        {"r", kRHighlights},
        {"regex", kRegexHighlights},
        {"ruby", kRubyHighlights},
        {"rust", kRustHighlights},
        {"scala", kScalaHighlights},
        {"solidity", kSolidityHighlights},
        {"sql", kSqlHighlights},
        {"starlark", kStarlarkHighlights},
        {"svelte", kSvelteHighlights},
        {"swift", kSwiftHighlights},
        {"tlaplus", kTlaplusHighlights},
        {"toml", kTomlHighlights},
        {"verilog", kVerilogHighlights},
        {"vue", kVueHighlights},
        {"yaml", kYamlHighlights},
        {"zig", kZigHighlights},
        {"typescript", kTypescriptHighlights},
        {"tsx", kTsxHighlights},
        {"markdown", kMarkdownHighlights},
        {"markdown_inline", kFallbackHighlights},
        {"ocaml", kOcamlHighlights},
        {"ocaml_interface", kOcamlHighlights},
        {"ocaml_type", kFallbackHighlights},
        {"php", kPhpHighlights},
        {"php_only", kPhpHighlights},
        {"xml", kXmlHighlights},
    }};

    // Aliases mapped to canonical grammar names. Anything not listed here
    // is matched against canonical names directly.
    std::string_view canonicalize(std::string_view name) {
      // typescript family
      if (name == "ts") return "typescript";
      if (name == "jsx") return "tsx";
      if (name == "js" || name == "mjs" || name == "cjs") return "javascript";
      // c family
      if (name == "c++" || name == "cxx" || name == "cc" || name == "h" || name == "hpp")
        return "cpp";
      if (name == "cs" || name == "csharp" || name == "c#") return "c_sharp";
      if (name == "objective-c" || name == "objectivec" || name == "m") return "objc";
      // shell
      if (name == "sh" || name == "shell" || name == "zsh") return "bash";
      // scripting
      if (name == "py") return "python";
      if (name == "rb") return "ruby";
      if (name == "rs") return "rust";
      if (name == "kt" || name == "kts") return "kotlin";
      if (name == "ps" || name == "ps1") return "powershell";
      if (name == "pl") return "perl";
      // data
      if (name == "yml") return "yaml";
      if (name == "jsonc" || name == "json5") return "json";
      if (name == "dockerfile" || name == "containerfile") return "dockerfile";
      if (name == "tf" || name == "terraform" || name == "hcl2") return "hcl";
      // web
      if (name == "htm") return "html";
      if (name == "scss" || name == "sass" || name == "less") return "css";
      // markdown / docs
      if (name == "md" || name == "markdown") return "markdown";
      // misc
      if (name == "make" || name == "makefile") return "make";
      if (name == "tla") return "tlaplus";
      if (name == "starlark" || name == "bzl" || name == "bazel") return "starlark";
      if (name == "graphql" || name == "gql") return "graphql";
      if (name == "proto" || name == "protobuf") return "proto";
      if (name == "ml") return "ocaml";
      if (name == "mli") return "ocaml_interface";
      return name;
    }

    const lang_def* lookup(std::string_view name) {
      auto canon = canonicalize(name);
      for (const auto& l : kLangs) {
        if (l.canonical == canon)
          return &l;
      }
      return nullptr;
    }

  } // namespace

  std::vector<std::string_view> supported_languages() {
    std::vector<std::string_view> out;
    out.reserve(kLangs.size());
    for (const auto& l : kLangs)
      out.push_back(l.canonical);
    return out;
  }

  std::optional<result> tokenize(std::string_view source, std::string_view language) {
    const lang_def* def = lookup(language);
    if (!def)
      return std::nullopt;

    const TSLanguage* lang = ts::language_by_name(def->canonical);
    if (!lang)
      return std::nullopt;

    result r;
    r.language = std::string(def->canonical);
    if (source.empty() || def->query.empty())
      return r;

    ts::parser p;
    if (!p.set_language(lang))
      return r;

    auto parsed = p.parse(source);
    if (!parsed.is_valid())
      return r;

    // Try the grammar-specific query, then fall back to the universal
    // (comment)-only query, then give up (return empty tokens).
    std::unique_ptr<ts::query> q;
    auto try_compile = [&](std::string_view src) -> bool {
      try {
        q = std::make_unique<ts::query>(lang, src);
        return true;
      } catch (const std::exception&) {
        q.reset();
        return false;
      }
    };

    if (!try_compile(def->query) && !try_compile(kFallbackHighlights))
      return r;

    struct cap_record {
      u32 start = 0;
      u32 end = 0;
      std::string name;
    };
    std::vector<cap_record> caps;
    q->run(parsed.root(), 0u, static_cast<u32>(source.size()),
           [&](const ts::query::capture& c) {
             u32 s = c.n.start_byte();
             u32 e = c.n.end_byte();
             if (e > s)
               caps.push_back({s, e, std::string(c.name)});
             return true;
           });

    if (caps.empty())
      return r;

    // Per-byte assignment: longer (less specific) spans first, shorter
    // (more specific) spans overwrite. Result: each byte ends up tagged
    // with the smallest capture that contains it.
    std::sort(caps.begin(), caps.end(), [](const cap_record& a, const cap_record& b) {
      const u32 la = a.end - a.start;
      const u32 lb = b.end - b.start;
      if (la != lb)
        return la > lb;
      return a.start < b.start;
    });

    std::unordered_map<std::string, u16> name_to_id;
    std::vector<std::string> id_to_name;
    id_to_name.emplace_back(); // id 0 = unhighlighted
    auto intern = [&](const std::string& n) -> u16 {
      auto it = name_to_id.find(n);
      if (it != name_to_id.end())
        return it->second;
      const u16 id = static_cast<u16>(id_to_name.size());
      id_to_name.push_back(n);
      name_to_id.emplace(n, id);
      return id;
    };

    const u32 n = static_cast<u32>(source.size());
    std::vector<u16> per_byte(n, 0);
    for (const auto& c : caps) {
      const u16 id = intern(c.name);
      const u32 e = std::min<u32>(c.end, n);
      for (u32 i = c.start; i < e; ++i)
        per_byte[i] = id;
    }

    r.tokens.reserve(64);
    u32 i = 0;
    while (i < n) {
      const u16 id = per_byte[i];
      if (id == 0) {
        ++i;
        continue;
      }
      u32 j = i + 1;
      while (j < n && per_byte[j] == id)
        ++j;
      r.tokens.push_back({i, j, id_to_name[id]});
      i = j;
    }
    return r;
  }

} // namespace fxe::highlight
