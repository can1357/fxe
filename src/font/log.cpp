// fxe::log implementation. One stderr sink, one logger per category, env-driven
// levels via SPDLOG_LEVEL (or its alias FXE_LOG). See include/fxe/log.hpp.
#include <fxe/log.hpp>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fxe::log {
  namespace {

    std::shared_ptr<spdlog::sinks::sink>& shared_sink() {
      static std::shared_ptr<spdlog::sinks::sink> sink;
      return sink;
    }

    std::unordered_map<std::string, std::shared_ptr<spdlog::logger>>& registry() {
      static std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> r;
      return r;
    }

    std::mutex& registry_mutex() {
      static std::mutex m;
      return m;
    }

    void ensure_initialised_locked() {
      if (shared_sink())
        return;

      // Honour FXE_LOG_LEVELS if SPDLOG_LEVEL isn't set, so callers don't
      // need to know spdlog's env-var name. This must happen before
      // load_env_levels() so the alias takes effect.
      if (std::getenv("SPDLOG_LEVEL") == nullptr) {
        if (const char* v = std::getenv("FXE_LOG_LEVELS"); v && v[0] != '\0') {
#ifdef _WIN32
          _putenv_s("SPDLOG_LEVEL", v);
#else
          setenv("SPDLOG_LEVEL", v, /*overwrite=*/1);
#endif
        }
      }

      shared_sink() = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

      // Default level: warn. Loud diagnostics opt in via SPDLOG_LEVEL.
      // load_env_levels() applies per-logger overrides on top.
      spdlog::set_level(spdlog::level::warn);
      spdlog::cfg::load_env_levels();
    }

    spdlog::logger& get_locked(std::string_view category) {
      ensure_initialised_locked();
      auto& reg = registry();
      const std::string key(category);
      if (auto it = reg.find(key); it != reg.end())
        return *it->second;

      auto logger = std::make_shared<spdlog::logger>(key, shared_sink());
      // Pattern: "[hh:mm:ss.mmm] [category] [level] message"
      logger->set_pattern("[%H:%M:%S.%e] [%n] [%^%l%$] %v");
      // Apply env-driven level (or fall back to spdlog's default level).
      logger->set_level(spdlog::get_level());
      // Re-apply env levels so a per-logger override (e.g. font=debug)
      // wins over the global default.
      spdlog::register_logger(logger);
      spdlog::cfg::load_env_levels();

      reg.emplace(key, logger);
      return *logger;
    }

  } // namespace

  spdlog::logger& get(std::string_view category) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return get_locked(category);
  }

  void init() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    ensure_initialised_locked();
  }

} // namespace fxe::log
