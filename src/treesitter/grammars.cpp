// Registry of baked-in grammars. Each `tree_sitter_<name>()` is the C entry
// point exported by the corresponding grammar's parser.c. Adding a new
// grammar means: bake its parser.c (+ scanner.c if any) into the build via
// FxeTreesitter.cmake, declare the extern below, and add the registry row.

#include <fxe/treesitter.hpp>
#include <fxe/types.hpp>

#include <array>
#include <string_view>

extern "C" {
  // Single-language repos (alphabetical).
  const struct TSLanguage* tree_sitter_astro(void);
  const struct TSLanguage* tree_sitter_bash(void);
  const struct TSLanguage* tree_sitter_c(void);
  const struct TSLanguage* tree_sitter_clojure(void);
  const struct TSLanguage* tree_sitter_cmake(void);
  const struct TSLanguage* tree_sitter_c_sharp(void);
  const struct TSLanguage* tree_sitter_cpp(void);
  const struct TSLanguage* tree_sitter_css(void);
  const struct TSLanguage* tree_sitter_dart(void);
  const struct TSLanguage* tree_sitter_diff(void);
  const struct TSLanguage* tree_sitter_dockerfile(void);
  const struct TSLanguage* tree_sitter_elixir(void);
  const struct TSLanguage* tree_sitter_erlang(void);
  const struct TSLanguage* tree_sitter_go(void);
  const struct TSLanguage* tree_sitter_graphql(void);
  const struct TSLanguage* tree_sitter_haskell(void);
  const struct TSLanguage* tree_sitter_hcl(void);
  const struct TSLanguage* tree_sitter_html(void);
  const struct TSLanguage* tree_sitter_ini(void);
  const struct TSLanguage* tree_sitter_java(void);
  const struct TSLanguage* tree_sitter_javascript(void);
  const struct TSLanguage* tree_sitter_json(void);
  const struct TSLanguage* tree_sitter_julia(void);
  const struct TSLanguage* tree_sitter_just(void);
  const struct TSLanguage* tree_sitter_kotlin(void);
  const struct TSLanguage* tree_sitter_lua(void);
  const struct TSLanguage* tree_sitter_make(void);
  const struct TSLanguage* tree_sitter_nix(void);
  const struct TSLanguage* tree_sitter_objc(void);
  const struct TSLanguage* tree_sitter_odin(void);
  const struct TSLanguage* tree_sitter_perl(void);
  const struct TSLanguage* tree_sitter_powershell(void);
  const struct TSLanguage* tree_sitter_proto(void);
  const struct TSLanguage* tree_sitter_python(void);
  const struct TSLanguage* tree_sitter_r(void);
  const struct TSLanguage* tree_sitter_regex(void);
  const struct TSLanguage* tree_sitter_ruby(void);
  const struct TSLanguage* tree_sitter_rust(void);
  const struct TSLanguage* tree_sitter_scala(void);
  const struct TSLanguage* tree_sitter_solidity(void);
  const struct TSLanguage* tree_sitter_sql(void);
  const struct TSLanguage* tree_sitter_starlark(void);
  const struct TSLanguage* tree_sitter_svelte(void);
  const struct TSLanguage* tree_sitter_swift(void);
  const struct TSLanguage* tree_sitter_tlaplus(void);
  const struct TSLanguage* tree_sitter_toml(void);
  const struct TSLanguage* tree_sitter_verilog(void);
  const struct TSLanguage* tree_sitter_vue(void);
  const struct TSLanguage* tree_sitter_yaml(void);
  const struct TSLanguage* tree_sitter_zig(void);

  // Multi-grammar repos.
  const struct TSLanguage* tree_sitter_typescript(void);
  const struct TSLanguage* tree_sitter_tsx(void);
  const struct TSLanguage* tree_sitter_markdown(void);
  const struct TSLanguage* tree_sitter_markdown_inline(void);
  const struct TSLanguage* tree_sitter_ocaml(void);
  const struct TSLanguage* tree_sitter_ocaml_interface(void);
  const struct TSLanguage* tree_sitter_ocaml_type(void);
  const struct TSLanguage* tree_sitter_php(void);
  const struct TSLanguage* tree_sitter_php_only(void);
  const struct TSLanguage* tree_sitter_xml(void);
  const struct TSLanguage* tree_sitter_dtd(void);
}

namespace fxe::treesitter {

  namespace {
    struct row {
      std::string_view name;
      const TSLanguage* (*fn)();
    };

    constexpr std::array<row, 60> kGrammars{{
        {"astro", &tree_sitter_astro},
        {"bash", &tree_sitter_bash},
        {"c", &tree_sitter_c},
        {"clojure", &tree_sitter_clojure},
        {"cmake", &tree_sitter_cmake},
        {"c_sharp", &tree_sitter_c_sharp},
        {"cpp", &tree_sitter_cpp},
        {"css", &tree_sitter_css},
        {"dart", &tree_sitter_dart},
        {"diff", &tree_sitter_diff},
        {"dockerfile", &tree_sitter_dockerfile},
        {"elixir", &tree_sitter_elixir},
        {"erlang", &tree_sitter_erlang},
        {"go", &tree_sitter_go},
        {"graphql", &tree_sitter_graphql},
        {"haskell", &tree_sitter_haskell},
        {"hcl", &tree_sitter_hcl},
        {"html", &tree_sitter_html},
        {"ini", &tree_sitter_ini},
        {"java", &tree_sitter_java},
        {"javascript", &tree_sitter_javascript},
        {"json", &tree_sitter_json},
        {"julia", &tree_sitter_julia},
        {"just", &tree_sitter_just},
        {"kotlin", &tree_sitter_kotlin},
        {"lua", &tree_sitter_lua},
        {"make", &tree_sitter_make},
        {"nix", &tree_sitter_nix},
        {"objc", &tree_sitter_objc},
        {"odin", &tree_sitter_odin},
        {"perl", &tree_sitter_perl},
        {"powershell", &tree_sitter_powershell},
        {"proto", &tree_sitter_proto},
        {"python", &tree_sitter_python},
        {"r", &tree_sitter_r},
        {"regex", &tree_sitter_regex},
        {"ruby", &tree_sitter_ruby},
        {"rust", &tree_sitter_rust},
        {"scala", &tree_sitter_scala},
        {"solidity", &tree_sitter_solidity},
        {"sql", &tree_sitter_sql},
        {"starlark", &tree_sitter_starlark},
        {"svelte", &tree_sitter_svelte},
        {"swift", &tree_sitter_swift},
        {"tlaplus", &tree_sitter_tlaplus},
        {"toml", &tree_sitter_toml},
        {"verilog", &tree_sitter_verilog},
        {"vue", &tree_sitter_vue},
        {"yaml", &tree_sitter_yaml},
        {"zig", &tree_sitter_zig},
        {"typescript", &tree_sitter_typescript},
        {"tsx", &tree_sitter_tsx},
        {"markdown", &tree_sitter_markdown},
        {"markdown_inline", &tree_sitter_markdown_inline},
        {"ocaml", &tree_sitter_ocaml},
        {"ocaml_interface", &tree_sitter_ocaml_interface},
        {"ocaml_type", &tree_sitter_ocaml_type},
        {"php", &tree_sitter_php},
        {"php_only", &tree_sitter_php_only},
        {"xml", &tree_sitter_xml},
        // dtd lives in the same repo as xml; some grammar versions don't
        // export tree_sitter_dtd. Re-add when upstream stabilizes the
        // function name. For now `xml` covers DTD subset parsing.
    }};
  } // namespace

  const TSLanguage* language_by_name(std::string_view name) {
    for (const auto& r : kGrammars) {
      if (r.name == name) return r.fn();
    }
    return nullptr;
  }

  std::vector<std::string_view> available_languages() {
    std::vector<std::string_view> out;
    out.reserve(kGrammars.size());
    for (const auto& r : kGrammars) out.push_back(r.name);
    return out;
  }

} // namespace fxe::treesitter
