// Merge + serialize + render helpers for the runner's CPU profiles.
//
// V8 hands us a CDP-format JSON blob from CpuProfile::Serialize(). The
// native sampler emits its own structured profile_data. The merger combines
// them under a synthetic root, renumbers ids, and concatenates samples /
// time deltas so the output remains a valid Chrome DevTools .cpuprofile.
//
// render_markdown() builds an agent-friendly summary with self-time and
// total-time leaderboards aggregated by (functionName | url).
#pragma once

#include "cpu_profile.hpp"

#include <string>
#include <string_view>

namespace fxe::runner {

  // Parse a V8 CDP cpuprofile JSON blob into profile_data. Returns false
  // and fills `err` on parse failure.
  bool parse_v8_profile(std::string_view json, profile_data& out, std::string& err);

  // Combine two profiles under a single synthetic root with two children
  // labeled "(js)" and "(native)". Either side may be empty.
  profile_data merge_profiles(const profile_data& js, const profile_data& native);

  // Serialize a profile_data back to a CDP cpuprofile JSON string.
  std::string serialize_cpuprofile(const profile_data& p);

  // Markdown report aggregated by (functionName | url). top_n caps each
  // table's rows; pass <=0 for all rows.
  std::string render_markdown(const profile_data& p, int top_n = 60);

} // namespace fxe::runner
