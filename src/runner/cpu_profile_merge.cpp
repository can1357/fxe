#include "cpu_profile_merge.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fxe/types.hpp>
#include <unordered_map>

namespace fxe::runner {

  namespace {

    using json = nlohmann::ordered_json;

    int read_int(const json& j, const char* key, int defv = 0) {
      auto it = j.find(key);
      if (it == j.end() || it->is_null())
        return defv;
      if (it->is_number_integer())
        return it->get<int>();
      if (it->is_number_unsigned())
        return static_cast<int>(it->get<u64>());
      if (it->is_number_float())
        return static_cast<int>(it->get<double>());
      return defv;
    }

    i64 read_i64(const json& j, const char* key, i64 defv = 0) {
      auto it = j.find(key);
      if (it == j.end() || it->is_null())
        return defv;
      if (it->is_number_integer())
        return it->get<i64>();
      if (it->is_number_unsigned())
        return static_cast<i64>(it->get<u64>());
      if (it->is_number_float())
        return static_cast<i64>(it->get<double>());
      return defv;
    }

    std::string read_str(const json& j, const char* key) {
      auto it = j.find(key);
      if (it == j.end() || !it->is_string())
        return {};
      return it->get<std::string>();
    }

    // V8's CpuProfile::Serialize sometimes emits raw regex bodies inside
    // function-name strings without escaping the backslash, e.g.
    //   "functionName": "RegExp: ^class\s"
    // which is not valid JSON. Walk the input and double any backslash
    // that doesn't introduce one of JSON's recognized escape sequences.
    std::string sanitize_v8_json(std::string_view src) {
      std::string out;
      out.reserve(src.size());
      bool in_string = false;
      for (usize i = 0; i < src.size(); ++i) {
        char c = src[i];
        out.push_back(c);
        if (c == '"') {
          in_string = !in_string;
          continue;
        }
        if (!in_string)
          continue;
        if (c == '\\' && i + 1 < src.size()) {
          char n = src[i + 1];
          if (n == '"' || n == '\\' || n == '/' || n == 'b' || n == 'f' || n == 'n' || n == 'r' ||
              n == 't' || n == 'u') {
            // Valid escape: copy as-is.
            out.push_back(n);
            ++i;
          } else {
            // Invalid escape (e.g. \s, \d, \., \w): double the backslash so
            // the parser treats the original byte as data.
            out.push_back('\\');
          }
        }
      }
      return out;
    }

  } // namespace

  bool parse_v8_profile(std::string_view jstr, profile_data& out, std::string& err) {
    try {
      auto j = json::parse(sanitize_v8_json(jstr));
      out = profile_data{};

      const auto& jnodes = j.at("nodes");
      out.nodes.reserve(jnodes.size());
      for (const auto& jn : jnodes) {
        profile_node n;
        n.id = read_int(jn, "id");
        if (auto it = jn.find("callFrame"); it != jn.end()) {
          n.frame.function_name = read_str(*it, "functionName");
          n.frame.url = read_str(*it, "url");
          // scriptId is sometimes a string, sometimes a number in CDP.
          if (auto sit = it->find("scriptId"); sit != it->end()) {
            if (sit->is_string()) {
              try {
                n.frame.script_id = std::stoi(sit->get<std::string>());
              } catch (...) {
                n.frame.script_id = 0;
              }
            } else if (sit->is_number()) {
              n.frame.script_id = sit->get<int>();
            }
          }
          n.frame.line_number = read_int(*it, "lineNumber", -1);
          n.frame.column_number = read_int(*it, "columnNumber", -1);
        }
        n.hit_count = static_cast<u32>(read_int(jn, "hitCount"));
        if (auto cit = jn.find("children"); cit != jn.end() && cit->is_array()) {
          n.children.reserve(cit->size());
          for (const auto& c : *cit)
            n.children.push_back(c.get<int>());
        }
        out.nodes.push_back(std::move(n));
      }

      if (auto it = j.find("samples"); it != j.end() && it->is_array()) {
        out.samples.reserve(it->size());
        for (const auto& s : *it)
          out.samples.push_back(s.get<int>());
      }
      if (auto it = j.find("timeDeltas"); it != j.end() && it->is_array()) {
        out.time_deltas.reserve(it->size());
        for (const auto& d : *it)
          out.time_deltas.push_back(d.get<i64>());
      }
      out.start_time = read_i64(j, "startTime");
      out.end_time = read_i64(j, "endTime");
      // V8 emits per-sample timeDeltas in microseconds; their mean is the
      // effective sampling period. Fall back to V8's default 1000 µs.
      if (!out.time_deltas.empty()) {
        i64 total = 0;
        usize counted = 0;
        for (auto d : out.time_deltas) {
          if (d > 0) {
            total += d;
            ++counted;
          }
        }
        out.sample_period_us = counted ? (total / static_cast<i64>(counted)) : 1000;
      } else {
        out.sample_period_us = 1000;
      }
      return true;
    } catch (const std::exception& e) {
      err = std::string("V8 profile parse failed: ") + e.what();
      return false;
    }
  }

  profile_data merge_profiles(const profile_data& js, const profile_data& native) {
    profile_data out;
    // Synthetic root + two subtree-roots regardless of which side is empty,
    // so DevTools always sees a stable shape.
    out.nodes.reserve(1 + 2 + js.nodes.size() + native.nodes.size());

    profile_node root;
    root.id = 1;
    root.frame.function_name = "(root)";
    out.nodes.push_back(std::move(root));

    auto append_subtree = [&](const profile_data& src, const char* label) -> int {
      int subtree_root_id = static_cast<int>(out.nodes.size()) + 1;
      profile_node header;
      header.id = subtree_root_id;
      header.frame.function_name = label;
      out.nodes.push_back(std::move(header));
      out.nodes[0].children.push_back(subtree_root_id);

      if (src.nodes.empty())
        return subtree_root_id;

      // Map old src-id -> new merged-id. Preserve src's existing ids by
      // simply offsetting; the src root becomes a child of the labeled
      // header.
      const int offset = subtree_root_id; // src id 1 -> subtree_root_id+1
      auto remap = [offset](int id) { return id + offset; };

      for (const auto& sn : src.nodes) {
        profile_node n;
        n.id = remap(sn.id);
        n.frame = sn.frame;
        n.hit_count = sn.hit_count;
        n.children.reserve(sn.children.size());
        for (int c : sn.children)
          n.children.push_back(remap(c));
        out.nodes.push_back(std::move(n));
      }
      // src.nodes[0] is conventionally its root: hook it under the header.
      if (!src.nodes.empty())
        out.nodes[static_cast<usize>(subtree_root_id - 1)].children.push_back(
            remap(src.nodes.front().id));

      // Copy samples; replace per-sample deltas with a flat sample_period
      // so self/total math doesn't get distorted by inter-sample sleeps,
      // and so two profiles whose absolute clocks live on different
      // epochs (V8 vs CLOCK_MONOTONIC) can be concatenated cleanly.
      const i64 period = src.sample_period_us > 0 ? src.sample_period_us : 1000;
      out.samples.reserve(out.samples.size() + src.samples.size());
      out.time_deltas.reserve(out.time_deltas.size() + src.samples.size());
      for (int s : src.samples) {
        out.samples.push_back(remap(s));
        out.time_deltas.push_back(period);
      }
      return subtree_root_id;
    };

    (void)append_subtree(js, "(js)");
    (void)append_subtree(native, "(native)");

    out.dropped_samples = js.dropped_samples + native.dropped_samples;

    // Synthesize a clean timeline. Absolute timestamps are unreliable when
    // mixing V8's startTime counter with the native sampler's
    // CLOCK_MONOTONIC clock; build the merged profile as
    //   start = 0, end = sum(time_deltas)
    // which is exactly what cpuprofile readers integrate over anyway.
    out.start_time = 0;
    out.end_time = 0;
    for (auto d : out.time_deltas)
      out.end_time += d;
    out.sample_period_us = std::max<i64>(js.sample_period_us, native.sample_period_us);
    return out;
  }

  std::string serialize_cpuprofile(const profile_data& p) {
    json j = json::object();
    json jnodes = json::array();
    jnodes.get_ref<json::array_t&>().reserve(p.nodes.size());
    for (const auto& n : p.nodes) {
      json jn = json::object();
      jn["id"] = n.id;
      json cf = json::object();
      cf["functionName"] = n.frame.function_name;
      cf["scriptId"] = std::to_string(n.frame.script_id);
      cf["url"] = n.frame.url;
      cf["lineNumber"] = n.frame.line_number;
      cf["columnNumber"] = n.frame.column_number;
      jn["callFrame"] = std::move(cf);
      jn["hitCount"] = n.hit_count;
      json ch = json::array();
      for (int c : n.children)
        ch.push_back(c);
      jn["children"] = std::move(ch);
      jnodes.push_back(std::move(jn));
    }
    j["nodes"] = std::move(jnodes);

    json jsamp = json::array();
    for (int s : p.samples)
      jsamp.push_back(s);
    j["samples"] = std::move(jsamp);

    json jdel = json::array();
    for (auto d : p.time_deltas)
      jdel.push_back(d);
    j["timeDeltas"] = std::move(jdel);

    j["startTime"] = p.start_time;
    j["endTime"] = p.end_time;
    return j.dump();
  }

  // ---- Markdown ----------------------------------------------------------

  namespace {

    struct agg {
      std::string function_name;
      std::string url;
      i64 self_us = 0;
      i64 total_us = 0;
      // Best-effort representative source line for the function/url
      // pair. Set on first non-empty contribution; reset to -1 if
      // a subsequent node instance disagrees, so the column never
      // misrepresents a single canonical location.
      int line = -1;
      bool line_set = false;
    };

    // Synthetic CDP frames that represent V8/state, not user code:
    // (root), (idle), (program), (garbage collector), (native), (js),
    // (parser), (compiler), …  We keep them in the leaderboards (they
    // explain idle vs. busy time) but exclude them from the JS-only
    // and Hot files views.
    bool is_synthetic_frame(std::string_view fn) {
      return fn.size() >= 2 && fn.front() == '(' && fn.back() == ')';
    }

    bool is_native_frame(std::string_view fn) {
      return fn.rfind("[native] ", 0) == 0;
    }

    std::string format_us(i64 us) {
      char buf[64];
      if (us >= 1'000'000)
        std::snprintf(buf, sizeof(buf), "%.2f s", static_cast<double>(us) / 1e6);
      else if (us >= 1'000)
        std::snprintf(buf, sizeof(buf), "%.2f ms", static_cast<double>(us) / 1e3);
      else
        std::snprintf(buf, sizeof(buf), "%lld us", static_cast<long long>(us));
      return buf;
    }

    std::string format_pct(i64 num, i64 den) {
      if (den <= 0)
        return "0.0%";
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%.1f%%",
                    100.0 * static_cast<double>(num) / static_cast<double>(den));
      return buf;
    }

    std::string md_escape(std::string_view s) {
      std::string out;
      out.reserve(s.size());
      for (char c : s) {
        if (c == '|')
          out.append("\\|");
        else if (c == '\n' || c == '\r')
          out.push_back(' ');
        else
          out.push_back(c);
      }
      return out;
    }

    std::string short_url(const std::string& url) {
      if (url.empty())
        return "—";
      auto slash = url.find_last_of('/');
      if (slash == std::string::npos)
        return url;
      return url.substr(slash + 1);
    }

  } // namespace

  std::string render_markdown(const profile_data& p, int top_n) {
    // Sampling interval is the average gap between consecutive samples.
    // We treat each sample as accounting for one interval of self time on
    // its leaf node. timeDeltas[i] gives the gap *between* sample i and
    // sample i-1 in CDP convention; we use it directly per sample.
    std::vector<i64> self_us_by_node(p.nodes.size(), 0);
    if (!p.samples.empty()) {
      // Mean gap, used as fallback for samples with delta=0 or negative.
      i64 total = 0;
      usize counted = 0;
      for (auto d : p.time_deltas) {
        if (d > 0) {
          total += d;
          ++counted;
        }
      }
      i64 mean = counted ? total / static_cast<i64>(counted) : 1000;

      for (usize i = 0; i < p.samples.size(); ++i) {
        int id = p.samples[i];
        i64 d = (i < p.time_deltas.size()) ? p.time_deltas[i] : mean;
        if (d <= 0)
          d = mean;
        usize idx = static_cast<usize>(id - 1);
        if (idx < self_us_by_node.size())
          self_us_by_node[idx] += d;
      }
    }
    i64 total_us = 0;
    for (auto v : self_us_by_node)
      total_us += v;

    // Compute total (subtree-inclusive) time per node via a single
    // post-order traversal. nodes are not guaranteed topologically sorted,
    // but the children-graph is a DAG (tree); use iterative DFS from the
    // root (id 1).
    std::vector<i64> total_us_by_node(p.nodes.size(), 0);
    std::vector<int> order;
    order.reserve(p.nodes.size());
    {
      std::vector<int> stack;
      std::vector<char> visited(p.nodes.size(), 0);
      if (!p.nodes.empty())
        stack.push_back(p.nodes.front().id);
      // Iterative post-order: push twice, second visit emits.
      std::vector<std::pair<int, bool>> wk;
      if (!p.nodes.empty())
        wk.push_back({p.nodes.front().id, false});
      while (!wk.empty()) {
        auto [id, done] = wk.back();
        wk.pop_back();
        usize idx = static_cast<usize>(id - 1);
        if (idx >= p.nodes.size())
          continue;
        if (done) {
          order.push_back(id);
          continue;
        }
        if (visited[idx])
          continue;
        visited[idx] = 1;
        wk.push_back({id, true});
        for (int c : p.nodes[idx].children)
          wk.push_back({c, false});
      }
      for (int id : order) {
        usize idx = static_cast<usize>(id - 1);
        i64 t = self_us_by_node[idx];
        for (int c : p.nodes[idx].children) {
          usize cidx = static_cast<usize>(c - 1);
          if (cidx < total_us_by_node.size())
            t += total_us_by_node[cidx];
        }
        total_us_by_node[idx] = t;
      }
    }

    // Aggregate by (functionName | url) across all nodes.
    struct key_t {
      std::string fn;
      std::string url;
      bool operator==(const key_t& o) const {
        return fn == o.fn && url == o.url;
      }
    };
    struct key_hash {
      usize operator()(const key_t& k) const noexcept {
        return std::hash<std::string>{}(k.fn) ^ (std::hash<std::string>{}(k.url) << 1);
      }
    };
    std::unordered_map<key_t, agg, key_hash> by_fn;
    by_fn.reserve(p.nodes.size());
    for (usize i = 0; i < p.nodes.size(); ++i) {
      const auto& n = p.nodes[i];
      key_t k{n.frame.function_name, n.frame.url};
      auto& a = by_fn[k];
      a.function_name = n.frame.function_name;
      a.url = n.frame.url;
      a.self_us += self_us_by_node[i];
      // Capture a representative source line. CDP lineNumber is 0-based;
      // we display 1-based so editors jump correctly. Reset to -1 if
      // multiple nodes for the same key disagree.
      if (n.frame.line_number >= 0) {
        if (!a.line_set) {
          a.line = n.frame.line_number + 1;
          a.line_set = true;
        } else if (a.line != n.frame.line_number + 1) {
          a.line = -1;
        }
      }
      // Total time is summed inclusively; aggregating across multiple node
      // instances of the same fn would double-count if one fn appears as
      // an ancestor of itself (recursion). We sum total only over nodes
      // that are NOT descendants of another node with the same key.
      // A precise treatment would require call-tree dominators; the
      // pragmatic approach: count totals only where the parent has a
      // different key.
    }

    // Totals: walk each sample's stack from leaf to root and add the
    // sample's period to each *unique* (functionName, url) key on that
    // stack. This avoids inflating recursive functions or repeated
    // synthetic frames past 100% of accounted time.
    {
      std::vector<int> parent(p.nodes.size(), 0);
      for (usize i = 0; i < p.nodes.size(); ++i)
        for (int c : p.nodes[i].children)
          if (static_cast<usize>(c - 1) < parent.size())
            parent[static_cast<usize>(c - 1)] = p.nodes[i].id;

      std::unordered_map<key_t, char, key_hash> seen;
      seen.reserve(64);
      for (usize si = 0; si < p.samples.size(); ++si) {
        int leaf = p.samples[si];
        i64 period = (si < p.time_deltas.size() && p.time_deltas[si] > 0)
                         ? p.time_deltas[si]
                         : (p.sample_period_us > 0 ? p.sample_period_us : 1000);
        seen.clear();
        int cur = leaf;
        while (cur > 0 && static_cast<usize>(cur - 1) < p.nodes.size()) {
          const auto& n = p.nodes[static_cast<usize>(cur - 1)];
          key_t k{n.frame.function_name, n.frame.url};
          if (seen.emplace(k, 1).second)
            by_fn[k].total_us += period;
          int pid = parent[static_cast<usize>(cur - 1)];
          if (pid == cur)
            break;
          cur = pid;
        }
      }
    }

    std::vector<agg> rows;
    rows.reserve(by_fn.size());
    for (auto& [k, v] : by_fn)
      rows.push_back(v);

    std::string out;
    out.append("# CPU profile\n\n");
    {
      i64 period =
          p.sample_period_us > 0 ? p.sample_period_us : (p.samples.empty() ? 0 : 1000);
      double hz = period > 0 ? 1e6 / static_cast<double>(period) : 0.0;
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "- duration: %s\n"
                    "- samples: %zu",
                    format_us(p.end_time - p.start_time).c_str(), p.samples.size());
      out.append(buf);
      if (period > 0) {
        std::snprintf(buf, sizeof(buf), " @ ~%.0f Hz (period %lld µs)", hz,
                      static_cast<long long>(period));
        out.append(buf);
      }
      out.append("\n");
      std::snprintf(buf, sizeof(buf),
                    "- nodes: %zu\n"
                    "- dropped (sampler overflow): %llu\n"
                    "- accounted self-time: %s\n\n",
                    p.nodes.size(), static_cast<unsigned long long>(p.dropped_samples),
                    format_us(total_us).c_str());
      out.append(buf);
    }

    // ---- Time breakdown by top-level subtree / synthetic frame ----------
    //
    // The merger plants two named subtree headers under the synthetic root
    // ((js) and (native)); V8 itself emits synthetic leaves like (program),
    // (idle), (garbage collector). Surfacing both up-front lets readers
    // sanity-check whether the run was JS-bound, native-bound, or just
    // sleeping in mach_msg.
    {
      out.append("## Time breakdown\n\n");
      out.append("| bucket | total | total% |\n");
      out.append("|---|---:|---:|\n");
      auto emit = [&](const char* label, i64 us) {
        char line[256];
        std::snprintf(line, sizeof(line), "| %s | %s | %s |\n", label, format_us(us).c_str(),
                      format_pct(us, total_us).c_str());
        out.append(line);
      };
      // Subtree headers: direct children of the (root) node.
      if (!p.nodes.empty()) {
        for (int cid : p.nodes.front().children) {
          usize idx = static_cast<usize>(cid - 1);
          if (idx >= p.nodes.size())
            continue;
          const auto& c = p.nodes[idx];
          if (c.frame.function_name == "(js)" || c.frame.function_name == "(native)") {
            char label[64];
            std::snprintf(label, sizeof(label), "%s subtree", c.frame.function_name.c_str());
            emit(label, total_us_by_node[idx]);
          }
        }
      }
      // Synthetic V8 buckets, summed across every node carrying that name.
      static const char* kSynthetic[] = {"(program)", "(idle)", "(garbage collector)", "(parser)",
                                         "(compiler)"};
      for (const char* name : kSynthetic) {
        i64 sum = 0;
        for (usize i = 0; i < p.nodes.size(); ++i)
          if (p.nodes[i].frame.function_name == name)
            sum += self_us_by_node[i];
        if (sum > 0)
          emit(name, sum);
      }
      out.append("\n");
    }

    auto file_cell = [&](const agg& r) -> std::string {
      std::string s = md_escape(short_url(r.url));
      if (r.line > 0 && !r.url.empty()) {
        char tail[32];
        std::snprintf(tail, sizeof(tail), ":%d", r.line);
        s.append(tail);
      }
      return s;
    };

    auto emit_table = [&](const char* title, const std::vector<const agg*>& view) {
      out.append("## ").append(title).append("\n\n");
      out.append("| # | self | self% | total | total% | function | file |\n");
      out.append("|---:|---:|---:|---:|---:|---|---|\n");
      int rank = 0;
      int limit = top_n > 0 ? top_n : static_cast<int>(view.size());
      for (const auto* rp : view) {
        const auto& r = *rp;
        if (r.self_us == 0 && r.total_us == 0)
          continue;
        if (++rank > limit)
          break;
        char line[1024];
        std::snprintf(line, sizeof(line), "| %d | %s | %s | %s | %s | `%s` | %s |\n", rank,
                      format_us(r.self_us).c_str(), format_pct(r.self_us, total_us).c_str(),
                      format_us(r.total_us).c_str(), format_pct(r.total_us, total_us).c_str(),
                      md_escape(r.function_name.empty() ? "(anonymous)" : r.function_name).c_str(),
                      file_cell(r).c_str());
        out.append(line);
      }
      out.append("\n");
    };

    auto view_all = [&](auto cmp) {
      std::vector<const agg*> v;
      v.reserve(rows.size());
      for (const auto& r : rows)
        v.push_back(&r);
      std::sort(v.begin(), v.end(), [&](const agg* a, const agg* b) { return cmp(*a, *b); });
      return v;
    };

    emit_table("Top by self time",
               view_all([](const agg& a, const agg& b) { return a.self_us > b.self_us; }));
    emit_table("Top by total time",
               view_all([](const agg& a, const agg& b) { return a.total_us > b.total_us; }));

    // JS-only view: drop synthetic and native frames so language-level
    // hotspots aren't drowned in InterpreterEntryTrampoline / mach_msg.
    {
      std::vector<const agg*> v;
      v.reserve(rows.size());
      for (const auto& r : rows) {
        if (is_synthetic_frame(r.function_name) || is_native_frame(r.function_name))
          continue;
        v.push_back(&r);
      }
      std::sort(v.begin(), v.end(),
                [](const agg* a, const agg* b) { return a->self_us > b->self_us; });
      emit_table("Top by self time (JS only)", v);
    }

    // ---- Hot files: aggregate self by url -------------------------------
    {
      std::unordered_map<std::string, i64> by_url;
      by_url.reserve(rows.size());
      for (const auto& r : rows) {
        if (r.url.empty())
          continue;
        if (is_synthetic_frame(r.function_name))
          continue;
        by_url[r.url] += r.self_us;
      }
      std::vector<std::pair<std::string, i64>> ranked(by_url.begin(), by_url.end());
      std::sort(ranked.begin(), ranked.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      out.append("## Hot files\n\n");
      out.append("| # | self | self% | file |\n");
      out.append("|---:|---:|---:|---|\n");
      int rank = 0;
      int limit = top_n > 0 ? top_n / 2 : static_cast<int>(ranked.size());
      if (limit < 15)
        limit = 15;
      for (const auto& [url, us] : ranked) {
        if (us == 0)
          continue;
        if (++rank > limit)
          break;
        char line[1024];
        std::snprintf(line, sizeof(line), "| %d | %s | %s | %s |\n", rank, format_us(us).c_str(),
                      format_pct(us, total_us).c_str(), md_escape(short_url(url)).c_str());
        out.append(line);
      }
      out.append("\n");
    }

    // ---- Hot stacks: top leaf-rooted call stacks by sample weight -------
    //
    // Folds samples by their leaf->root frame signature (truncated to a
    // fixed depth so deep ts-compiler stacks don't fragment the table) and
    // ranks by aggregate µs. This is what `flamegraph --collapse` would
    // call a folded-stacks view; the markdown form is just a list of the
    // top N with their leaf-up frames inlined.
    if (!p.samples.empty()) {
      constexpr usize kStackDepth = 8;
      std::vector<int> parent2(p.nodes.size(), 0);
      for (usize i = 0; i < p.nodes.size(); ++i)
        for (int c : p.nodes[i].children)
          if (static_cast<usize>(c - 1) < parent2.size())
            parent2[static_cast<usize>(c - 1)] = p.nodes[i].id;

      struct stack_agg {
        std::vector<std::string> frames; // "fn @ short_url[:line]" leaf-first
        i64 weight_us = 0;
        u64 hits = 0;
      };
      std::unordered_map<std::string, stack_agg> by_stack;
      by_stack.reserve(p.samples.size() / 4 + 16);

      for (usize si = 0; si < p.samples.size(); ++si) {
        int leaf = p.samples[si];
        i64 period = (si < p.time_deltas.size() && p.time_deltas[si] > 0)
                         ? p.time_deltas[si]
                         : (p.sample_period_us > 0 ? p.sample_period_us : 1000);

        std::vector<std::string> frames;
        std::string sig;
        sig.reserve(128);
        int cur = leaf;
        usize hops = 0;
        while (cur > 0 && static_cast<usize>(cur - 1) < p.nodes.size() && hops < kStackDepth) {
          const auto& n = p.nodes[static_cast<usize>(cur - 1)];
          // Skip the (root)/(js)/(native) headers so they don't dominate
          // every stack signature.
          bool skip = (n.frame.function_name == "(root)" || n.frame.function_name == "(js)" ||
                       n.frame.function_name == "(native)");
          if (!skip) {
            std::string label =
                n.frame.function_name.empty() ? "(anonymous)" : n.frame.function_name;
            std::string url_short = short_url(n.frame.url);
            std::string line;
            line.append(label);
            if (!n.frame.url.empty() || n.frame.line_number >= 0) {
              line.append(" @ ").append(url_short);
              if (n.frame.line_number >= 0) {
                char tail[32];
                std::snprintf(tail, sizeof(tail), ":%d", n.frame.line_number + 1);
                line.append(tail);
              }
            }
            sig.append(line).push_back('\x1f');
            frames.push_back(std::move(line));
            ++hops;
          }
          int pid = parent2[static_cast<usize>(cur - 1)];
          if (pid == cur)
            break;
          cur = pid;
        }
        if (frames.empty())
          continue;
        auto& a = by_stack[sig];
        if (a.frames.empty())
          a.frames = std::move(frames);
        a.weight_us += period;
        a.hits += 1;
      }

      std::vector<const stack_agg*> ranked;
      ranked.reserve(by_stack.size());
      for (auto& [k, v] : by_stack)
        ranked.push_back(&v);
      std::sort(ranked.begin(), ranked.end(),
                [](const stack_agg* a, const stack_agg* b) { return a->weight_us > b->weight_us; });

      out.append("## Hot stacks\n\n");
      out.append("Top leaf-rooted call stacks (depth ≤ ");
      out.append(std::to_string(kStackDepth)).append(") aggregated by sample weight.\n\n");
      int limit = 20;
      if (top_n > 0 && top_n / 3 > limit)
        limit = top_n / 3;
      int rank = 0;
      for (const auto* sp : ranked) {
        if (++rank > limit)
          break;
        char head[128];
        std::snprintf(head, sizeof(head), "%d. %s (%s, %llu samples)\n", rank,
                      format_us(sp->weight_us).c_str(), format_pct(sp->weight_us, total_us).c_str(),
                      static_cast<unsigned long long>(sp->hits));
        out.append(head);
        for (const auto& f : sp->frames)
          out.append("   - `").append(md_escape(f)).append("`\n");
      }
      out.append("\n");
    }

    return out;
  }

} // namespace fxe::runner
