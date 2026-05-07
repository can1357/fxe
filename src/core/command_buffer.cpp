#include <fxe/command_buffer.hpp>
#include <fxe/types.hpp>

#include <algorithm>
#include <string_view>
namespace fxe {
  command_buffer command_buffer::clone() const {
    command_buffer out;
    out.vertex_buffer = vertex_buffer;
    for (usize i = 0; i != index_buffers.size(); ++i)
      out.index_buffers[i] = index_buffers[i];
    out.epoch = epoch;
    return out;
  }

  bool command_buffer::is_empty() const noexcept {
    if (!vertex_buffer.empty())
      return false;
    for (const auto& ib : index_buffers)
      if (!ib.empty())
        return false;
    return true;
  }
  std::pair<math::vec3, math::vec3> command_buffer::get_boundaries() const {
    if (vertex_buffer.empty())
      return {{0, 0, 0}, {0, 0, 0}};
    math::vec3 mn{math::flt_max, math::flt_max, math::flt_max};
    math::vec3 mx{-math::flt_max, -math::flt_max, -math::flt_max};
    for (const auto& v : vertex_buffer) {
      mn = math::vec_min(mn, v.pos);
      mx = math::vec_max(mx, v.pos);
    }
    return {mn, mx};
  }

  std::pair<math::vec3, math::vec3> command_buffer::get_boundaries(const math::mat4x4& tf) const {
    if (vertex_buffer.empty())
      return {{0, 0, 0}, {0, 0, 0}};
    math::vec3 mn{math::flt_max, math::flt_max, math::flt_max};
    math::vec3 mx{-math::flt_max, -math::flt_max, -math::flt_max};
    for (const auto& v : vertex_buffer) {
      auto p = math::vec3(tf * math::vec4(v.pos, 1.0f));
      mn = math::vec_min(mn, p);
      mx = math::vec_max(mx, p);
    }
    return {mn, mx};
  }

  std::string r8g8b8a8::to_string() const {
    char out[10]{};
    static constexpr char hex[] = "0123456789abcdef";
    out[0] = '#';
    const u8 bytes[] = {r, g, b, a};
    for (int i = 0; i != 4; ++i) {
      out[1 + i * 2] = hex[bytes[i] >> 4];
      out[2 + i * 2] = hex[bytes[i] & 0xf];
    }
    return out;
  }

  r8g8b8a8 color_by_name(std::string_view name) noexcept {
    auto cmp = [](const named_color& kv, std::string_view n) {
      return std::string_view(kv.first) < n;
    };
    auto it = std::lower_bound(color_table.begin(), color_table.end(), name, cmp);
    if (it == color_table.end() || std::string_view(it->first) != name)
      return transparent;
    return it->second;
  }
} // namespace fxe
