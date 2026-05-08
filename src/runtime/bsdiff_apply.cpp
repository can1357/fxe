#include "updater.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace fxe::runtime {
  namespace {

    constexpr std::array<uint8_t, 8> k_magic{'F', 'X', 'E', 'B', 'S', 'D', '\0', '\0'};

    bool read_u64_le(const std::vector<uint8_t>& bytes, size_t offset, uint64_t& out) {
      if (offset > bytes.size() || bytes.size() - offset < 8)
        return false;
      out = 0;
      for (int i = 7; i >= 0; --i)
        out = (out << 8) | bytes[offset + static_cast<size_t>(i)];
      return true;
    }

    bool read_i64_le(const uint8_t* ptr, size_t len, size_t& offset, int64_t& out) {
      if (offset > len || len - offset < 8)
        return false;
      uint64_t raw = 0;
      for (int i = 7; i >= 0; --i)
        raw = (raw << 8) | ptr[offset + static_cast<size_t>(i)];
      offset += 8;
      out = static_cast<int64_t>(raw);
      return true;
    }

  } // namespace

  bool apply_bsdiff(const std::vector<uint8_t>& old_bytes, const std::vector<uint8_t>& patch,
                    std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (patch.size() < 32) {
      err = "bsdiff patch header is truncated";
      return false;
    }
    if (!std::equal(k_magic.begin(), k_magic.end(), patch.begin())) {
      err = "bsdiff patch magic is invalid";
      return false;
    }

    uint64_t control_len = 0;
    uint64_t diff_len = 0;
    uint64_t extra_len = 0;
    if (!read_u64_le(patch, 8, control_len) || !read_u64_le(patch, 16, diff_len) ||
        !read_u64_le(patch, 24, extra_len)) {
      err = "bsdiff patch header is truncated";
      return false;
    }
    constexpr uint64_t k_header_size = 32;
    const uint64_t total = k_header_size + control_len + diff_len + extra_len;
    if (total != patch.size()) {
      err = "bsdiff patch length mismatch";
      return false;
    }
    if (control_len % 24 != 0) {
      err = "bsdiff control stream is misaligned";
      return false;
    }

    const uint8_t* control = patch.data() + k_header_size;
    const uint8_t* diff = control + control_len;
    const uint8_t* extra = diff + diff_len;
    size_t control_off = 0;
    size_t diff_off = 0;
    size_t extra_off = 0;
    size_t old_pos = 0;

    while (control_off < control_len) {
      int64_t mix_count = 0;
      int64_t extra_count = 0;
      int64_t seek_offset = 0;
      if (!read_i64_le(control, static_cast<size_t>(control_len), control_off, mix_count) ||
          !read_i64_le(control, static_cast<size_t>(control_len), control_off, extra_count) ||
          !read_i64_le(control, static_cast<size_t>(control_len), control_off, seek_offset)) {
        err = "bsdiff control stream is truncated";
        return false;
      }
      if (mix_count < 0 || extra_count < 0) {
        err = "bsdiff control stream has negative sizes";
        return false;
      }
      const size_t mix = static_cast<size_t>(mix_count);
      const size_t extra_size = static_cast<size_t>(extra_count);
      if (diff_off > diff_len || static_cast<uint64_t>(mix) > diff_len - diff_off) {
        err = "bsdiff diff stream is truncated";
        return false;
      }
      if (old_pos > old_bytes.size() || mix > old_bytes.size() - old_pos) {
        err = "bsdiff mix range exceeds source bytes";
        return false;
      }
      if (out.size() > std::numeric_limits<size_t>::max() - mix) {
        err = "bsdiff output is too large";
        return false;
      }
      out.reserve(out.size() + mix + extra_size);
      for (size_t i = 0; i < mix; ++i) {
        const uint8_t sum = static_cast<uint8_t>(old_bytes[old_pos + i] + diff[diff_off + i]);
        out.push_back(sum);
      }
      diff_off += mix;
      old_pos += mix;
      if (extra_off > extra_len || static_cast<uint64_t>(extra_size) > extra_len - extra_off) {
        err = "bsdiff extra stream is truncated";
        return false;
      }
      out.insert(out.end(), extra + extra_off, extra + extra_off + extra_size);
      extra_off += extra_size;
      if (seek_offset < 0 && static_cast<size_t>(-seek_offset) > old_pos) {
        err = "bsdiff seek moved before start of source bytes";
        return false;
      }
      if (seek_offset >= 0 && static_cast<uint64_t>(seek_offset) > old_bytes.size() - old_pos) {
        err = "bsdiff seek moved past end of source bytes";
        return false;
      }
      old_pos = seek_offset < 0 ? old_pos - static_cast<size_t>(-seek_offset)
                                : old_pos + static_cast<size_t>(seek_offset);
    }
    if (diff_off != diff_len || extra_off != extra_len) {
      err = "bsdiff patch streams were not fully consumed";
      return false;
    }
    return true;
  }

} // namespace fxe::runtime
