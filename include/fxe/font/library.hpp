#pragma once

// Process-wide FreeType library handle. Owned by a singleton with a mutex so
// callers can `lock()` for the duration of an FT call. CoreText-only builds
// link against `library_noop.cpp` which provides the same API as a no-op.

#include <memory>
#include <mutex>

namespace fxe::font {

  class Library {
  public:
    Library();
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&&) = delete;
    Library& operator=(Library&&) = delete;

    // Returns the underlying `FT_Library` handle as a void* so callers don't
    // need to drag <ft2build.h> into headers. FreeType-free builds return
    // nullptr.
    [[nodiscard]] void* raw() const noexcept;

    // Acquires an exclusive lock on the underlying library. FreeType is not
    // thread-safe across `FT_Face` instances created from the same `FT_Library`,
    // so callers must hold the lock for the duration of any FT call.
    [[nodiscard]] std::unique_lock<std::mutex> lock() {
      return std::unique_lock<std::mutex>{mu_};
    }

  private:
    std::mutex mu_;
    [[maybe_unused]] void* impl_ = nullptr;
  };

  // Process-wide singleton. Lazily initialised on first call.
  [[nodiscard]] Library& shared_library();

} // namespace fxe::font
