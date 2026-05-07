// Performance.timeline implementation. See bind_performance.hpp.
//
// State model: a single process-global `mark_store` keyed by name. Each entry
// holds aggregate stats plus an "open" timestamp set by beginMark and consumed
// by endMark. Mismatched begin/end pairs are tolerated (endMark without an
// open begin is dropped after a once-per-label warning).

#include "bind_performance.hpp"

#include <chrono>
#include <cstdio>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace fxe::js {
  namespace {

    using clock = std::chrono::steady_clock;

    struct mark_entry {
      std::optional<clock::time_point> open;
      uint64_t count = 0;
      double total_ms = 0.0;
      double last_ms = 0.0;
      double min_ms = std::numeric_limits<double>::infinity();
      double max_ms = 0.0;
    };

    struct mark_store {
      std::mutex mu;
      std::unordered_map<std::string, mark_entry> entries;
      std::unordered_set<std::string> unmatched_end_warned;
    };

    mark_store& store() {
      static mark_store s;
      return s;
    }

    std::string to_std_string(v8::Isolate* iso, v8::Local<v8::Value> value) {
      v8::String::Utf8Value u(iso, value);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    void record_locked(mark_entry& e, double ms) {
      e.count += 1;
      e.total_ms += ms;
      e.last_ms = ms;
      if (ms < e.min_ms)
        e.min_ms = ms;
      if (ms > e.max_ms)
        e.max_ms = ms;
    }

    void begin_mark_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      auto* iso = args.GetIsolate();
      if (args.Length() < 1 || !args[0]->IsString()) {
        iso->ThrowError(v8::String::NewFromUtf8Literal(iso, "beginMark: name required"));
        return;
      }
      auto name = to_std_string(iso, args[0]);
      auto& s = store();
      std::lock_guard<std::mutex> g(s.mu);
      auto& e = s.entries[name];
      e.open = clock::now();
    }

    void end_mark_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      auto* iso = args.GetIsolate();
      if (args.Length() < 1 || !args[0]->IsString()) {
        iso->ThrowError(v8::String::NewFromUtf8Literal(iso, "endMark: name required"));
        return;
      }
      auto name = to_std_string(iso, args[0]);
      auto end = clock::now();
      double ms = 0.0;
      {
        auto& s = store();
        std::lock_guard<std::mutex> g(s.mu);
        auto it = s.entries.find(name);
        if (it == s.entries.end() || !it->second.open) {
          if (s.unmatched_end_warned.insert(name).second) {
            std::fprintf(
                stderr, "[fxe] performance.timeline.endMark('%s') ignored: no matching beginMark\n",
                name.c_str());
            std::fflush(stderr);
          }
          return;
        }
        ms = std::chrono::duration<double, std::milli>(end - *it->second.open).count();
        it->second.open.reset();
        record_locked(it->second, ms);
      }
      args.GetReturnValue().Set(ms);
    }

    void snapshot_callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
      auto* iso = args.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto json = performance_timeline_snapshot();
      auto str = json.dump();
      v8::Local<v8::String> v;
      if (!v8::String::NewFromUtf8(iso, str.data(), v8::NewStringType::kNormal,
                                   static_cast<int>(str.size()))
               .ToLocal(&v)) {
        return;
      }
      v8::Local<v8::Value> parsed;
      if (!v8::JSON::Parse(ctx, v).ToLocal(&parsed))
        return;
      args.GetReturnValue().Set(parsed);
    }

  } // namespace

  void install_performance_global(v8::Isolate* iso, v8::Local<v8::Object> global) {
    v8::HandleScope hs(iso);
    auto ctx = iso->GetCurrentContext();
    v8::Local<v8::Value> perf_v;
    if (!global->Get(ctx, v8::String::NewFromUtf8Literal(iso, "performance")).ToLocal(&perf_v) ||
        !perf_v->IsObject()) {
      // No existing `performance`; create a stub host-side caller can extend.
      perf_v = v8::Object::New(iso);
      (void)global->Set(ctx, v8::String::NewFromUtf8Literal(iso, "performance"), perf_v);
    }
    auto perf = perf_v.As<v8::Object>();

    auto begin_fn = v8::Function::New(ctx, begin_mark_callback).ToLocalChecked();
    auto end_fn = v8::Function::New(ctx, end_mark_callback).ToLocalChecked();
    auto snap_fn = v8::Function::New(ctx, snapshot_callback).ToLocalChecked();

    auto timeline = v8::Object::New(iso);
    (void)timeline->Set(ctx, v8::String::NewFromUtf8Literal(iso, "beginMark"), begin_fn);
    (void)timeline->Set(ctx, v8::String::NewFromUtf8Literal(iso, "endMark"), end_fn);
    (void)timeline->Set(ctx, v8::String::NewFromUtf8Literal(iso, "snapshot"), snap_fn);
    (void)perf->Set(ctx, v8::String::NewFromUtf8Literal(iso, "timeline"), timeline);
  }

  void perf_record_sample(std::string_view name, double ms) noexcept {
    auto& s = store();
    std::lock_guard<std::mutex> g(s.mu);
    auto& e = s.entries[std::string(name)];
    record_locked(e, ms);
  }

  nlohmann::ordered_json performance_timeline_snapshot() {
    nlohmann::ordered_json marks = nlohmann::ordered_json::object();
    auto& s = store();
    std::lock_guard<std::mutex> g(s.mu);
    for (auto& [name, e] : s.entries) {
      nlohmann::ordered_json m = nlohmann::ordered_json::object();
      m["count"] = static_cast<double>(e.count);
      m["totalMs"] = e.total_ms;
      m["lastMs"] = e.last_ms;
      m["minMs"] = e.count == 0 ? 0.0 : e.min_ms;
      m["maxMs"] = e.max_ms;
      marks[name] = std::move(m);
    }
    nlohmann::ordered_json out = nlohmann::ordered_json::object();
    out["marks"] = std::move(marks);
    return out;
  }

  void performance_timeline_reset() noexcept {
    auto& s = store();
    std::lock_guard<std::mutex> g(s.mu);
    s.entries.clear();
  }

} // namespace fxe::js
