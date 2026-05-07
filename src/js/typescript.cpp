#include <fxe/types.hpp>
#include <fxe/typescript.hpp>

#include <fxe/generated/fxe_types.hpp>
#include <fxe/generated/typescript_compiler.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include <v8.h>

namespace fxe::js {
  namespace {
    constexpr u32 k_typescript_context_slot = 1;

    std::string to_std_string(v8::Isolate* iso, v8::Local<v8::Value> value) {
      if (value.IsEmpty())
        return {};
      v8::String::Utf8Value utf8(iso, value);
      return *utf8 ? std::string(*utf8, utf8.length()) : std::string{};
    }

    v8::Local<v8::String> str(v8::Isolate* iso, std::string_view value) {
      return v8::String::NewFromUtf8(iso, value.data(), v8::NewStringType::kNormal,
                                     static_cast<int>(value.size()))
          .ToLocalChecked();
    }

    std::string exception_message(v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                  v8::TryCatch& try_catch, std::string_view prefix) {
      std::string message(prefix);
      message += to_std_string(iso, try_catch.Exception());
      v8::Local<v8::Value> stack;
      if (try_catch.StackTrace(ctx).ToLocal(&stack)) {
        std::string rendered = to_std_string(iso, stack);
        if (!rendered.empty()) {
          message += "\n";
          message += rendered;
        }
      }
      return message;
    }

    typescript_transpile_result get_compiler_context(v8::Isolate* iso,
                                                     v8::Local<v8::Context>& out) {
      if (auto* cached =
              static_cast<v8::Global<v8::Context>*>(iso->GetData(k_typescript_context_slot))) {
        out = cached->Get(iso);
        return {true, {}, {}};
      }

      v8::EscapableHandleScope handle_scope(iso);
      v8::Local<v8::Context> ctx = v8::Context::New(iso);
      v8::Context::Scope context_scope(ctx);
      v8::TryCatch try_catch(iso);

      v8::ScriptOrigin origin(str(iso, "<fxe-typescript-compiler>"));
      v8::Local<v8::Script> script;
      if (!v8::Script::Compile(ctx, str(iso, k_typescript_compiler_source), &origin)
               .ToLocal(&script)) {
        return {false,
                {},
                exception_message(iso, ctx, try_catch,
                                  "failed to compile embedded TypeScript compiler: ")};
      }
      v8::Local<v8::Value> ignored;
      if (!script->Run(ctx).ToLocal(&ignored)) {
        return {false,
                {},
                exception_message(iso, ctx, try_catch,
                                  "failed to initialize embedded TypeScript compiler: ")};
      }

      v8::Local<v8::Value> ts_value;
      if (!ctx->Global()->Get(ctx, str(iso, "ts")).ToLocal(&ts_value) || !ts_value->IsObject()) {
        return {false, {}, "embedded TypeScript compiler did not create global `ts`"};
      }
      auto ts = ts_value.As<v8::Object>();
      v8::Local<v8::Value> create_program;
      v8::Local<v8::Value> script_target;
      if (!ts->Get(ctx, str(iso, "createProgram")).ToLocal(&create_program) ||
          !create_program->IsFunction() ||
          !ts->Get(ctx, str(iso, "ScriptTarget")).ToLocal(&script_target) ||
          !script_target->IsObject()) {
        return {false, {}, "embedded TypeScript compiler is missing required compiler APIs"};
      }

      auto* cached = new v8::Global<v8::Context>(iso, ctx);
      iso->SetData(k_typescript_context_slot, cached);
      out = handle_scope.Escape(ctx);
      return {true, {}, {}};
    }

    const char k_transpile_function_source[] = R"JS(
(function(source, fileName, fxeTypes) {
  const normalizedFileName = String(fileName).replace(/\\/g, '/');
  const lastSlash = normalizedFileName.lastIndexOf('/');
  const sourceRoot = lastSlash >= 0 ? normalizedFileName.slice(0, lastSlash + 1) : '';
  const options = {
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.ESNext,
    noLib: true,
    skipLibCheck: true,
    isolatedModules: false,
    preserveConstEnums: false,
    noEmitOnError: false,
    removeComments: false,
    sourceMap: false,
    inlineSourceMap: true,
    inlineSources: true,
    sourceRoot,
    // JSX/TSX support: emit the automatic runtime (importSource defaults to
    // fxe-ui and can be overridden per file via `/** @jsxImportSource X */`).
    jsx: ts.JsxEmit.ReactJSX,
    jsxImportSource: 'fxe-ui',
    // Allow .jsx alongside .tsx so authors can use either extension.
    allowJs: true,
  };

  let outputText = undefined;
  const files = {
    [fileName]: source,
    'fxe.d.ts': fxeTypes,
  };
  const host = {
    getSourceFile(name, languageVersion) {
      return Object.prototype.hasOwnProperty.call(files, name)
        ? ts.createSourceFile(name, files[name], languageVersion, true)
        : undefined;
    },
    getDefaultLibFileName() { return 'lib.d.ts'; },
    writeFile(name, text) {
      if (name.endsWith('.js')) outputText = text;
    },
    getCurrentDirectory() { return '.'; },
    getDirectories() { return []; },
    fileExists(name) { return Object.prototype.hasOwnProperty.call(files, name); },
    readFile(name) { return files[name]; },
    getCanonicalFileName(name) { return name; },
    useCaseSensitiveFileNames() { return true; },
    getNewLine() { return '\n'; },
  };

  const program = ts.createProgram([fileName, 'fxe.d.ts'], options, host);
  const sourceFile = program.getSourceFile(fileName);
  const emitResult = program.emit(sourceFile);
  const diagnostics = [
    ...program.getSyntacticDiagnostics(sourceFile),
    ...emitResult.diagnostics,
  ];
  if (diagnostics.length !== 0) {
    const message = diagnostics.map((diagnostic) => {
      let location = fileName;
      if (diagnostic.file && typeof diagnostic.start === 'number') {
        const pos = diagnostic.file.getLineAndCharacterOfPosition(diagnostic.start);
        location += `:${pos.line + 1}:${pos.character + 1}`;
      }
      return `${location} TS${diagnostic.code}: ${ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')}`;
    }).join('\n');
    return { ok: false, message };
  }
  if (emitResult.emitSkipped || typeof outputText !== 'string') {
    return { ok: false, message: `${fileName}: TypeScript emit produced no JavaScript` };
  }
  return { ok: true, outputText };
})
)JS";

    // Tiny prelude prepended to every transpiled TS module so user code can
    // call `import.meta.hot.accept(handler)` (or the lower-level
    // `globalThis.__fxe_hmr.accept(modulePath, handler)`) without checking
    // for runtime presence. The prelude is idempotent.
    constexpr const char* k_hmr_prelude = R"JS(
/* fxe HMR prelude (auto-injected) */
globalThis.__fxe_hmr ??= (() => {
  const handlers = Object.create(null);
  return {
    handlers,
    accept(path, handler) {
      if (typeof path === 'string' && typeof handler === 'function') handlers[path] = handler;
    },
    fire(path) {
      const h = handlers[path];
      if (typeof h === 'function') { try { h(); } catch (e) { console.error('fxe hmr:', e); } }
    },
  };
})();
)JS";
  } // namespace

  bool is_typescript_path(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    if (filename.size() >= 5 && filename.substr(filename.size() - 5) == ".d.ts")
      return false;
    const auto ext = path.extension().string();
    return ext == ".ts" || ext == ".mts" || ext == ".cts" || ext == ".tsx" || ext == ".jsx";
  }

  typescript_transpile_result transpile_typescript(v8::Isolate* iso, std::string_view source,
                                                   std::string_view origin) {
    if (!iso)
      return {false, {}, "TypeScript transpile requested without a V8 isolate"};

    v8::HandleScope handle_scope(iso);
    v8::Local<v8::Context> compiler_context;
    auto compiler = get_compiler_context(iso, compiler_context);
    if (!compiler.ok)
      return compiler;

    v8::Context::Scope context_scope(compiler_context);
    v8::TryCatch try_catch(iso);

    v8::ScriptOrigin function_origin(str(iso, "<fxe-typescript-transpile>"));
    v8::Local<v8::Script> function_script;
    if (!v8::Script::Compile(compiler_context, str(iso, k_transpile_function_source),
                             &function_origin)
             .ToLocal(&function_script)) {
      return {false,
              {},
              exception_message(iso, compiler_context, try_catch,
                                "failed to compile TypeScript transpile bridge: ")};
    }

    v8::Local<v8::Value> function_value;
    if (!function_script->Run(compiler_context).ToLocal(&function_value) ||
        !function_value->IsFunction()) {
      return {false,
              {},
              exception_message(iso, compiler_context, try_catch,
                                "failed to initialize TypeScript transpile bridge: ")};
    }

    v8::Local<v8::Value> argv[] = {str(iso, source), str(iso, origin),
                                   str(iso, k_fxe_types_source)};
    v8::Local<v8::Value> result_value;
    if (!function_value.As<v8::Function>()
             ->Call(compiler_context, compiler_context->Global(), 3, argv)
             .ToLocal(&result_value)) {
      return {false,
              {},
              exception_message(iso, compiler_context, try_catch, "TypeScript transpile failed: ")};
    }
    if (!result_value->IsObject())
      return {false, {}, "TypeScript transpile bridge returned a non-object result"};

    auto result = result_value.As<v8::Object>();
    v8::Local<v8::Value> ok_value;
    if (!result->Get(compiler_context, str(iso, "ok")).ToLocal(&ok_value) ||
        !ok_value->BooleanValue(iso)) {
      v8::Local<v8::Value> message_value;
      std::string message = "unknown TypeScript transpile failure";
      if (result->Get(compiler_context, str(iso, "message")).ToLocal(&message_value))
        message = to_std_string(iso, message_value);
      return {false, {}, std::move(message)};
    }

    v8::Local<v8::Value> output_value;
    if (!result->Get(compiler_context, str(iso, "outputText")).ToLocal(&output_value) ||
        !output_value->IsString()) {
      return {false, {}, "TypeScript transpile bridge returned no outputText"};
    }
    std::string emitted = to_std_string(iso, output_value);
    const int source_map_line_offset = static_cast<int>(
        std::count(k_hmr_prelude, k_hmr_prelude + std::strlen(k_hmr_prelude), '\n'));
    std::string with_prelude;
    with_prelude.reserve(std::strlen(k_hmr_prelude) + emitted.size());
    with_prelude.append(k_hmr_prelude);
    with_prelude.append(emitted);
    return {true, std::move(with_prelude), {}, source_map_line_offset};
  }

  namespace {
    int b64_value(char c) {
      if (c >= 'A' && c <= 'Z')
        return c - 'A';
      if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
      if (c >= '0' && c <= '9')
        return c - '0' + 52;
      if (c == '+')
        return 62;
      if (c == '/')
        return 63;
      return -1;
    }

    std::string b64_decode(std::string_view in) {
      std::string out;
      out.reserve((in.size() / 4) * 3);
      int buf = 0, bits = 0;
      for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
          continue;
        int v = b64_value(c);
        if (v < 0)
          return {};
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          out.push_back(static_cast<char>((buf >> bits) & 0xff));
        }
      }
      return out;
    }
  } // namespace

  std::string extract_inline_source_map(std::string_view js) {
    constexpr std::string_view k_marker = "//# sourceMappingURL=data:application/json";
    auto pos = js.rfind(k_marker);
    if (pos == std::string_view::npos)
      return {};
    auto comma = js.find(',', pos);
    if (comma == std::string_view::npos)
      return {};
    auto end = js.find_first_of("\r\n", comma);
    auto payload =
        js.substr(comma + 1, (end == std::string_view::npos ? js.size() : end) - comma - 1);
    // Detect ;base64 token to know whether to base64-decode.
    auto header = js.substr(pos, comma - pos);
    if (header.find(";base64") != std::string_view::npos)
      return b64_decode(payload);
    return std::string(payload);
  }

  void dispose_typescript_compiler(v8::Isolate* iso) noexcept {
    if (!iso)
      return;
    auto* cached = static_cast<v8::Global<v8::Context>*>(iso->GetData(k_typescript_context_slot));
    if (!cached)
      return;
    cached->Reset();
    delete cached;
    iso->SetData(k_typescript_context_slot, nullptr);
  }
} // namespace fxe::js
