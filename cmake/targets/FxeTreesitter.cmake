# fxe_treesitter — Tree-sitter integration. Static library exposing:
#   - The Tree-sitter runtime (from vcpkg `unofficial::tree-sitter::tree-sitter`).
#   - A bundled set of grammars (parser.c/scanner.c per language, fetched via
#     FetchContent from upstream grammar repos).
#   - C++ wrappers (`src/treesitter/*.cpp`) that present a friendlier API
#     and own grammar registration. Consumed by `fxe_js` for the
#     `fxe:treesitter` synthetic module.
#
# Each grammar's parser.c includes `"tree_sitter/parser.h"` — the same header
# the runtime ships at `<install>/include/tree_sitter/parser.h`. We rely on
# that single canonical copy from the runtime instead of pulling per-grammar
# copies onto the include path (they're identical content but adding many
# would create header-collision hazards).
#
# Adding a grammar = three steps:
#   1. _fxe_ts_fetch(<key> <url>) below.
#   2. _fxe_ts_subdir("${<key>_SOURCE_DIR>}/<src>") for each src dir in the repo.
#   3. extern "C" + registry row in src/treesitter/grammars.cpp.

if(FXE_ENABLE_TREESITTER)
    find_package(unofficial-tree-sitter CONFIG QUIET)
    if(NOT unofficial-tree-sitter_FOUND)
        message(
            STATUS
            "fxe_treesitter: unofficial-tree-sitter not found; tree-sitter integration disabled"
        )
        return()
    endif()

    include(FetchContent)

    set(_fxe_ts_grammar_sources)
    set(_fxe_ts_grammar_includes)

    # Append parser.c, scanner.c, and scanner.cc (when present) from <_src>
    # to the grammar source list, and add <_src> to the private include path
    # so `#include "tree_sitter/parser.h"` resolves locally.
    macro(_fxe_ts_subdir _src)
        if(EXISTS "${_src}/parser.c")
            list(APPEND _fxe_ts_grammar_sources "${_src}/parser.c")
        endif()
        if(EXISTS "${_src}/scanner.c")
            list(APPEND _fxe_ts_grammar_sources "${_src}/scanner.c")
        endif()
        if(EXISTS "${_src}/scanner.cc")
            list(APPEND _fxe_ts_grammar_sources "${_src}/scanner.cc")
        endif()
        list(APPEND _fxe_ts_grammar_includes "${_src}")
    endmacro()

    # Tarball-only fetch. We skip FetchContent_MakeAvailable (and thus
    # add_subdirectory) because grammar repos' CMakeLists.txt files routinely
    # look for bindings/c/*.pc.in (missing in archive tarballs) and define a
    # colliding `ts-test` custom target.
    #
    # Use FetchContent_Populate with explicit URL and SOURCE_DIR/BINARY_DIR — not
    # Populate(<name>) after Declare, which CMake 3.30+ deprecates (CMP0169).
    # Paths use FETCHCONTENT_BASE_DIR so downloads stay under build/_deps (CI
    # cache).
    macro(_fxe_ts_fetch _key _url)
        # FetchContent_Populate is implemented as a function; when it is invoked
        # from a macro, *_SOURCE_DIR is not always visible in this directory
        # scope afterward (so _fxe_ts_subdir would see an empty path and skip
        # every parser.c). Pin SOURCE_DIR to the same layout we pass in below.
        set(_fxe_ts_dep_src "${FETCHCONTENT_BASE_DIR}/${_key}-src")
        FetchContent_Populate(
            ${_key}
            URL "${_url}"
            SOURCE_DIR "${_fxe_ts_dep_src}"
            BINARY_DIR "${FETCHCONTENT_BASE_DIR}/${_key}-build"
        )
        set(${_key}_SOURCE_DIR "${_fxe_ts_dep_src}")
        unset(_fxe_ts_dep_src)
    endmacro()

    # ----- single-grammar repos --------------------------------------------
    _fxe_ts_fetch(
        tree_sitter_astro
        https://github.com/virchau13/tree-sitter-astro/archive/refs/heads/master.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_astro_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_bash
        https://github.com/tree-sitter/tree-sitter-bash/archive/refs/tags/v0.25.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_bash_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_c
        https://github.com/tree-sitter/tree-sitter-c/archive/refs/tags/v0.24.1.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_c_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_clojure
        https://github.com/sogaiu/tree-sitter-clojure/archive/refs/heads/master.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_clojure_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_cmake
        https://github.com/uyha/tree-sitter-cmake/archive/refs/tags/v0.7.1.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_cmake_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_c_sharp
        https://github.com/tree-sitter/tree-sitter-c-sharp/archive/refs/tags/v0.23.1.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_c_sharp_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_cpp
        https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_cpp_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_dart
        https://github.com/UserNobody14/tree-sitter-dart/archive/refs/heads/master.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_dart_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_css
        https://github.com/tree-sitter/tree-sitter-css/archive/refs/tags/v0.23.2.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_css_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_diff
        https://github.com/the-mikedavis/tree-sitter-diff/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_diff_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_dockerfile
        https://github.com/camdencheek/tree-sitter-dockerfile/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_dockerfile_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_elixir
        https://github.com/elixir-lang/tree-sitter-elixir/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_elixir_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_erlang
        https://github.com/WhatsApp/tree-sitter-erlang/archive/refs/tags/0.16.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_erlang_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_go
        https://github.com/tree-sitter/tree-sitter-go/archive/refs/tags/v0.25.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_go_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_graphql
        https://github.com/bkegley/tree-sitter-graphql/archive/refs/heads/master.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_graphql_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_haskell
        https://github.com/tree-sitter/tree-sitter-haskell/archive/refs/tags/v0.23.1.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_haskell_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_hcl
        https://github.com/MichaHoffmann/tree-sitter-hcl/archive/refs/tags/v1.1.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_hcl_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_html
        https://github.com/tree-sitter/tree-sitter-html/archive/refs/tags/v0.23.2.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_html_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_ini
        https://github.com/justinmk/tree-sitter-ini/archive/refs/tags/v1.3.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_ini_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_java
        https://github.com/tree-sitter/tree-sitter-java/archive/refs/tags/v0.23.5.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_java_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_javascript
        https://github.com/tree-sitter/tree-sitter-javascript/archive/refs/tags/v0.25.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_javascript_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_json
        https://github.com/tree-sitter/tree-sitter-json/archive/refs/tags/v0.24.8.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_json_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_just
        https://github.com/IndianBoy42/tree-sitter-just/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_just_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_julia
        https://github.com/tree-sitter/tree-sitter-julia/archive/refs/tags/v0.23.1.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_julia_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_kotlin
        https://github.com/fwcd/tree-sitter-kotlin/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_kotlin_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_lua
        https://github.com/tree-sitter-grammars/tree-sitter-lua/archive/refs/tags/v0.4.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_lua_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_make
        https://github.com/tree-sitter-grammars/tree-sitter-make/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_make_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_nix
        https://github.com/nix-community/tree-sitter-nix/archive/refs/heads/master.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_nix_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_objc
        https://github.com/tree-sitter-grammars/tree-sitter-objc/archive/refs/tags/v3.0.2.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_objc_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_odin
        https://github.com/amaanq/tree-sitter-odin/archive/refs/tags/v1.3.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_odin_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_perl
        https://github.com/tree-sitter-perl/tree-sitter-perl/archive/refs/heads/release.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_perl_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_powershell
        https://github.com/airbus-cert/tree-sitter-powershell/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_powershell_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_proto
        https://github.com/mitchellh/tree-sitter-proto/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_proto_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_python
        https://github.com/tree-sitter/tree-sitter-python/archive/refs/tags/v0.25.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_python_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_r
        https://github.com/r-lib/tree-sitter-r/archive/refs/tags/v1.2.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_r_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_regex
        https://github.com/tree-sitter/tree-sitter-regex/archive/refs/tags/v0.25.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_regex_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_ruby
        https://github.com/tree-sitter/tree-sitter-ruby/archive/refs/tags/v0.23.1.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_ruby_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_rust
        https://github.com/tree-sitter/tree-sitter-rust/archive/refs/tags/v0.24.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_rust_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_scala
        https://github.com/tree-sitter/tree-sitter-scala/archive/refs/tags/v0.23.4.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_scala_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_solidity
        https://github.com/JoranHonig/tree-sitter-solidity/archive/refs/heads/master.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_solidity_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_sql
        https://github.com/m-novikov/tree-sitter-sql/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_sql_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_starlark
        https://github.com/tree-sitter-grammars/tree-sitter-starlark/archive/refs/tags/v1.3.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_starlark_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_svelte
        https://github.com/tree-sitter-grammars/tree-sitter-svelte/archive/refs/heads/master.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_svelte_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_swift
        https://github.com/alex-pinkus/tree-sitter-swift/archive/refs/heads/with-generated-files.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_swift_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_toml
        https://github.com/tree-sitter-grammars/tree-sitter-toml/archive/refs/tags/v0.7.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_toml_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_tlaplus
        https://github.com/tlaplus-community/tree-sitter-tlaplus/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_tlaplus_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_verilog
        https://github.com/tree-sitter/tree-sitter-verilog/archive/refs/heads/master.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_verilog_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_vue
        https://github.com/tree-sitter-grammars/tree-sitter-vue/archive/refs/heads/main.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_vue_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_yaml
        https://github.com/tree-sitter-grammars/tree-sitter-yaml/archive/refs/tags/v0.7.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_yaml_SOURCE_DIR}/src")

    _fxe_ts_fetch(
        tree_sitter_zig
        https://github.com/tree-sitter-grammars/tree-sitter-zig/archive/refs/tags/v1.1.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_zig_SOURCE_DIR}/src")

    # ----- multi-grammar repos ---------------------------------------------
    # tree-sitter-typescript hosts BOTH TypeScript and TSX in one repo.
    _fxe_ts_fetch(
        tree_sitter_typescript
        https://github.com/tree-sitter/tree-sitter-typescript/archive/refs/tags/v0.23.2.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_typescript_SOURCE_DIR}/typescript/src")
    _fxe_ts_subdir("${tree_sitter_typescript_SOURCE_DIR}/tsx/src")

    # tree-sitter-markdown hosts the block grammar and the inline grammar.
    _fxe_ts_fetch(
        tree_sitter_markdown
        https://github.com/tree-sitter-grammars/tree-sitter-markdown/archive/refs/tags/v0.5.0.tar.gz
    )
    _fxe_ts_subdir(
        "${tree_sitter_markdown_SOURCE_DIR}/tree-sitter-markdown/src"
    )
    _fxe_ts_subdir(
        "${tree_sitter_markdown_SOURCE_DIR}/tree-sitter-markdown-inline/src"
    )

    # tree-sitter-ocaml hosts ocaml, ocaml_interface (.mli), and ocaml_type.
    _fxe_ts_fetch(
        tree_sitter_ocaml
        https://github.com/tree-sitter/tree-sitter-ocaml/archive/refs/tags/v0.24.2.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_ocaml_SOURCE_DIR}/grammars/ocaml/src")
    _fxe_ts_subdir("${tree_sitter_ocaml_SOURCE_DIR}/grammars/interface/src")
    _fxe_ts_subdir("${tree_sitter_ocaml_SOURCE_DIR}/grammars/type/src")

    # tree-sitter-php hosts php (with HTML embedding) and php_only.
    _fxe_ts_fetch(
        tree_sitter_php
        https://github.com/tree-sitter/tree-sitter-php/archive/refs/tags/v0.24.2.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_php_SOURCE_DIR}/php/src")
    _fxe_ts_subdir("${tree_sitter_php_SOURCE_DIR}/php_only/src")

    # tree-sitter-xml hosts xml and dtd.
    _fxe_ts_fetch(
        tree_sitter_xml
        https://github.com/tree-sitter-grammars/tree-sitter-xml/archive/refs/tags/v0.7.0.tar.gz
    )
    _fxe_ts_subdir("${tree_sitter_xml_SOURCE_DIR}/xml/src")
    _fxe_ts_subdir("${tree_sitter_xml_SOURCE_DIR}/dtd/src")

    # ----- library ---------------------------------------------------------
    file(GLOB _fxe_treesitter_cpp CONFIGURE_DEPENDS src/treesitter/*.cpp)
    add_library(
        fxe_treesitter
        STATIC
        ${_fxe_treesitter_cpp}
        ${_fxe_ts_grammar_sources}
    )
    add_library(fxe::treesitter ALIAS fxe_treesitter)
    target_include_directories(fxe_treesitter PUBLIC include)
    target_compile_features(fxe_treesitter PUBLIC cxx_std_20)
    target_link_libraries(
        fxe_treesitter
        PUBLIC unofficial::tree-sitter::tree-sitter
    )
    # Each grammar source includes "tree_sitter/parser.h"; the runtime's
    # public include directory provides it. Grammar repos also bundle a
    # private copy at <repo>/<lang>/src/tree_sitter/parser.h — adding the
    # parent `src/` so `#include "tree_sitter/parser.h"` resolves locally
    # avoids tying us to the runtime's exposure of that header (some
    # vcpkg-installed runtimes only export api.h publicly).
    target_include_directories(
        fxe_treesitter
        PRIVATE ${_fxe_ts_grammar_includes}
    )
    # Generated grammar sources contain unused parameters / signed-vs-unsigned
    # comparisons / large jump tables that trip our project warning floor.
    # Only relax for the upstream parser/scanner files; keep the C++ wrappers
    # under the strict flags.
    # tree-sitter-just (and similar scanners) use `#error` when NDEBUG is
    # defined; release presets still pass -DNDEBUG for the target. Undefine
    # NDEBUG for grammar C sources only (Clang/GCC: -UNDEBUG). MSVC: /UNDEBUG.
    if(MSVC)
        set(_fxe_ts_grammar_opts "/w" "/UNDEBUG")
    else()
        set(_fxe_ts_grammar_opts "-w" "-UNDEBUG")
    endif()
    set_source_files_properties(
        ${_fxe_ts_grammar_sources}
        PROPERTIES COMPILE_OPTIONS "${_fxe_ts_grammar_opts}"
    )
    target_compile_definitions(fxe_treesitter PUBLIC FXE_HAS_TREESITTER=1)

    if(TARGET fxe_js)
        target_link_libraries(fxe_js PUBLIC fxe_treesitter)
    endif()
endif()

