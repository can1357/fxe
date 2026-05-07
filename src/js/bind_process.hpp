#pragma once
#include <string>
#include <v8.h>
#include <vector>

namespace fxe::js {
  // Set host CLI argv before context creation so `process.argv` reflects it.
  // Safe to call multiple times; the most recent value wins.
  void set_host_argv(std::vector<std::string> argv);

  // Installs the `process` global: argv, env (Proxy-like), cwd/chdir,
  // platform/arch/pid, exit/kill/umask/hrtime, versions/release,
  // stdio.write, on/off, nextTick.
  void install_process_global(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js
