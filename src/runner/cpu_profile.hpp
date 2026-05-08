// Common types for the runner's merged CPU profile (V8 + native).
//
// The shape mirrors Chrome DevTools' .cpuprofile format so we can serialize
// directly to a file that DevTools and other tooling already understand:
//   { nodes: [{id, callFrame:{...}, hitCount, children:[id,...]}],
//     samples: [id,...], timeDeltas: [us,...],
//     startTime, endTime }
//
// The merger combines V8's CDP profile JSON with samples from a native
// sampling profiler (SIGPROF) into a single tree under a synthetic root.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fxe::runner {

  struct call_frame {
    std::string function_name;
    std::string url;
    int script_id = 0;    // 0 = unknown / native
    int line_number = -1; // CDP convention: 0-based, -1 = unknown
    int column_number = -1;
  };

  struct profile_node {
    int id = 0;
    call_frame frame;
    std::uint32_t hit_count = 0;
    std::vector<int> children;
  };

  struct profile_data {
    std::vector<profile_node> nodes;       // nodes[0] is root by convention
    std::vector<int> samples;              // node ids, in order
    std::vector<std::int64_t> time_deltas; // microseconds between samples
    std::int64_t start_time = 0;           // microseconds (monotonic)
    std::int64_t end_time = 0;
    std::uint64_t dropped_samples = 0; // sampler dropped (overflow)
    // Average per-sample period; lets renderers reason about self time
    // without trusting wall-clock stamps that may live on different
    // epochs (V8's startTime counter vs CLOCK_MONOTONIC).
    std::int64_t sample_period_us = 0;
  };

} // namespace fxe::runner
