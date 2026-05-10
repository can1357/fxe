#pragma once

#include <fxe/log.hpp>
#include <fxe/types.hpp>

#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fxe::font {
  class mmap_region {
  public:
    mmap_region() = default;

    explicit mmap_region(std::string_view path) {
#ifdef _WIN32
      const auto wide = utf8_to_utf16(path);
      if (wide.empty())
        return;

      file_ = CreateFileW(wide.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file_ == INVALID_HANDLE_VALUE) {
        FXE_DEBUG("font.face", "mmap CreateFileW failed path='{}' err={}", path,
                  static_cast<u32>(GetLastError()));
        return;
      }

      LARGE_INTEGER file_size{};
      if (!GetFileSizeEx(file_, &file_size)) {
        FXE_DEBUG("font.face", "mmap GetFileSizeEx failed path='{}' err={}", path,
                  static_cast<u32>(GetLastError()));
        reset();
        return;
      }
      if (file_size.QuadPart <= 0) {
        FXE_DEBUG("font.face", "mmap rejected empty file path='{}'", path);
        reset();
        return;
      }
      const auto size64 = static_cast<u64>(file_size.QuadPart);
      if (size64 > static_cast<u64>(std::numeric_limits<usize>::max())) {
        FXE_DEBUG("font.face", "mmap rejected oversized file path='{}' bytes={}", path, size64);
        reset();
        return;
      }

      mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
      if (!mapping_) {
        FXE_DEBUG("font.face", "mmap CreateFileMappingW failed path='{}' err={}", path,
                  static_cast<u32>(GetLastError()));
        reset();
        return;
      }

      void* mapped = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
      if (!mapped) {
        FXE_DEBUG("font.face", "mmap MapViewOfFile failed path='{}' err={}", path,
                  static_cast<u32>(GetLastError()));
        reset();
        return;
      }

      data_ = static_cast<const u8*>(mapped);
      size_ = static_cast<usize>(size64);
#else
      std::string path_string(path);
      const int fd = open(path_string.c_str(), O_RDONLY);
      if (fd < 0) {
        FXE_DEBUG("font.face", "mmap open failed path='{}' errno={}", path, errno);
        return;
      }

      struct stat st{};
      if (fstat(fd, &st) != 0) {
        FXE_DEBUG("font.face", "mmap fstat failed path='{}' errno={}", path, errno);
        close(fd);
        return;
      }
      if (!S_ISREG(st.st_mode)) {
        FXE_DEBUG("font.face", "mmap rejected non-regular file path='{}'", path);
        close(fd);
        return;
      }
      if (st.st_size <= 0) {
        FXE_DEBUG("font.face", "mmap rejected empty file path='{}'", path);
        close(fd);
        return;
      }
      const auto size64 = static_cast<u64>(st.st_size);
      if (size64 > static_cast<u64>(std::numeric_limits<usize>::max())) {
        FXE_DEBUG("font.face", "mmap rejected oversized file path='{}' bytes={}", path, size64);
        close(fd);
        return;
      }

      void* mapped = mmap(nullptr, static_cast<usize>(size64), PROT_READ, MAP_PRIVATE, fd, 0);
      const int map_errno = errno;
      close(fd);
      if (mapped == MAP_FAILED) {
        FXE_DEBUG("font.face", "mmap failed path='{}' errno={}", path, map_errno);
        return;
      }

      mapping_ = mapped;
      data_ = static_cast<const u8*>(mapped);
      size_ = static_cast<usize>(size64);
#endif
    }

    mmap_region(const mmap_region&) = delete;
    mmap_region& operator=(const mmap_region&) = delete;

    mmap_region(mmap_region&& other) noexcept {
      *this = std::move(other);
    }

    mmap_region& operator=(mmap_region&& other) noexcept {
      if (this == &other)
        return *this;
      reset();
      data_ = other.data_;
      size_ = other.size_;
#ifdef _WIN32
      file_ = other.file_;
      mapping_ = other.mapping_;
      other.file_ = INVALID_HANDLE_VALUE;
      other.mapping_ = nullptr;
#else
      mapping_ = other.mapping_;
      other.mapping_ = nullptr;
#endif
      other.data_ = nullptr;
      other.size_ = 0;
      return *this;
    }

    ~mmap_region() {
      reset();
    }

    [[nodiscard]] const u8* data() const noexcept {
      return data_;
    }

    [[nodiscard]] std::span<const u8> bytes() const noexcept {
      return std::span<const u8>(data_, size_);
    }

  private:
#ifdef _WIN32
    static std::wstring utf8_to_utf16(std::string_view path) {
      if (path.empty()) {
        FXE_DEBUG("font.face", "mmap rejected empty path");
        return {};
      }
      if (path.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        FXE_DEBUG("font.face", "mmap rejected oversized path bytes={}",
                  static_cast<u64>(path.size()));
        return {};
      }

      const int narrow_len = static_cast<int>(path.size());
      const int wide_len =
          MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), narrow_len, nullptr, 0);
      if (wide_len <= 0) {
        FXE_DEBUG("font.face", "mmap utf8 path conversion failed err={}",
                  static_cast<u32>(GetLastError()));
        return {};
      }

      std::wstring wide(static_cast<usize>(wide_len), L'\0');
      const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                              narrow_len, wide.data(), wide_len);
      if (written != wide_len) {
        FXE_DEBUG("font.face", "mmap utf8 path conversion short write err={}",
                  static_cast<u32>(GetLastError()));
        return {};
      }
      return wide;
    }
#endif

    void reset() noexcept {
#ifdef _WIN32
      if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
      }
      if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
      }
      if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
      }
#else
      if (mapping_) {
        munmap(mapping_, size_);
        mapping_ = nullptr;
      }
      data_ = nullptr;
#endif
      size_ = 0;
    }

    const u8* data_ = nullptr;
    usize size_ = 0;
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    void* mapping_ = nullptr;
#endif
  };
} // namespace fxe::font
