#include <fxe/print_pdf.hpp>

#include <fxe/command_buffer.hpp>
#include <fxe/vertex.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <fxe/types.hpp>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fxe {
  namespace {
    struct pdf_point {
      double x = 0.0;
      double y = 0.0;
    };

    std::string num(double v) {
      if (v == 0.0)
        v = 0.0;
      std::ostringstream ss;
      ss << std::fixed << std::setprecision(3) << v;
      std::string s = ss.str();
      while (s.size() > 1 && s.back() == '0')
        s.pop_back();
      if (!s.empty() && s.back() == '.')
        s.pop_back();
      return s.empty() ? "0" : s;
    }

    pdf_point page_point(const vertex& v, u32 height_pt) {
      return {static_cast<double>(v.pos.x),
              static_cast<double>(height_pt) - static_cast<double>(v.pos.y)};
    }

    void emit_color(std::ostringstream& out, r8g8b8a8 c, bool stroke) {
      const double r = static_cast<double>(c.r) / 255.0;
      const double g = static_cast<double>(c.g) / 255.0;
      const double b = static_cast<double>(c.b) / 255.0;
      out << num(r) << ' ' << num(g) << ' ' << num(b) << (stroke ? " RG\n" : " rg\n");
    }

    void emit_move_line(std::ostringstream& out, pdf_point p, char op) {
      out << num(p.x) << ' ' << num(p.y) << ' ' << op << "\n";
    }

    bool same_color(const vertex& a, const vertex& b, const vertex& c) {
      return a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b &&
             a.color.a == b.color.a && a.color.r == c.color.r && a.color.g == c.color.g &&
             a.color.b == c.color.b && a.color.a == c.color.a;
    }

    bool fail(std::string* err_out, std::string reason) {
      if (err_out)
        *err_out = std::move(reason);
      return false;
    }

    std::string path_for_error(const std::filesystem::path& path) {
      const std::string s = path.string();
      return s.empty() ? std::string{"<empty>"} : s;
    }

    texture_id texture_of(const vertex& v) noexcept {
      return std::bit_cast<texture_id>(v.uv.z);
    }

    bool has_texture(const vertex& a, const vertex& b, const vertex& c) noexcept {
      return texture_of(a) != null_texture || texture_of(b) != null_texture ||
             texture_of(c) != null_texture;
    }

    bool has_texture(const vertex& a, const vertex& b) noexcept {
      return texture_of(a) != null_texture || texture_of(b) != null_texture;
    }

    bool content_for_page(const pdf_page& page, usize page_index, std::string& content,
                          std::string* err_out) {
      std::ostringstream out;
      out << "q\n";
      out << "1 J\n1 j\n";
      if (!page.cb) {
        out << "Q\n";
        content = out.str();
        return true;
      }

      const auto& vertices = page.cb->vertex_buffer;
      const auto& triangles = page.cb->index_buffers[static_cast<usize>(vertex_topology::triangle)];
      for (usize i = 0; i + 2 < triangles.size(); i += 3) {
        const u32 ia = triangles[i + 0];
        const u32 ib = triangles[i + 1];
        const u32 ic = triangles[i + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) {
          return fail(err_out,
                      "malformed primitive index: triangle on page " + std::to_string(page_index));
        }
        const vertex& a = vertices[ia];
        const vertex& b = vertices[ib];
        const vertex& c = vertices[ic];
        if (has_texture(a, b, c)) {
          return fail(err_out, "unsupported primitive: textured triangle on page " +
                                   std::to_string(page_index));
        }
        const r8g8b8a8 fill = same_color(a, b, c)
                                  ? a.color
                                  : r8g8b8a8{u8((u32(a.color.r) + b.color.r + c.color.r) / 3u),
                                             u8((u32(a.color.g) + b.color.g + c.color.g) / 3u),
                                             u8((u32(a.color.b) + b.color.b + c.color.b) / 3u),
                                             u8((u32(a.color.a) + b.color.a + c.color.a) / 3u)};
        emit_color(out, fill, false);
        emit_move_line(out, page_point(a, page.height_pt), 'm');
        emit_move_line(out, page_point(b, page.height_pt), 'l');
        emit_move_line(out, page_point(c, page.height_pt), 'l');
        out << "h\nf\n";
      }

      const auto& lines = page.cb->index_buffers[static_cast<usize>(vertex_topology::line)];
      out << "1 w\n";
      for (usize i = 0; i + 1 < lines.size(); i += 2) {
        const u32 ia = lines[i + 0];
        const u32 ib = lines[i + 1];
        if (ia >= vertices.size() || ib >= vertices.size()) {
          return fail(err_out,
                      "malformed primitive index: line on page " + std::to_string(page_index));
        }
        const vertex& a = vertices[ia];
        const vertex& b = vertices[ib];
        if (has_texture(a, b)) {
          return fail(err_out,
                      "unsupported primitive: textured line on page " + std::to_string(page_index));
        }
        emit_color(out, a.color, true);
        emit_move_line(out, page_point(a, page.height_pt), 'm');
        emit_move_line(out, page_point(b, page.height_pt), 'l');
        out << "S\n";
      }
      out << "Q\n";
      content = out.str();
      return true;
    }

    std::string stream_object(const std::string& content) {
      std::ostringstream out;
      out << "<< /Length " << content.size() << " >>\nstream\n";
      out << content;
      out << "endstream\n";
      return out.str();
    }
  } // namespace

  bool emit_pdf(const std::filesystem::path& path, const std::vector<pdf_page>& pages,
                std::string* err_out) {
    if (err_out)
      err_out->clear();
    if (path.empty())
      return fail(err_out, "pdf output path empty");
    if (pages.empty())
      return fail(err_out, "no pdf pages supplied");

    std::vector<std::string> objects;
    objects.resize(2 + pages.size() * 2);

    std::ostringstream kids;
    for (usize i = 0; i != pages.size(); ++i) {
      const int page_id = static_cast<int>(3 + i * 2);
      const int content_id = page_id + 1;
      kids << page_id << " 0 R ";

      const u32 width = std::max<u32>(pages[i].width_pt, 1);
      const u32 height = std::max<u32>(pages[i].height_pt, 1);
      objects[static_cast<usize>(page_id - 1)] = [&] {
        std::ostringstream obj;
        obj << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " << width << ' ' << height
            << "] /Resources << >> /Contents " << content_id << " 0 R >>\n";
        return obj.str();
      }();
      std::string content;
      if (!content_for_page(pages[i], i + 1, content, err_out))
        return false;
      objects[static_cast<usize>(content_id - 1)] = stream_object(content);
    }

    objects[0] = "<< /Type /Catalog /Pages 2 0 R >>\n";
    objects[1] = [&] {
      std::ostringstream obj;
      obj << "<< /Type /Pages /Count " << pages.size() << " /Kids [ " << kids.str() << "] >>\n";
      return obj.str();
    }();

    const std::string display_path = path_for_error(path);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
      return fail(err_out, "io error writing " + display_path + ": open failed");

    out << "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::streamoff> offsets;
    offsets.reserve(objects.size() + 1);
    offsets.push_back(0);
    for (usize i = 0; i != objects.size(); ++i) {
      const auto pos = out.tellp();
      if (pos == std::streampos(-1))
        return fail(err_out, "io error writing " + display_path + ": position unavailable");
      offsets.push_back(static_cast<std::streamoff>(pos));
      out << (i + 1) << " 0 obj\n" << objects[i] << "endobj\n";
    }

    const auto xref_pos = out.tellp();
    if (xref_pos == std::streampos(-1))
      return fail(err_out, "io error writing " + display_path + ": position unavailable");
    const std::streamoff xref = static_cast<std::streamoff>(xref_pos);
    out << "xref\n0 " << offsets.size() << "\n";
    out << "0000000000 65535 f \n";
    for (usize i = 1; i != offsets.size(); ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%010lld 00000 n \n", static_cast<long long>(offsets[i]));
      out << buf;
    }
    out << "trailer\n<< /Size " << offsets.size() << " /Root 1 0 R >>\nstartxref\n"
        << xref << "\n%%EOF\n";
    if (!out.good())
      return fail(err_out, "io error writing " + display_path);
    return true;
  }
} // namespace fxe
