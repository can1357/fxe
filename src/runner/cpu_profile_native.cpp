#include "cpu_profile_native.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fxe/types.hpp>
#include <unordered_map>

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

namespace fxe::runner {

#if defined(__APPLE__) || defined(__linux__)

  namespace {

    constexpr int kMaxFrames = 96;

    struct sample_slot {
      u64 t_us = 0;
      int n_frames = 0;
      void* frames[kMaxFrames] = {};
    };

    // One global ring per process (we only allow one native_profiler at a
    // time anyway; SIGPROF is a process-wide signal).
    struct sampler_ring {
      sample_slot* slots = nullptr;
      usize cap = 0;
      // Monotonically increasing. Writers (signal handler) cas-increment;
      // readers wait until !active_ then drain head/cap entries.
      std::atomic<usize> head{0};
      std::atomic<u64> dropped{0};
      std::atomic<bool> active{false};
      // Saved sigaction at install time. V8's CpuProfiler also hooks
      // SIGPROF on POSIX; if it was active when we installed, this points
      // at V8's handler so we can chain through after capturing our sample.
      struct sigaction prior{};
      bool have_prior = false;
    };

    sampler_ring& ring() {
      static sampler_ring r;
      return r;
    }

    u64 now_us() noexcept {
      timespec ts{};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return static_cast<u64>(ts.tv_sec) * 1000000ull + static_cast<u64>(ts.tv_nsec) / 1000ull;
    }

    void sigprof_handler(int sig, siginfo_t* info, void* ctx) {
      auto& r = ring();
      if (r.active.load(std::memory_order_acquire)) {
        usize idx = r.head.fetch_add(1, std::memory_order_acq_rel);
        if (idx < r.cap) {
          sample_slot& s = r.slots[idx];
          s.t_us = now_us();
          s.n_frames = backtrace(s.frames, kMaxFrames);
        } else {
          r.dropped.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // Chain to whatever handler was installed before us (typically V8's
      // CpuProfiler) so its sample collector keeps running. Without this,
      // V8's profile shows up empty whenever native profiling is also on.
      if (r.have_prior) {
        if (r.prior.sa_flags & SA_SIGINFO) {
          if (r.prior.sa_sigaction)
            r.prior.sa_sigaction(sig, info, ctx);
        } else {
          if (r.prior.sa_handler != SIG_IGN && r.prior.sa_handler != SIG_DFL &&
              r.prior.sa_handler != nullptr)
            r.prior.sa_handler(sig);
        }
      }
    }

    void install_handler() {
      struct sigaction sa{};
      sa.sa_sigaction = &sigprof_handler;
      sa.sa_flags = SA_SIGINFO | SA_RESTART;
      sigemptyset(&sa.sa_mask);
      auto& r = ring();
      r.have_prior = (sigaction(SIGPROF, &sa, &r.prior) == 0);
      // Don't chain back into ourselves if for some reason install runs twice.
      if (r.have_prior && (r.prior.sa_flags & SA_SIGINFO) &&
          r.prior.sa_sigaction == &sigprof_handler)
        r.have_prior = false;
    }

    void restore_handler() {
      // Restore whatever was installed before us (typically V8's CpuProfiler
      // handler) so V8 keeps receiving SIGPROF samples until it runs its own
      // teardown. If nothing was previously installed, fall back to SIG_IGN
      // so any in-flight pthread_kill from our just-joined sampler thread —
      // or a signal queued by the kernel while the target thread had SIGPROF
      // transiently masked — doesn't terminate the process.
      auto& r = ring();
      if (r.have_prior) {
        sigaction(SIGPROF, &r.prior, nullptr);
        r.have_prior = false;
      } else {
        struct sigaction sa{};
        sa.sa_handler = SIG_IGN;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
      }
    }

    std::string demangle(const char* sym) {
      if (!sym || !*sym)
        return {};
      int status = 0;
      char* out = abi::__cxa_demangle(sym, nullptr, nullptr, &status);
      if (status == 0 && out) {
        std::string s(out);
        std::free(out);
        return s;
      }
      return std::string(sym);
    }

    // Resolve a single PC to a call_frame. Cached by caller.
    call_frame resolve(void* pc) {
      call_frame f;
      Dl_info di{};
      if (dladdr(pc, &di) != 0) {
        if (di.dli_sname && *di.dli_sname)
          f.function_name = demangle(di.dli_sname);
        if (di.dli_fname && *di.dli_fname)
          f.url = di.dli_fname;
      }
      if (f.function_name.empty()) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "0x%llx",
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pc)));
        f.function_name = std::string("[native] ") + buf;
      } else {
        f.function_name = std::string("[native] ") + f.function_name;
      }
      return f;
    }

  } // namespace

  native_profiler::~native_profiler() {
    if (running_)
      (void)stop();
  }

  bool native_profiler::start(int hz, usize max_samples, std::string& err) {
    if (running_) {
      err = "native profiler already running";
      return false;
    }
    if (hz <= 0)
      hz = 1000;
    if (max_samples == 0)
      max_samples = 65536;

    auto& r = ring();
    if (r.active.load(std::memory_order_acquire)) {
      err = "another native profiler is already active in this process";
      return false;
    }
    r.slots = new (std::nothrow) sample_slot[max_samples];
    if (!r.slots) {
      err = "failed to allocate sampler ring";
      return false;
    }
    r.cap = max_samples;
    r.head.store(0, std::memory_order_relaxed);
    r.dropped.store(0, std::memory_order_relaxed);

    // Pre-warm backtrace() so the first call from inside the signal handler
    // doesn't take the lazy-init path (which can allocate / dlopen).
    {
      void* tmp[8];
      (void)backtrace(tmp, 8);
    }

    target_ = pthread_self();
    install_handler();
    r.active.store(true, std::memory_order_release);

    hz_ = hz;
    stop_flag_.store(false, std::memory_order_relaxed);
    sampler_ = std::thread([this] {
      const auto period = std::chrono::microseconds(1000000 / hz_);
      auto next = std::chrono::steady_clock::now();
      while (!stop_flag_.load(std::memory_order_acquire)) {
        next += period;
        std::this_thread::sleep_until(next);
        // Best-effort: the signal may be lost if the target has SIGPROF
        // masked transiently. We accept that.
        pthread_kill(target_, SIGPROF);
      }
    });

    running_ = true;
    return true;
  }

  profile_data native_profiler::stop() {
    profile_data out;
    if (!running_)
      return out;

    // Install SIG_IGN *before* doing anything else. V8's CPU profiler also
    // hooks SIGPROF on POSIX; if it gets stopped first elsewhere it will
    // restore the handler it saved at start, which may be SIG_DFL. We must
    // make sure no in-flight pthread_kill from our sampler thread can hit
    // SIG_DFL and terminate the process.
    restore_handler();

    stop_flag_.store(true, std::memory_order_release);
    if (sampler_.joinable())
      sampler_.join();

    auto& r = ring();
    r.active.store(false, std::memory_order_release);

    usize produced = r.head.load(std::memory_order_acquire);
    if (produced > r.cap)
      produced = r.cap;
    out.dropped_samples = r.dropped.load(std::memory_order_relaxed);

    // Build root.
    out.nodes.push_back({});
    out.nodes[0].id = 1;
    out.nodes[0].frame.function_name = "(native root)";

    // PC -> resolved call_frame cache.
    std::unordered_map<void*, call_frame> sym_cache;
    sym_cache.reserve(1024);

    // Tree construction. Children are looked up by an index keyed on
    // (parent_id, function_name+url) so we collapse identical frames.
    auto child_key = [](int parent, const call_frame& f) {
      std::string k;
      k.reserve(f.function_name.size() + f.url.size() + 16);
      k.append(std::to_string(parent));
      k.push_back('|');
      k.append(f.function_name);
      k.push_back('|');
      k.append(f.url);
      return k;
    };
    std::unordered_map<std::string, int> child_index;

    auto get_child = [&](int parent, const call_frame& f) -> int {
      auto k = child_key(parent, f);
      auto it = child_index.find(k);
      if (it != child_index.end())
        return it->second;
      profile_node n;
      n.id = static_cast<int>(out.nodes.size()) + 1;
      n.frame = f;
      out.nodes.push_back(std::move(n));
      out.nodes[static_cast<usize>(parent - 1)].children.push_back(out.nodes.back().id);
      child_index.emplace(std::move(k), out.nodes.back().id);
      return out.nodes.back().id;
    };

    // Skip the topmost frames inside our own SIGPROF handler so they don't
    // dominate every leaf in the tree. backtrace(3) returns the immediate
    // signal-trampoline + handler entry as frames[0..1] on macOS/Linux;
    // those are sampling overhead, not application work.
    constexpr int kHandlerSkip = 2;
    out.sample_period_us = (hz_ > 0) ? (1'000'000 / hz_) : 1000;
    out.samples.reserve(produced);
    out.time_deltas.reserve(produced);
    u64 prev_t = 0;
    for (usize i = 0; i < produced; ++i) {
      const sample_slot& s = r.slots[i];
      if (s.n_frames <= kHandlerSkip)
        continue;
      // backtrace(): frames[0] = leaf (innermost), frames[n-1] = oldest.
      // Walk oldest -> innermost so the tree grows from root down. Stop
      // before the kHandlerSkip leaf frames so the handler doesn't show up
      // as the hottest function in every report.
      int cur = out.nodes[0].id;
      for (int f = s.n_frames - 1; f >= kHandlerSkip; --f) {
        auto& cf = sym_cache[s.frames[f]];
        if (cf.function_name.empty())
          cf = resolve(s.frames[f]);
        cur = get_child(cur, cf);
      }
      out.nodes[static_cast<usize>(cur - 1)].hit_count += 1;
      out.samples.push_back(cur);
      // Use a flat per-sample budget (sample_period_us) instead of raw
      // wall-clock gaps. That way self-time is hit_count × period and
      // doesn't get distorted by long sleeps between scheduled samples.
      out.time_deltas.push_back(out.sample_period_us);
      if (i == 0)
        out.start_time = static_cast<i64>(s.t_us);
      prev_t = s.t_us;
      out.end_time = static_cast<i64>(s.t_us);
    }
    (void)prev_t;

    // Free ring.
    delete[] r.slots;
    r.slots = nullptr;
    r.cap = 0;

    running_ = false;
    return out;
  }

#else // !POSIX

  native_profiler::~native_profiler() = default;

  bool native_profiler::start(int /*hz*/, usize /*max_samples*/, std::string& err) {
    err = "native CPU profiling is not implemented on this platform";
    return false;
  }

  profile_data native_profiler::stop() {
    return {};
  }

#endif

} // namespace fxe::runner
