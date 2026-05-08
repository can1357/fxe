#include "source_map.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <system_error>
#include <utility>

#include <fxe/types.hpp>
#include <nlohmann/json.hpp>

namespace fxe::js {
  namespace {
    int base64_vlq_value(char c) {
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

    bool decode_vlq(std::string_view mappings, usize& index, int& out) {
      int shift = 0;
      int value = 0;
      bool saw_digit = false;
      while (index < mappings.size()) {
        const int digit = base64_vlq_value(mappings[index++]);
        if (digit < 0)
          return false;
        saw_digit = true;
        const bool continuation = (digit & 0x20) != 0;
        value |= (digit & 0x1f) << shift;
        shift += 5;
        if (shift > 30)
          return false;
        if (!continuation)
          break;
      }
      if (!saw_digit)
        return false;
      const bool negative = (value & 1) != 0;
      value >>= 1;
      out = negative ? -value : value;
      return true;
    }

    std::string normalize_slashes(std::string value) {
      for (auto& c : value) {
        if (c == '\\')
          c = '/';
      }
      return value;
    }

    bool has_file_scheme(std::string_view url) {
      return url.starts_with("file://");
    }

    std::string strip_file_scheme(std::string_view url) {
      if (!has_file_scheme(url))
        return std::string(url);
      auto path = url.substr(7);
      if (path.starts_with("localhost/"))
        path.remove_prefix(9);
      return std::string(path);
    }

    std::string resolve_source_path(std::string_view module_url, std::string_view source) {
      std::string source_text = strip_file_scheme(source);
      source_text = normalize_slashes(std::move(source_text));
      if (source_text.empty())
        return source_text;
      if (source_text.starts_with("/") || source_text.starts_with("<"))
        return source_text;

      const std::string module_path = strip_file_scheme(module_url);
      if (module_path.empty() || module_path.starts_with("<"))
        return source_text;

      std::filesystem::path resolved =
          std::filesystem::path(module_path).parent_path() / std::filesystem::path(source_text);
      std::error_code ec;
      auto canonical = std::filesystem::weakly_canonical(resolved, ec);
      if (!ec)
        resolved = canonical;
      return normalize_slashes(resolved.lexically_normal().string());
    }

    std::string apply_source_root(std::string_view source_root, std::string_view source) {
      if (source_root.empty() || source.empty() || source.starts_with("/") ||
          source.starts_with("<") || has_file_scheme(source)) {
        return std::string(source);
      }
      std::string out = normalize_slashes(std::string(source_root));
      if (!out.empty() && out.back() != '/')
        out.push_back('/');
      out.append(source);
      return out;
    }
  } // namespace

  std::string normalize_source_map_url(std::string_view url) {
    std::string text = strip_file_scheme(url);
    text = normalize_slashes(std::move(text));
    if (text.empty() || text.starts_with("<"))
      return text;

    std::filesystem::path path(text);
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
      path = canonical;
    return normalize_slashes(path.lexically_normal().string());
  }

  std::optional<source_map_decoder> source_map_decoder::parse(std::string_view module_url,
                                                              std::string_view source_map_json,
                                                              int generated_line_offset) {
    nlohmann::json parsed;
    try {
      parsed = nlohmann::json::parse(source_map_json.begin(), source_map_json.end());
    } catch (const nlohmann::json::parse_error&) {
      return std::nullopt;
    }

    const auto mappings_it = parsed.find("mappings");
    if (mappings_it == parsed.end() || !mappings_it->is_string())
      return std::nullopt;

    source_map_decoder decoder;
    decoder.module_url_ = normalize_source_map_url(module_url);
    decoder.generated_line_offset_ = std::max(generated_line_offset, 0);

    std::string source_root;
    if (const auto source_root_it = parsed.find("sourceRoot");
        source_root_it != parsed.end() && source_root_it->is_string()) {
      source_root = source_root_it->get<std::string>();
    }
    if (const auto sources_it = parsed.find("sources");
        sources_it != parsed.end() && sources_it->is_array()) {
      decoder.sources_.reserve(sources_it->size());
      for (const auto& source : *sources_it) {
        decoder.sources_.push_back(source.is_string()
                                       ? apply_source_root(source_root, source.get<std::string>())
                                       : std::string{});
      }
    }
    if (const auto names_it = parsed.find("names");
        names_it != parsed.end() && names_it->is_array()) {
      decoder.names_.reserve(names_it->size());
      for (const auto& name : *names_it) {
        decoder.names_.push_back(name.is_string() ? name.get<std::string>() : std::string{});
      }
    }

    const std::string mappings = mappings_it->get<std::string>();
    int source_index = 0;
    int source_line = 0;
    int source_column = 0;
    int name_index = 0;
    int generated_column = 0;
    std::vector<segment> current_line;

    for (usize i = 0; i <= mappings.size();) {
      if (i == mappings.size() || mappings[i] == ';' || mappings[i] == ',') {
        const char separator = i < mappings.size() ? mappings[i] : ';';
        if (separator == ';') {
          decoder.mappings_.push_back(std::move(current_line));
          current_line.clear();
          generated_column = 0;
        }
        if (i == mappings.size())
          break;
        ++i;
        continue;
      }

      int fields[5] = {0, 0, 0, 0, 0};
      int field_count = 0;
      while (field_count < 5 && i < mappings.size() && mappings[i] != ',' && mappings[i] != ';') {
        if (!decode_vlq(mappings, i, fields[field_count]))
          return std::nullopt;
        ++field_count;
      }
      if (field_count != 1 && field_count != 4 && field_count != 5)
        return std::nullopt;

      generated_column += fields[0];
      segment seg;
      seg.generated_column = generated_column;
      if (field_count >= 4) {
        source_index += fields[1];
        source_line += fields[2];
        source_column += fields[3];
        seg.source_index = source_index;
        seg.source_line = source_line;
        seg.source_column = source_column;
      }
      if (field_count == 5) {
        name_index += fields[4];
        seg.name_index = name_index;
      }
      current_line.push_back(seg);
    }

    if (decoder.mappings_.empty() || decoder.sources_.empty())
      return std::nullopt;
    return decoder;
  }

  std::optional<source_mapped_position>
  source_map_decoder::original_position(int generated_line, int generated_column) const {
    const int zero_based_line = generated_line - 1 - generated_line_offset_;
    if (zero_based_line < 0)
      return std::nullopt;
    const auto line_index = static_cast<usize>(zero_based_line);
    if (line_index >= mappings_.size())
      return std::nullopt;

    const auto& segments = mappings_[line_index];
    if (segments.empty())
      return std::nullopt;

    const int zero_based_column = std::max(generated_column - 1, 0);
    const segment* best = nullptr;
    for (const auto& seg : segments) {
      if (seg.generated_column <= zero_based_column) {
        best = &seg;
      } else {
        break;
      }
    }
    if (!best)
      best = &segments.front();
    if (best->source_index < 0 || best->source_line < 0 || best->source_column < 0)
      return std::nullopt;
    const auto source_index = static_cast<usize>(best->source_index);
    if (source_index >= sources_.size())
      return std::nullopt;

    source_mapped_position pos;
    pos.source = resolve_source_path(module_url_, sources_[source_index]);
    pos.line = best->source_line + 1;
    pos.column = best->source_column + 1;
    if (best->name_index >= 0) {
      const auto name_index = static_cast<usize>(best->name_index);
      if (name_index < names_.size())
        pos.name = names_[name_index];
    }
    return pos;
  }

  bool source_map_decoder::empty() const noexcept {
    return mappings_.empty() || sources_.empty();
  }

  source_map_cache::source_map_cache(usize capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  void source_map_cache::put(std::string_view module_url, source_map_decoder decoder) {
    const std::string key = normalize_source_map_url(module_url);
    if (key.empty() || decoder.empty())
      return;
    erase(key);
    entries_.push_front(entry{key, std::move(decoder)});
    while (entries_.size() > capacity_)
      entries_.pop_back();
  }

  void source_map_cache::put_json(std::string_view module_url, std::string_view source_map_json,
                                  int generated_line_offset) {
    if (auto decoder =
            source_map_decoder::parse(module_url, source_map_json, generated_line_offset))
      put(module_url, std::move(*decoder));
  }

  void source_map_cache::erase(std::string_view module_url) {
    const std::string key = normalize_source_map_url(module_url);
    entries_.remove_if([&](const entry& e) { return e.module_url == key; });
  }

  void source_map_cache::clear() {
    entries_.clear();
  }

  std::optional<source_mapped_position>
  source_map_cache::original_position(std::string_view module_url, int generated_line,
                                      int generated_column) {
    const std::string key = normalize_source_map_url(module_url);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->module_url != key)
        continue;
      auto mapped = it->decoder.original_position(generated_line, generated_column);
      entries_.splice(entries_.begin(), entries_, it);
      return mapped;
    }
    return std::nullopt;
  }

  usize source_map_cache::size() const noexcept {
    return entries_.size();
  }

  source_map_cache& source_maps() {
    thread_local source_map_cache cache(64);
    return cache;
  }
} // namespace fxe::js
