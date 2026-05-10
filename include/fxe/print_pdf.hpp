#pragma once

#include <fxe/types.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fxe {
  struct command_view;

  struct pdf_page {
    u32 width_pt = 0;
    u32 height_pt = 0;
    const command_view* cb = nullptr;
  };

  bool emit_pdf(const std::filesystem::path& path, const std::vector<pdf_page>& pages,
                std::string* err_out = nullptr);
} // namespace fxe
