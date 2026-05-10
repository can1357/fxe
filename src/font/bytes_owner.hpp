#pragma once

// Lifetime-owning byte source for Face. Polymorphic so a single Face type can
// hold either an owned heap copy (e.g. from a caller-supplied std::span) or
// an mmap'd file (zero-copy from disk) without templating Face.

#include "mmap_file.hpp"

#include <fxe/types.hpp>

#include <span>
#include <utility>
#include <vector>

namespace fxe::font {

  class bytes_owner {
  public:
    virtual ~bytes_owner() = default;
    [[nodiscard]] virtual std::span<const u8> bytes() const noexcept = 0;
  };

  class vector_bytes_owner final : public bytes_owner {
  public:
    explicit vector_bytes_owner(std::vector<u8> v) noexcept : data_(std::move(v)) {}

    [[nodiscard]] std::span<const u8> bytes() const noexcept override {
      return {data_.data(), data_.size()};
    }

  private:
    std::vector<u8> data_;
  };

  class mmap_bytes_owner final : public bytes_owner {
  public:
    explicit mmap_bytes_owner(mmap_region m) noexcept : map_(std::move(m)) {}

    [[nodiscard]] std::span<const u8> bytes() const noexcept override {
      return map_.bytes();
    }

  private:
    mmap_region map_;
  };

} // namespace fxe::font
