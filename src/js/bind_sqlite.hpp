#pragma once
#include <v8.h>

namespace fxe::js {
  // Type tags stored alongside the C++ pointer in object internal field 1.
  inline constexpr unsigned int TAG_SQLITE_DATABASE = 0x53444220u;  // 'SDB '
  inline constexpr unsigned int TAG_SQLITE_STATEMENT = 0x53535420u; // 'SST '

  // Installs the Database/Statement FunctionTemplate on the isolate global so
  // they share lifecycle with other engine bindings (template resetters, GC
  // finalisers). The synthetic `fxe:sqlite` module pulls these constructors at
  // evaluation time. Call before context creation alongside the other
  // install_*_global helpers.
  void install_sqlite_bindings(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);

  // Build the synthetic ES module for `fxe:sqlite`. Must be called inside a
  // context scope after the bindings have been installed. The returned module
  // exports `Database` (constructor) and `constants` (record of FCNTL codes).
  v8::MaybeLocal<v8::Module> build_sqlite_module(v8::Isolate* iso, v8::Local<v8::Context> ctx);
} // namespace fxe::js
