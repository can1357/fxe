#include <fxe/font/face.hpp>
#include <fxe/types.hpp>

#include "font/mmap_file.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
  int g_pass = 0;
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
    } else {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }
#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
} // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path dir = fs::current_path();
  const fs::path stub_path = dir / "font-mmap-stub.bin";
  const fs::path empty_path = dir / "font-mmap-empty.bin";

  const std::array<u8, 16> expected = {0x00, 0x01, 0x7f, 0x80, 0x10, 0x20, 0x30, 0x40,
                                       0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0, 0xff};

  {
    std::ofstream out(stub_path, std::ios::binary | std::ios::trunc);
    CHECK(out.is_open());
    if (out.is_open()) {
      out.write(reinterpret_cast<const char*>(expected.data()),
                static_cast<std::streamsize>(expected.size()));
      CHECK(out.good());
    }
  }

  {
    fxe::font::mmap_region mapped(stub_path.string());
    CHECK(mapped.data() != nullptr);
    CHECK(mapped.bytes().size() == expected.size());
    if (mapped.data() != nullptr && mapped.bytes().size() == expected.size()) {
      CHECK(std::equal(mapped.bytes().begin(), mapped.bytes().end(), expected.begin()));
    }
  }

  {
    auto face = fxe::font::load_face_from_file(stub_path.string(), 16.0f);
    CHECK(face == nullptr);
  }

  {
    std::ofstream out(empty_path, std::ios::binary | std::ios::trunc);
    CHECK(out.is_open());
  }

  {
    fxe::font::mmap_region mapped(empty_path.string());
    CHECK(mapped.data() == nullptr);
    CHECK(mapped.bytes().empty());
  }

  std::error_code ec;
  fs::remove(stub_path, ec);
  ec.clear();
  fs::remove(empty_path, ec);

  std::printf("PASS %d FAIL %d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
