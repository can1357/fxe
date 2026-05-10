// Win32 implementation of fxe::os.
//
// Window-owned UI features are implemented through a private message-only HWND
// and background message pump because GLFW owns the visible application HWND.

#include "../os.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fxe/log.hpp>
#include <fxe/string_utils.hpp>
#include <limits>
#include <map>
#include <memory>

#include <fxe/types.hpp>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <commctrl.h>
#include <dwmapi.h>
#include <inspectable.h>
#include <knownfolders.h>
#include <objbase.h>
#include <propkey.h>
#include <propsys.h>
#include <roapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.data.xml.dom.h>
#include <windows.foundation.collections.h>
#include <windows.foundation.h>
#include <windows.h>
#include <windows.ui.notifications.h>
#include <windowsx.h>
#include <wrl/client.h>
#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "WindowsApp.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

#include <string>
#include <thread>
#include <utility>
#include <vector>
#endif

namespace fxe::os {
  namespace {
    std::mutex g_mu;
    std::queue<std::function<void()>> g_q;
  } // namespace

#ifdef _WIN32
  namespace {
    std::mutex g_instance_mu;
    HANDLE g_instance_mutex = nullptr;

    struct co_scope {
      HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      ~co_scope() {
        if (SUCCEEDED(hr))
          CoUninitialize();
      }
      bool usable() const noexcept {
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
      }
    };

    std::wstring widen(std::string_view s) {
      if (s.empty())
        return {};
      int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
      if (n <= 0)
        return {};
      std::wstring out(static_cast<usize>(n), L'\0');
      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                              out.data(), n) != n)
        return {};
      return out;
    }

    std::string narrow(const wchar_t* s) {
      if (!s || !*s)
        return {};
      int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
      if (n <= 1)
        return {};
      std::string out(static_cast<usize>(n - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
      return out;
    }
    std::optional<DWORD> read_reg_dword(HKEY root, const wchar_t* subkey, const wchar_t* value) {
      DWORD data = 0;
      DWORD type = 0;
      DWORD size = sizeof(data);
      LONG rc = RegGetValueW(root, subkey, value, RRF_RT_REG_DWORD, &type, &data, &size);
      if (rc != ERROR_SUCCESS || type != REG_DWORD)
        return std::nullopt;
      return data;
    }

    std::string narrow_hstring(HSTRING value) {
      UINT32 len = 0;
      const wchar_t* raw = WindowsGetStringRawBuffer(value, &len);
      if (!raw || len == 0)
        return {};
      int n =
          WideCharToMultiByte(CP_UTF8, 0, raw, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
      if (n <= 0)
        return {};
      std::string out(static_cast<usize>(n), '\0');
      WideCharToMultiByte(CP_UTF8, 0, raw, static_cast<int>(len), out.data(), n, nullptr, nullptr);
      return out;
    }

    HGLOBAL hglobal_from_bytes(const u8* bytes, usize size, bool nul_terminate = false) {
      if (size > static_cast<usize>(std::numeric_limits<SIZE_T>::max()) - (nul_terminate ? 1u : 0u))
        return nullptr;
      HGLOBAL handle =
          GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(size + (nul_terminate ? 1u : 0u)));
      if (!handle)
        return nullptr;
      void* dst = GlobalLock(handle);
      if (!dst) {
        GlobalFree(handle);
        return nullptr;
      }
      if (size > 0)
        std::memcpy(dst, bytes, size);
      if (nul_terminate)
        static_cast<u8*>(dst)[size] = 0;
      GlobalUnlock(handle);
      return handle;
    }

    HGLOBAL hglobal_from_string(std::string_view value, bool nul_terminate = true) {
      return hglobal_from_bytes(reinterpret_cast<const u8*>(value.data()), value.size(),
                                nul_terminate);
    }

    bool set_clipboard_format(UINT format, HGLOBAL handle) {
      if (format == 0 || !handle)
        return false;
      if (!OpenClipboard(nullptr)) {
        GlobalFree(handle);
        return false;
      }
      bool ok = false;
      if (EmptyClipboard() && SetClipboardData(format, handle)) {
        ok = true;
        handle = nullptr;
      }
      CloseClipboard();
      if (handle)
        GlobalFree(handle);
      return ok;
    }

    std::optional<std::vector<u8>> get_clipboard_format(UINT format) {
      if (format == 0 || !OpenClipboard(nullptr))
        return std::nullopt;
      std::optional<std::vector<u8>> out;
      HANDLE handle = GetClipboardData(format);
      if (handle) {
        SIZE_T size = GlobalSize(handle);
        void* src = GlobalLock(handle);
        if (src) {
          out = std::vector<u8>(static_cast<usize>(size));
          if (size > 0)
            std::memcpy(out->data(), src, static_cast<usize>(size));
          GlobalUnlock(handle);
        }
      }
      CloseClipboard();
      return out;
    }

    UINT html_clipboard_format() {
      return RegisterClipboardFormatW(L"HTML Format");
    }

    UINT rtf_clipboard_format() {
      return RegisterClipboardFormatW(L"Rich Text Format");
    }

    std::string cf_html_from_fragment(std::string_view html) {
      constexpr std::string_view prefix = "Version:1.0\r\n"
                                          "StartHTML:0000000000\r\n"
                                          "EndHTML:0000000000\r\n"
                                          "StartFragment:0000000000\r\n"
                                          "EndFragment:0000000000\r\n";
      constexpr std::string_view start_marker = "<!--StartFragment-->";
      constexpr std::string_view end_marker = "<!--EndFragment-->";
      std::string out;
      out.reserve(prefix.size() + start_marker.size() + html.size() + end_marker.size());
      out.append(prefix);
      const usize start_html = out.size();
      out.append(start_marker);
      const usize start_fragment = out.size();
      out.append(html);
      const usize end_fragment = out.size();
      out.append(end_marker);
      const usize end_html = out.size();
      auto write_offset = [&](std::string_view key, usize value) {
        usize pos = out.find(key);
        if (pos == std::string::npos)
          return;
        pos += key.size();
        char buf[11] = {};
        std::snprintf(buf, sizeof(buf), "%010zu", value);
        out.replace(pos, 10, buf, 10);
      };
      write_offset("StartHTML:", start_html);
      write_offset("EndHTML:", end_html);
      write_offset("StartFragment:", start_fragment);
      write_offset("EndFragment:", end_fragment);
      return out;
    }

    std::optional<usize> cf_html_offset(const std::string& text, const char* key) {
      usize pos = text.find(key);
      if (pos == std::string::npos)
        return std::nullopt;
      pos += std::strlen(key);
      while (pos < text.size() && text[pos] == ' ')
        ++pos;
      usize end = pos;
      while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
        ++end;
      if (end == pos)
        return std::nullopt;
      return static_cast<usize>(std::strtoull(text.c_str() + pos, nullptr, 10));
    }

    std::string fragment_from_cf_html(std::vector<u8> bytes) {
      while (!bytes.empty() && bytes.back() == 0)
        bytes.pop_back();
      std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      auto start = cf_html_offset(text, "StartFragment:");
      auto end = cf_html_offset(text, "EndFragment:");
      if (!start || !end) {
        start = cf_html_offset(text, "StartHTML:");
        end = cf_html_offset(text, "EndHTML:");
      }
      if (start && end && *start <= *end && *end <= text.size())
        return text.substr(*start, *end - *start);
      return text;
    }

    std::string known_folder(REFKNOWNFOLDERID id) {
      PWSTR path = nullptr;
      HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path);
      if (FAILED(hr) || !path)
        return {};
      std::string out = narrow(path);
      CoTaskMemFree(path);
      return out;
    }

    std::wstring quote_arg(const std::wstring& arg) {
      std::wstring out = L"\"";
      unsigned slashes = 0;
      for (wchar_t ch : arg) {
        if (ch == L'\\') {
          ++slashes;
        } else if (ch == L'\"') {
          out.append(slashes * 2 + 1, L'\\');
          out.push_back(ch);
          slashes = 0;
        } else {
          out.append(slashes, L'\\');
          slashes = 0;
          out.push_back(ch);
        }
      }
      out.append(slashes * 2, L'\\');
      out.push_back(L'\"');
      return out;
    }

    bool create_process(const std::wstring& application, std::wstring command_line) {
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi{};
      BOOL ok = CreateProcessW(application.empty() ? nullptr : application.c_str(),
                               command_line.empty() ? nullptr : command_line.data(), nullptr,
                               nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
      if (!ok)
        return false;
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      return true;
    }

    bool explorer_select(const std::wstring& path) {
      std::wstring params = L"/select," + quote_arg(path);
      HINSTANCE r =
          ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
      return reinterpret_cast<INT_PTR>(r) > 32;
    }

    void split_default_path(const std::wstring& path, std::wstring& folder, std::wstring& file) {
      usize pos = path.find_last_of(L"\\/");
      if (pos == std::wstring::npos) {
        file = path;
        return;
      }
      folder = path.substr(0, pos);
      file = path.substr(pos + 1);
    }

    void set_dialog_default_path(IFileDialog* dialog, const std::string& default_path,
                                 bool set_file_name) {
      std::wstring path = widen(default_path);
      if (path.empty())
        return;

      DWORD attrs = GetFileAttributesW(path.c_str());
      std::wstring folder;
      std::wstring file;
      if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        folder = path;
      } else {
        split_default_path(path, folder, file);
      }

      if (!folder.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
          dialog->SetFolder(item);
          item->Release();
        }
      }
      if (set_file_name && !file.empty())
        dialog->SetFileName(file.c_str());
    }

    struct filter_storage {
      std::vector<std::wstring> names;
      std::vector<std::wstring> specs;
      std::vector<COMDLG_FILTERSPEC> filters;
    };

    filter_storage make_filters(const std::vector<dialog_filter>& input) {
      filter_storage out;
      out.names.reserve(input.size());
      out.specs.reserve(input.size());
      for (const auto& f : input) {
        if (f.extensions.empty())
          continue;
        std::wstring spec;
        for (const auto& ext8 : f.extensions) {
          std::wstring ext = widen(ext8);
          if (ext.empty())
            continue;
          if (!spec.empty())
            spec += L";";
          spec += L"*.";
          if (!ext.empty() && ext.front() == L'.')
            spec += ext.substr(1);
          else
            spec += ext;
        }
        if (spec.empty())
          continue;
        std::wstring name = widen(f.name);
        if (name.empty())
          name = spec;
        out.names.push_back(std::move(name));
        out.specs.push_back(std::move(spec));
      }
      out.filters.reserve(out.names.size());
      for (usize i = 0; i < out.names.size(); ++i)
        out.filters.push_back(COMDLG_FILTERSPEC{out.names[i].c_str(), out.specs[i].c_str()});
      return out;
    }

    void apply_filters(IFileDialog* dialog, const filter_storage& storage) {
      if (storage.filters.empty())
        return;
      if (SUCCEEDED(dialog->SetFileTypes(static_cast<UINT>(storage.filters.size()),
                                         storage.filters.data())))
        dialog->SetFileTypeIndex(1);
    }

    constexpr UINT kTrayMessage = WM_USER + 1;
    constexpr UINT_PTR kSubclassId = 0x66786531;
    constexpr UINT kBalloonUidBase = 40000u;
    constexpr wchar_t kHiddenWindowClass[] = L"fxe.os.win32.hidden";

    struct parsed_accelerator {
      UINT modifiers = 0;
      UINT vk = 0;
    };

    struct menu_item_handle {
      HMENU parent = nullptr;
      HMENU submenu = nullptr;
      UINT command_id = 0;
      UINT position = 0;
      bool is_submenu = false;
      bool visible = true;
      UINT state_flags = 0;
      std::wstring label;
      std::wstring accelerator;
    };

    struct menu_build_result {
      HMENU menu = nullptr;
      std::unordered_map<UINT, std::string> command_ids;
      std::unordered_map<std::string, menu_item_handle> item_handles;
    };

    struct tray_entry {
      UINT uid = 0;
      HICON icon = nullptr;
      bool owns_icon = false;
      HMENU menu = nullptr;
      std::unordered_map<UINT, std::string> command_ids;
      std::wstring tooltip;
    };

    struct hotkey_entry {
      int id = 0;
      std::string accelerator;
      std::function<void()> callback;
    };

    struct tray_listener_entry {
      int tray_id = -1;
      tray_event_kind kind = tray_event_kind::click;
      std::function<void()> callback;
    };

    std::mutex g_win32_mu;
    std::condition_variable g_hidden_cv;
    HWND g_hidden_hwnd = nullptr;
    DWORD g_hidden_thread_id = 0;
    std::thread g_hidden_thread;
    std::once_flag g_hidden_once;
    bool g_hidden_ready = false;
    bool g_hidden_failed = false;

    HWND g_active_hwnd = nullptr;
    HMENU g_application_menu = nullptr;
    std::unordered_map<UINT, std::string> g_application_menu_ids;
    std::unordered_map<std::string, menu_item_handle> g_application_menu_items;
    int g_pending_badge_count = 0;

    std::atomic<int> g_tray_seq{1};
    std::atomic<int> g_hotkey_seq{1};
    std::atomic<int> g_notification_seq{1};
    std::atomic<UINT> g_menu_command_seq{1000};
    std::atomic<int> g_tray_listener_seq{1};
    std::unordered_map<int, tray_entry> g_trays;
    std::unordered_map<int, hotkey_entry> g_hotkeys_by_id;
    std::unordered_map<std::string, int> g_hotkeys_by_accelerator;
    std::unordered_map<int, std::function<void()>> g_notification_callbacks;
    std::unordered_map<int, std::function<void(const std::string&, std::optional<std::string>)>>
        g_notification_action_callbacks;
    std::unordered_map<UINT, int> g_balloon_uid_to_notification;
    std::map<int, tray_listener_entry> g_tray_listeners;

    void log_once(bool& flag, const char* message) {
      if (!flag) {
        flag = true;
        FXE_WARN("os.win32", "{}", message);
      }
    }

    std::wstring xml_escape(std::string_view text) {
      std::wstring w = widen(text);
      std::wstring out;
      out.reserve(w.size());
      for (wchar_t ch : w) {
        switch (ch) {
        case L'&':
          out += L"&amp;";
          break;
        case L'<':
          out += L"&lt;";
          break;
        case L'>':
          out += L"&gt;";
          break;
        case L'"':
          out += L"&quot;";
          break;
        case L'\'':
          out += L"&apos;";
          break;
        default:
          out.push_back(ch);
          break;
        }
      }
      return out;
    }

    std::string parse_toast_action_id(std::string_view arguments) {
      constexpr std::string_view marker = ";action=";
      usize pos = arguments.find(marker);
      if (pos == std::string_view::npos)
        return {};
      pos += marker.size();
      return std::string(arguments.substr(pos));
    }

    std::string normalize_accelerator(std::string_view acc) {
      std::string out(acc);
      out.erase(std::remove_if(out.begin(), out.end(),
                               [](unsigned char ch) { return std::isspace(ch) != 0; }),
                out.end());
      return out;
    }

    UINT map_virtual_key_name(std::string_view key) {
      std::string s = ascii_lower(key);
      if (s.size() == 1) {
        unsigned char ch = static_cast<unsigned char>(s[0]);
        if (ch >= 'a' && ch <= 'z')
          return static_cast<UINT>('A' + (ch - 'a'));
        if (ch >= '0' && ch <= '9')
          return static_cast<UINT>(ch);
      }
      if (s.size() >= 2 && s[0] == 'f') {
        int n = 0;
        for (usize i = 1; i < s.size(); ++i) {
          if (!std::isdigit(static_cast<unsigned char>(s[i])))
            return 0;
          n = n * 10 + (s[i] - '0');
        }
        if (n >= 1 && n <= 24)
          return VK_F1 + static_cast<UINT>(n - 1);
      }
      static const std::pair<const char*, UINT> table[] = {
          {"space", VK_SPACE},     {"spacebar", VK_SPACE},  {"tab", VK_TAB},
          {"enter", VK_RETURN},    {"return", VK_RETURN},   {"escape", VK_ESCAPE},
          {"esc", VK_ESCAPE},      {"backspace", VK_BACK},  {"delete", VK_DELETE},
          {"del", VK_DELETE},      {"insert", VK_INSERT},   {"ins", VK_INSERT},
          {"home", VK_HOME},       {"end", VK_END},         {"pageup", VK_PRIOR},
          {"pagedown", VK_NEXT},   {"left", VK_LEFT},       {"right", VK_RIGHT},
          {"up", VK_UP},           {"down", VK_DOWN},       {"plus", VK_OEM_PLUS},
          {"+", VK_OEM_PLUS},      {"minus", VK_OEM_MINUS}, {"-", VK_OEM_MINUS},
          {"comma", VK_OEM_COMMA}, {",", VK_OEM_COMMA},     {"period", VK_OEM_PERIOD},
          {".", VK_OEM_PERIOD},    {"slash", VK_OEM_2},     {"/", VK_OEM_2},
          {"backslash", VK_OEM_5}, {"\\", VK_OEM_5},        {"semicolon", VK_OEM_1},
          {";", VK_OEM_1},         {"quote", VK_OEM_7},     {"'", VK_OEM_7},
      };
      for (const auto& [name, vk] : table) {
        if (s == name)
          return vk;
      }
      return 0;
    }

    std::optional<parsed_accelerator> parse_accelerator(std::string_view accelerator) {
      std::string normalized = normalize_accelerator(accelerator);
      if (normalized.empty())
        return std::nullopt;

      std::vector<std::string_view> parts;
      usize start = 0;
      while (start <= normalized.size()) {
        usize plus = normalized.find('+', start);
        if (plus == std::string::npos) {
          parts.emplace_back(normalized.data() + start, normalized.size() - start);
          break;
        }
        parts.emplace_back(normalized.data() + start, plus - start);
        start = plus + 1;
      }
      if (parts.empty() || parts.back().empty())
        return std::nullopt;

      parsed_accelerator out;
      for (usize i = 0; i + 1 < parts.size(); ++i) {
        std::string token = ascii_lower(parts[i]);
        if (token == "ctrl" || token == "control" || token == "cmdorctrl" ||
            token == "commandorcontrol")
          out.modifiers |= MOD_CONTROL;
        else if (token == "shift")
          out.modifiers |= MOD_SHIFT;
        else if (token == "alt" || token == "option")
          out.modifiers |= MOD_ALT;
        else if (token == "cmd" || token == "command" || token == "meta" || token == "super" ||
                 token == "win" || token == "windows")
          out.modifiers |= MOD_WIN;
        else
          return std::nullopt;
      }
      out.modifiers |= MOD_NOREPEAT;
      out.vk = map_virtual_key_name(parts.back());
      if (out.vk == 0)
        return std::nullopt;
      return out;
    }

    void copy_tip(wchar_t (&dst)[128], const std::wstring& src) {
      std::wmemset(dst, 0, 128);
      if (!src.empty())
        std::wcsncpy(dst, src.c_str(), 127);
    }

    void destroy_icon_if_owned(HICON icon, bool owned) {
      if (icon && owned)
        DestroyIcon(icon);
    }

    HICON load_png_icon(std::string_view icon_path) {
      std::wstring path = widen(icon_path);
      if (path.empty())
        return nullptr;

      co_scope co;
      if (!co.usable())
        return nullptr;

      Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
      if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory))))
        return nullptr;

      Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
      if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &decoder)))
        return nullptr;

      Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
      if (FAILED(decoder->GetFrame(0, &frame)))
        return nullptr;

      Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
      if (FAILED(factory->CreateBitmapScaler(&scaler)))
        return nullptr;
      if (FAILED(scaler->Initialize(frame.Get(), 32, 32, WICBitmapInterpolationModeFant)))
        return nullptr;

      Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
      if (FAILED(factory->CreateFormatConverter(&converter)))
        return nullptr;
      if (FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeMedianCut)))
        return nullptr;

      BITMAPV5HEADER bi{};
      bi.bV5Size = sizeof(bi);
      bi.bV5Width = 32;
      bi.bV5Height = -32;
      bi.bV5Planes = 1;
      bi.bV5BitCount = 32;
      bi.bV5Compression = BI_BITFIELDS;
      bi.bV5RedMask = 0x00ff0000;
      bi.bV5GreenMask = 0x0000ff00;
      bi.bV5BlueMask = 0x000000ff;
      bi.bV5AlphaMask = 0xff000000;

      void* bits = nullptr;
      HDC screen = GetDC(nullptr);
      HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                       &bits, nullptr, 0);
      ReleaseDC(nullptr, screen);
      if (!color || !bits)
        return nullptr;

      HRESULT hr = converter->CopyPixels(nullptr, 32 * 4, 32 * 32 * 4, static_cast<BYTE*>(bits));
      if (FAILED(hr)) {
        DeleteObject(color);
        return nullptr;
      }

      std::vector<BYTE> mask_bits((32 * 32) / 8, 0);
      HBITMAP mask = CreateBitmap(32, 32, 1, 1, mask_bits.data());
      if (!mask) {
        DeleteObject(color);
        return nullptr;
      }
      ICONINFO ii{};
      ii.fIcon = TRUE;
      ii.hbmColor = color;
      ii.hbmMask = mask;
      HICON icon = CreateIconIndirect(&ii);
      DeleteObject(mask);
      DeleteObject(color);
      return icon;
    }

    HICON make_badge_icon(int n) {
      if (n <= 0)
        return nullptr;
      constexpr int size = 32;
      HDC screen = GetDC(nullptr);
      HDC dc = CreateCompatibleDC(screen);
      HBITMAP color = CreateCompatibleBitmap(screen, size, size);
      std::vector<BYTE> mask_bits((size * size) / 8, 0);
      HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());
      ReleaseDC(nullptr, screen);
      if (!dc || !color || !mask) {
        if (dc)
          DeleteDC(dc);
        if (color)
          DeleteObject(color);
        if (mask)
          DeleteObject(mask);
        return nullptr;
      }

      HGDIOBJ old = SelectObject(dc, color);
      HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
      RECT rc{0, 0, size, size};
      FillRect(dc, &rc, clear);
      DeleteObject(clear);
      HBRUSH red = CreateSolidBrush(RGB(220, 0, 0));
      HGDIOBJ old_brush = SelectObject(dc, red);
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(160, 0, 0));
      HGDIOBJ old_pen = SelectObject(dc, pen);
      Ellipse(dc, 1, 1, size - 1, size - 1);
      SelectObject(dc, old_pen);
      SelectObject(dc, old_brush);
      DeleteObject(pen);
      DeleteObject(red);

      std::wstring text = n > 99 ? L"99+" : std::to_wstring(n);
      HFONT font = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (old_font)
        SelectObject(dc, old_font);
      if (font)
        DeleteObject(font);
      SelectObject(dc, old);
      DeleteDC(dc);

      ICONINFO ii{};
      ii.fIcon = TRUE;
      ii.hbmColor = color;
      ii.hbmMask = mask;
      HICON icon = CreateIconIndirect(&ii);
      DeleteObject(color);
      DeleteObject(mask);
      return icon;
    }

    void apply_badge_to_hwnd(HWND hwnd, int n) {
      if (!hwnd || !IsWindow(hwnd))
        return;
      co_scope co;
      if (!co.usable())
        return;
      Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
      if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&taskbar))))
        return;
      if (FAILED(taskbar->HrInit()))
        return;
      HICON icon = make_badge_icon(n);
      taskbar->SetOverlayIcon(hwnd, icon, n > 0 ? L"fxe badge count" : nullptr);
      if (icon)
        DestroyIcon(icon);
    }

    UINT next_menu_command_id() {
      UINT id = g_menu_command_seq.fetch_add(1);
      if (id >= 0xf000u) {
        g_menu_command_seq.store(1000);
        id = g_menu_command_seq.fetch_add(1);
      }
      return id;
    }

    std::wstring menu_item_label_text(const std::wstring& label, const std::wstring& accelerator) {
      std::wstring text = label;
      if (!accelerator.empty()) {
        text += L"\t";
        text += accelerator;
      }
      return text;
    }

    UINT menu_item_state_flags(const menu_item& item) {
      UINT flags = 0;
      if (!item.enabled)
        flags |= MF_GRAYED;
      if (item.type == "checkbox" && item.checked)
        flags |= MF_CHECKED;
      return flags;
    }

    HMENU build_hmenu(const std::vector<menu_item>& items,
                      std::unordered_map<UINT, std::string>* command_ids,
                      std::unordered_map<std::string, menu_item_handle>* item_handles,
                      bool top_level) {
      HMENU menu = top_level ? CreateMenu() : CreatePopupMenu();
      if (!menu)
        return nullptr;

      for (const auto& item : items) {
        UINT position = static_cast<UINT>(GetMenuItemCount(menu));
        if (item.type == "separator") {
          AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
          continue;
        }

        UINT state_flags = menu_item_state_flags(item);
        std::wstring label = widen(item.label);
        if (label.empty() && !item.label.empty())
          label = L" ";
        std::wstring accelerator = widen(item.accelerator);
        std::wstring text = menu_item_label_text(label, accelerator);

        if (item.type == "submenu" || !item.submenu.empty()) {
          HMENU child = build_hmenu(item.submenu, command_ids, item_handles, false);
          if (child &&
              AppendMenuW(menu, MF_POPUP | MF_STRING | state_flags,
                          reinterpret_cast<UINT_PTR>(child), text.c_str()) &&
              item_handles && !item.id.empty()) {
            (*item_handles)[item.id] = menu_item_handle{menu, child,       0,     position,   true,
                                                        true, state_flags, label, accelerator};
          }
          continue;
        }

        UINT command_id = next_menu_command_id();
        if (AppendMenuW(menu, MF_STRING | state_flags, command_id, text.c_str())) {
          if (command_ids)
            (*command_ids)[command_id] = item.id;
          if (item_handles && !item.id.empty()) {
            (*item_handles)[item.id] = menu_item_handle{
                menu, nullptr, command_id, position, false, true, state_flags, label, accelerator};
          }
        }
      }
      return menu;
    }

    menu_build_result build_menu_result(const std::vector<menu_item>& items, bool top_level) {
      menu_build_result result;
      result.menu = build_hmenu(items, &result.command_ids, &result.item_handles, top_level);
      return result;
    }

    void sync_menu_positions_locked(HMENU parent) {
      if (!parent)
        return;
      int count = GetMenuItemCount(parent);
      for (int pos = 0; pos < count; ++pos) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(parent, static_cast<UINT>(pos), TRUE, &info))
          continue;
        for (auto& [_, entry] : g_application_menu_items) {
          if (!entry.visible || entry.parent != parent)
            continue;
          if ((entry.is_submenu && entry.submenu == info.hSubMenu) ||
              (!entry.is_submenu && entry.command_id == info.wID)) {
            entry.position = static_cast<UINT>(pos);
            break;
          }
        }
      }
    }

    bool apply_menu_item_locked(menu_item_handle& entry) {
      if (!entry.visible)
        return true;
      std::wstring text = menu_item_label_text(entry.label, entry.accelerator);
      UINT flags = MF_BYPOSITION | MF_STRING | entry.state_flags;
      UINT_PTR item_id = entry.command_id;
      if (entry.is_submenu) {
        flags |= MF_POPUP;
        item_id = reinterpret_cast<UINT_PTR>(entry.submenu);
      }
      return ModifyMenuW(entry.parent, entry.position, flags, item_id, text.c_str()) != FALSE;
    }

    bool set_menu_item_visible_locked(menu_item_handle& entry, bool visible) {
      if (entry.visible == visible)
        return true;
      if (!visible) {
        sync_menu_positions_locked(entry.parent);
        if (!RemoveMenu(entry.parent, entry.position, MF_BYPOSITION))
          return false;
        entry.visible = false;
        sync_menu_positions_locked(entry.parent);
        return true;
      }
      std::wstring text = menu_item_label_text(entry.label, entry.accelerator);
      UINT flags = MF_BYPOSITION | MF_STRING | entry.state_flags;
      UINT_PTR item_id = entry.command_id;
      if (entry.is_submenu) {
        flags |= MF_POPUP;
        item_id = reinterpret_cast<UINT_PTR>(entry.submenu);
      }
      int count = GetMenuItemCount(entry.parent);
      UINT insert_pos = entry.position;
      if (count >= 0 && insert_pos > static_cast<UINT>(count))
        insert_pos = static_cast<UINT>(count);
      if (!InsertMenuW(entry.parent, insert_pos, flags, item_id, text.c_str()))
        return false;
      entry.visible = true;
      entry.position = insert_pos;
      sync_menu_positions_locked(entry.parent);
      return true;
    }

    void redraw_application_menu_locked() {
      if (g_active_hwnd && IsWindow(g_active_hwnd))
        DrawMenuBar(g_active_hwnd);
    }

    void dispatch_tray_event(int tray_id, tray_event_kind kind) {
      std::vector<std::function<void()>> callbacks;
      {
        std::lock_guard<std::mutex> lock(g_win32_mu);
        for (const auto& [_, listener] : g_tray_listeners) {
          if (listener.tray_id == tray_id && listener.kind == kind && listener.callback)
            callbacks.push_back(listener.callback);
        }
      }
      for (auto& cb : callbacks)
        post_main_thread_dispatch(std::move(cb));
    }

    void dispatch_menu_command(UINT command_id) {
      std::string id;
      {
        std::lock_guard<std::mutex> lock(g_win32_mu);
        auto it = g_application_menu_ids.find(command_id);
        if (it != g_application_menu_ids.end())
          id = it->second;
      }
      if (!id.empty()) {
        post_main_thread_dispatch(
            [id]() { fxe::os::detail::dispatch_application_menu_command(id); });
      }
    }

    LRESULT CALLBACK active_hwnd_subclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                          UINT_PTR, DWORD_PTR) {
      if (msg == WM_COMMAND && HIWORD(wparam) == 0) {
        dispatch_menu_command(LOWORD(wparam));
        return 0;
      }
      if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, active_hwnd_subclass, kSubclassId);
        SetMenu(hwnd, nullptr);
        std::lock_guard<std::mutex> lock(g_win32_mu);
        if (g_active_hwnd == hwnd)
          g_active_hwnd = nullptr;
      }
      return DefSubclassProc(hwnd, msg, wparam, lparam);
    }

    void apply_application_menu_locked() {
      if (!g_active_hwnd || !IsWindow(g_active_hwnd) || !g_application_menu)
        return;
      SetWindowSubclass(g_active_hwnd, active_hwnd_subclass, kSubclassId, 0);
      SetMenu(g_active_hwnd, g_application_menu);
      DrawMenuBar(g_active_hwnd);
    }

    void invoke_notification_callback(int id) {
      std::function<void()> cb;
      {
        std::lock_guard<std::mutex> lock(g_win32_mu);
        auto it = g_notification_callbacks.find(id);
        if (it != g_notification_callbacks.end()) {
          cb = std::move(it->second);
          g_notification_callbacks.erase(it);
        }
        g_notification_action_callbacks.erase(id);
      }
      if (cb)
        post_main_thread_dispatch(std::move(cb));
    }

    void invoke_notification_action_callback(int id, std::string action_id,
                                             std::optional<std::string> input) {
      std::function<void(const std::string&, std::optional<std::string>)> cb;
      {
        std::lock_guard<std::mutex> lock(g_win32_mu);
        auto it = g_notification_action_callbacks.find(id);
        if (it != g_notification_action_callbacks.end()) {
          cb = std::move(it->second);
          g_notification_action_callbacks.erase(it);
        }
        g_notification_callbacks.erase(id);
      }
      if (cb) {
        post_main_thread_dispatch(
            [cb = std::move(cb), action_id = std::move(action_id),
             input = std::move(input)]() mutable { cb(action_id, std::move(input)); });
      }
    }

    void handle_tray_message(WPARAM wparam, LPARAM lparam) {
      UINT event = LOWORD(lparam);
      UINT uid = HIWORD(lparam);
      bool version4 = uid != 0;
      if (!version4)
        uid = static_cast<UINT>(wparam);
      if (uid >= kBalloonUidBase) {
        if (event == NIN_BALLOONUSERCLICK || event == NIN_SELECT || event == WM_LBUTTONUP) {
          int notification_id = 0;
          {
            std::lock_guard<std::mutex> lock(g_win32_mu);
            auto it = g_balloon_uid_to_notification.find(uid);
            if (it != g_balloon_uid_to_notification.end())
              notification_id = it->second;
          }
          if (notification_id)
            invoke_notification_callback(notification_id);
        }
        return;
      }

      if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
        dispatch_tray_event(static_cast<int>(uid), tray_event_kind::right_click);
        HMENU menu = nullptr;
        std::unordered_map<UINT, std::string> ids;
        {
          std::lock_guard<std::mutex> lock(g_win32_mu);
          auto it = g_trays.find(static_cast<int>(uid));
          if (it == g_trays.end())
            return;
          menu = it->second.menu;
          ids = it->second.command_ids;
        }
        if (!menu)
          return;
        POINT pt{};
        if (event == WM_CONTEXTMENU && version4) {
          pt.x = GET_X_LPARAM(wparam);
          pt.y = GET_Y_LPARAM(wparam);
        }
        if (pt.x == 0 && pt.y == 0)
          GetCursorPos(&pt);
        SetForegroundWindow(g_hidden_hwnd);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                  g_hidden_hwnd, nullptr);
        PostMessageW(g_hidden_hwnd, WM_NULL, 0, 0);
        auto id_it = ids.find(cmd);
        if (id_it != ids.end()) {
          std::string id = id_it->second;
          post_main_thread_dispatch([id]() { (void)id; });
        }
      } else if (event == WM_LBUTTONDBLCLK) {
        dispatch_tray_event(static_cast<int>(uid), tray_event_kind::double_click);
      } else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) {
        dispatch_tray_event(static_cast<int>(uid), tray_event_kind::click);
      }
    }

    LRESULT CALLBACK hidden_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
      switch (msg) {
      case WM_HOTKEY: {
        std::function<void()> cb;
        {
          std::lock_guard<std::mutex> lock(g_win32_mu);
          auto it = g_hotkeys_by_id.find(static_cast<int>(wparam));
          if (it != g_hotkeys_by_id.end())
            cb = it->second.callback;
        }
        if (cb)
          post_main_thread_dispatch(std::move(cb));
        return 0;
      }
      case kTrayMessage:
        handle_tray_message(wparam, lparam);
        return 0;
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY: {
        std::lock_guard<std::mutex> lock(g_win32_mu);
        if (g_hidden_hwnd == hwnd)
          g_hidden_hwnd = nullptr;
      }
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
      }
    }

    void destroy_win32_ui_state() {
      HWND hidden = nullptr;
      DWORD tid = 0;
      {
        std::lock_guard<std::mutex> lock(g_win32_mu);
        hidden = g_hidden_hwnd;
        tid = g_hidden_thread_id;
        if (hidden) {
          for (const auto& [id, _] : g_hotkeys_by_id)
            UnregisterHotKey(hidden, id);
          for (auto& [_, tray] : g_trays) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hidden;
            nid.uID = tray.uid;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            if (tray.menu)
              DestroyMenu(tray.menu);
            destroy_icon_if_owned(tray.icon, tray.owns_icon);
          }
          for (const auto& [uid, _] : g_balloon_uid_to_notification) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hidden;
            nid.uID = uid;
            Shell_NotifyIconW(NIM_DELETE, &nid);
          }
        }
        g_trays.clear();
        g_tray_listeners.clear();
        g_hotkeys_by_id.clear();
        g_hotkeys_by_accelerator.clear();
        g_balloon_uid_to_notification.clear();
        g_notification_callbacks.clear();
        g_notification_action_callbacks.clear();
        g_application_menu_items.clear();
        if (g_active_hwnd && IsWindow(g_active_hwnd))
          SetMenu(g_active_hwnd, nullptr);
        if (g_application_menu) {
          DestroyMenu(g_application_menu);
          g_application_menu = nullptr;
        }
      }
      if (hidden)
        PostMessageW(hidden, WM_CLOSE, 0, 0);
      else if (tid)
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
      if (g_hidden_thread.joinable())
        g_hidden_thread.join();
    }

    void hidden_thread_main() {
      WNDCLASSW wc{};
      wc.lpfnWndProc = hidden_wndproc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = kHiddenWindowClass;
      RegisterClassW(&wc);
      HWND hwnd = CreateWindowExW(0, kHiddenWindowClass, L"fxe hidden window", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
      {
        std::lock_guard<std::mutex> lock(g_win32_mu);
        g_hidden_thread_id = GetCurrentThreadId();
        g_hidden_hwnd = hwnd;
        g_hidden_ready = hwnd != nullptr;
        g_hidden_failed = hwnd == nullptr;
      }
      g_hidden_cv.notify_all();
      if (!hwnd)
        return;

      MSG msg{};
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }

    HWND ensure_hidden_window() {
      std::call_once(g_hidden_once, [] {
        g_hidden_thread = std::thread(hidden_thread_main);
        std::atexit(destroy_win32_ui_state);
      });
      std::unique_lock<std::mutex> lock(g_win32_mu);
      g_hidden_cv.wait(lock, [] { return g_hidden_ready || g_hidden_failed; });
      return g_hidden_hwnd;
    }

    bool show_tray_balloon_notification(int id, const notification_options& opts) {
      HWND hwnd = ensure_hidden_window();
      if (!hwnd)
        return false;
      UINT uid = kBalloonUidBase + static_cast<UINT>(id);
      NOTIFYICONDATAW nid{};
      nid.cbSize = sizeof(nid);
      nid.hWnd = hwnd;
      nid.uID = uid;
      nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
      nid.uCallbackMessage = kTrayMessage;
      nid.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
      copy_tip(nid.szTip, L"fxe");
      std::wstring title = widen(opts.title);
      std::wstring body = widen(opts.body);
      if (title.empty())
        title = L"fxe";
      std::wcsncpy(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle) - 1);
      std::wcsncpy(nid.szInfo, body.c_str(), ARRAYSIZE(nid.szInfo) - 1);
      nid.dwInfoFlags = NIIF_INFO;
      {
        std::lock_guard<std::mutex> lock(g_win32_mu);
        g_balloon_uid_to_notification[uid] = id;
      }
      BOOL added = Shell_NotifyIconW(NIM_ADD, &nid);
      nid.uVersion = NOTIFYICON_VERSION_4;
      if (added)
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
      BOOL shown = Shell_NotifyIconW(added ? NIM_MODIFY : NIM_ADD, &nid);
      if (!added && shown)
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
      return added || shown;
    }

    struct hstring_handle {
      HSTRING value = nullptr;
      hstring_handle() = default;
      explicit hstring_handle(const wchar_t* s) {
        WindowsCreateString(s, static_cast<UINT32>(std::wcslen(s)), &value);
      }
      explicit hstring_handle(const std::wstring& s) {
        WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), &value);
      }
      ~hstring_handle() {
        if (value)
          WindowsDeleteString(value);
      }
      HSTRING get() const noexcept {
        return value;
      }
      explicit operator bool() const noexcept {
        return value != nullptr;
      }
      hstring_handle(const hstring_handle&) = delete;
      hstring_handle& operator=(const hstring_handle&) = delete;
    };

    bool show_winrt_notification(int id, const notification_options& opts) {
      HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
      bool uninit = SUCCEEDED(init);
      if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        return false;

      auto cleanup = [&]() {
        if (uninit)
          RoUninitialize();
      };

      using ABI::Windows::Data::Xml::Dom::IXmlDocument;
      using ABI::Windows::Data::Xml::Dom::IXmlDocumentIO;
      using ABI::Windows::Foundation::IPropertyValue;
      using ABI::Windows::Foundation::ITypedEventHandler;
      using ABI::Windows::Foundation::Collections::IPropertySet;
      using ABI::Windows::UI::Notifications::IToastActivatedEventArgs;
      using ABI::Windows::UI::Notifications::IToastActivatedEventArgs2;
      using ABI::Windows::UI::Notifications::IToastNotification;
      using ABI::Windows::UI::Notifications::IToastNotificationFactory;
      using ABI::Windows::UI::Notifications::IToastNotificationManagerStatics;
      using ABI::Windows::UI::Notifications::IToastNotifier;
      using Microsoft::WRL::Callback;
      using Microsoft::WRL::ComPtr;

      hstring_handle manager_class(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager);
      hstring_handle xml_class(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument);
      hstring_handle app_id(L"fxe");
      if (!manager_class || !xml_class || !app_id) {
        cleanup();
        return false;
      }

      ComPtr<IToastNotificationManagerStatics> manager;
      if (FAILED(RoGetActivationFactory(manager_class.get(), IID_PPV_ARGS(&manager)))) {
        cleanup();
        return false;
      }
      ComPtr<IToastNotifier> notifier;
      if (FAILED(manager->CreateToastNotifierWithId(app_id.get(), &notifier))) {
        cleanup();
        return false;
      }

      ComPtr<IInspectable> inspectable;
      if (FAILED(RoActivateInstance(xml_class.get(), &inspectable))) {
        cleanup();
        return false;
      }
      ComPtr<IXmlDocument> xml_doc;
      ComPtr<IXmlDocumentIO> xml_io;
      if (FAILED(inspectable.As(&xml_doc)) || FAILED(inspectable.As(&xml_io))) {
        cleanup();
        return false;
      }

      std::wstring xml = L"<toast launch=\"fxe-notification-";
      xml += std::to_wstring(id);
      xml += L"\"><visual><binding template=\"ToastGeneric\"><text>";
      xml +=
          xml_escape(opts.title.empty() ? std::string_view("fxe") : std::string_view(opts.title));
      xml += L"</text><text>";
      xml += xml_escape(opts.body);
      xml += L"</text>";
      if (opts.hero_image_path && !opts.hero_image_path->empty()) {
        xml += L"<image placement=\"hero\" src=\"";
        xml += xml_escape(*opts.hero_image_path);
        xml += L"\"/>";
      }
      if (opts.app_logo_image_path && !opts.app_logo_image_path->empty()) {
        xml += L"<image placement=\"appLogoOverride\" src=\"";
        xml += xml_escape(*opts.app_logo_image_path);
        xml += L"\"/>";
      }
      if (opts.image_path && !opts.image_path->empty()) {
        xml += L"<image placement=\"inline\" src=\"";
        xml += xml_escape(*opts.image_path);
        xml += L"\"/>";
      }
      xml += L"</binding></visual>";
      if (opts.attachment_path && !opts.attachment_path->empty()) {
        xml += L"<audio src=\"";
        xml += xml_escape(*opts.attachment_path);
        xml += L"\"/>";
      }
      xml += L"<actions><action content=\"Open\" arguments=\"fxe-notification-";
      xml += std::to_wstring(id);
      xml += L"\" activationType=\"foreground\"/>";
      for (const auto& action : opts.actions) {
        if (action.id.empty() || action.title.empty())
          continue;
        std::wstring action_id = xml_escape(action.id);
        if (action.kind == notification_action_kind::input) {
          xml += L"<input id=\"";
          xml += action_id;
          xml += L"\" type=\"text\"/>";
        }
        xml += L"<action content=\"";
        xml += xml_escape(action.title);
        xml += L"\" arguments=\"fxe-notification-";
        xml += std::to_wstring(id);
        xml += L";action=";
        xml += action_id;
        xml += L"\" activationType=\"foreground\"";
        if (action.kind == notification_action_kind::input) {
          xml += L" hint-inputId=\"";
          xml += action_id;
          xml += L"\"";
        }
        xml += L"/>";
      }
      xml += L"</actions></toast>";

      hstring_handle xml_text(xml);
      if (!xml_text || FAILED(xml_io->LoadXml(xml_text.get()))) {
        cleanup();
        return false;
      }

      hstring_handle toast_class(RuntimeClass_Windows_UI_Notifications_ToastNotification);
      ComPtr<IToastNotificationFactory> toast_factory;
      if (!toast_class ||
          FAILED(RoGetActivationFactory(toast_class.get(), IID_PPV_ARGS(&toast_factory)))) {
        cleanup();
        return false;
      }
      ComPtr<IToastNotification> toast;
      if (FAILED(toast_factory->CreateToastNotification(xml_doc.Get(), &toast))) {
        cleanup();
        return false;
      }
      auto activated = Callback<ITypedEventHandler<IToastNotification*, IInspectable*>>(
          [id](IToastNotification*, IInspectable* args) -> HRESULT {
            std::string arguments;
            ComPtr<IToastActivatedEventArgs> activated_args;
            if (args && SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&activated_args)))) {
              HSTRING raw = nullptr;
              if (SUCCEEDED(activated_args->get_Arguments(&raw))) {
                arguments = narrow_hstring(raw);
                WindowsDeleteString(raw);
              }
            }
            std::string action_id = parse_toast_action_id(arguments);
            std::optional<std::string> input;
            if (!action_id.empty()) {
              ComPtr<IToastActivatedEventArgs2> activated_args2;
              if (activated_args && SUCCEEDED(activated_args.As(&activated_args2))) {
                ComPtr<IPropertySet> user_input;
                if (SUCCEEDED(activated_args2->get_UserInput(&user_input)) && user_input) {
                  hstring_handle key(widen(action_id));
                  ComPtr<IInspectable> value;
                  if (key && SUCCEEDED(user_input->Lookup(key.get(), &value)) && value) {
                    ComPtr<IPropertyValue> property_value;
                    if (SUCCEEDED(value.As(&property_value)) && property_value) {
                      HSTRING raw_input = nullptr;
                      if (SUCCEEDED(property_value->GetString(&raw_input))) {
                        input = narrow_hstring(raw_input);
                        WindowsDeleteString(raw_input);
                      }
                    }
                  }
                }
              }
            }
            if (action_id.empty())
              invoke_notification_callback(id);
            else
              invoke_notification_action_callback(id, std::move(action_id), std::move(input));
            return S_OK;
          });
      EventRegistrationToken activated_token{};
      if (activated)
        (void)toast->add_Activated(activated.Get(), &activated_token);
      HRESULT shown = notifier->Show(toast.Get());
      cleanup();
      return SUCCEEDED(shown);
    }
  } // namespace

  std::string get_path(std::string_view kind) {
    if (kind == "userData")
      return known_folder(FOLDERID_LocalAppData);
    if (kind == "documents")
      return known_folder(FOLDERID_Documents);
    if (kind == "downloads")
      return known_folder(FOLDERID_Downloads);
    if (kind == "home")
      return known_folder(FOLDERID_Profile);
    if (kind == "temp") {
      DWORD n = GetTempPathW(0, nullptr);
      if (n == 0)
        return {};
      std::wstring buf(static_cast<usize>(n), L'\0');
      DWORD written = GetTempPathW(n, buf.data());
      if (written == 0 || written >= n)
        return {};
      buf.resize(written);
      return narrow(buf.c_str());
    }
    return {};
  }
  bool system_prefers_reduced_motion() {
    BOOL animations_enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations_enabled, 0))
      return false;
    return animations_enabled == FALSE;
  }

  bool system_prefers_high_contrast() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
      return false;
    return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
  }

  double system_font_scale() {
    auto percent = read_reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility",
                                  L"TextScaleFactor");
    if (!percent || *percent == 0)
      return 1.0;
    return static_cast<double>(*percent) / 100.0;
  }

  std::string system_color_scheme() {
    auto use_light = read_reg_dword(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme");
    if (!use_light)
      return "no-preference";
    return *use_light == 0 ? "dark" : "light";
  }

  std::string system_accent_color() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (DwmGetColorizationColor(&color, &opaque) != S_OK)
      return {};
    (void)opaque;
    unsigned r = static_cast<unsigned>((color >> 16) & 0xFFu);
    unsigned g = static_cast<unsigned>((color >> 8) & 0xFFu);
    unsigned b = static_cast<unsigned>(color & 0xFFu);
    char hex[7] = {};
    std::snprintf(hex, sizeof(hex), "%02x%02x%02x", r, g, b);
    return std::string(hex);
  }

  bool install_system_change_observer(std::function<void(const char* kind)> cb) {
    (void)cb;
    return false;
  }
  bool open_external(std::string_view url) {
    std::wstring target = widen(url);
    if (target.empty())
      return false;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
  }

  bool show_item_in_folder(std::string_view path8) {
    std::wstring path = widen(path8);
    if (path.empty())
      return false;

    co_scope co;
    if (co.usable()) {
      PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
      if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr))
          return true;
      }
    }
    return explorer_select(path);
  }

  void beep() {
    MessageBeep(MB_OK);
  }

  bool trash_item(std::string_view path8) {
    std::wstring path = widen(path8);
    if (path.empty())
      return false;

    co_scope co;
    if (co.usable()) {
      IFileOperation* op = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&op)))) {
        op->SetOperationFlags(FOR_ALLOWUNDO | FOR_NOCONFIRMATION | FOR_NOERRORUI);
        IShellItem* item = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr)) {
          hr = op->DeleteItem(item, nullptr);
          if (SUCCEEDED(hr))
            hr = op->PerformOperations();
          item->Release();
        }
        op->Release();
        if (SUCCEEDED(hr))
          return true;
      }
    }

    std::wstring from = path;
    from.push_back(L'\0');
    SHFILEOPSTRUCTW fo{};
    fo.wFunc = FO_DELETE;
    fo.pFrom = from.c_str();
    fo.fFlags = FOR_ALLOWUNDO | FOR_NOCONFIRMATION | FOR_NOERRORUI;
    return SHFileOperationW(&fo) == 0 && !fo.fAnyOperationsAborted;
  }

  bool request_single_instance_lock(std::string_view app_id) {
    std::lock_guard<std::mutex> lock(g_instance_mu);
    if (g_instance_mutex)
      return true;

    std::wstring id = widen(app_id.empty() ? std::string_view("fxe") : app_id);
    if (id.empty())
      return false;
    for (wchar_t& ch : id) {
      if (ch == L'\\' || ch == L'/' || ch == L':' || ch < 0x20)
        ch = L'_';
    }
    std::wstring name = L"Local\\fxe_single_instance_" + id;
    HANDLE h = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!h)
      return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(h);
      return false;
    }
    g_instance_mutex = h;
    return true;
  }

  void set_badge_count(int n) {
    HWND hwnd = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      g_pending_badge_count = n;
      hwnd = g_active_hwnd;
    }
    if (hwnd && IsWindow(hwnd)) {
      apply_badge_to_hwnd(hwnd, n);
    } else {
      static bool logged = false;
      log_once(logged, "no active Win32 HWND registered for taskbar badge yet");
    }
  }

  void relaunch() {
    std::wstring exe;
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
      DWORD written = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
      if (written == 0)
        return;
      if (written < static_cast<DWORD>(buf.size())) {
        exe.assign(buf.data(), written);
        break;
      }
      buf.resize(buf.size() * 2);
    }

    std::wstring command = quote_arg(exe);
    if (create_process(exe, command))
      ExitProcess(0);
  }

  std::optional<std::string> bookmark_persist(std::string_view path) {
    return std::string(path);
  }

  std::optional<std::pair<std::string, bool>> bookmark_resolve(std::string_view blob) {
    return std::make_pair(std::string(blob), false);
  }

  bool bookmark_start_access(std::string_view blob) {
    (void)blob;
    return true;
  }

  void bookmark_stop_access(std::string_view blob) {
    (void)blob;
  }

  std::vector<std::string> show_open_dialog(const open_dialog_options& opts) {
    co_scope co;
    if (!co.usable())
      return {};

    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
      return {};

    if (!opts.title.empty()) {
      std::wstring title = widen(opts.title);
      if (!title.empty())
        dialog->SetTitle(title.c_str());
    }
    DWORD flags = FOS_FORCEFILESYSTEM;
    if (opts.multiple)
      flags |= FOS_ALLOWMULTISELECT;
    if (opts.directories)
      flags |= FOS_PICKFOLDERS;
    dialog->SetOptions(flags);
    filter_storage filters;
    if (!opts.directories) {
      filters = make_filters(opts.filters);
      apply_filters(dialog, filters);
    }
    set_dialog_default_path(dialog, opts.default_path, false);

    std::vector<std::string> out;
    if (SUCCEEDED(dialog->Show(nullptr))) {
      IShellItemArray* items = nullptr;
      if (SUCCEEDED(dialog->GetResults(&items))) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
          IShellItem* item = nullptr;
          if (SUCCEEDED(items->GetItemAt(i, &item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
              out.push_back(narrow(path));
              CoTaskMemFree(path);
            }
            item->Release();
          }
        }
        items->Release();
      }
    }
    dialog->Release();
    return out;
  }

  std::optional<std::string> show_save_dialog(const save_dialog_options& opts) {
    co_scope co;
    if (!co.usable())
      return std::nullopt;

    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
      return std::nullopt;

    if (!opts.title.empty()) {
      std::wstring title = widen(opts.title);
      if (!title.empty())
        dialog->SetTitle(title.c_str());
    }
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    filter_storage filters = make_filters(opts.filters);
    apply_filters(dialog, filters);
    set_dialog_default_path(dialog, opts.default_path, true);

    std::optional<std::string> out;
    if (SUCCEEDED(dialog->Show(nullptr))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
          out = narrow(path);
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dialog->Release();
    return out;
  }

  int show_message_box(const message_box_options& opts) {
    std::wstring title =
        widen(opts.title.empty() ? std::string_view("fxe") : std::string_view(opts.title));
    std::string text8 = opts.message;
    if (!opts.detail.empty()) {
      if (!text8.empty())
        text8 += "\n\n";
      text8 += opts.detail;
    }
    std::wstring text = widen(text8);
    if (text.empty() && !text8.empty())
      return -1;

    UINT flags = MB_APPLMODAL;
    if (opts.type == "warning")
      flags |= MB_ICONWARNING;
    else if (opts.type == "error")
      flags |= MB_ICONERROR;
    else if (opts.type == "question")
      flags |= MB_ICONQUESTION;
    else
      flags |= MB_ICONINFORMATION;

    usize count = opts.buttons.size();
    if (count <= 1)
      flags |= MB_OK;
    else if (count == 2)
      flags |= MB_OKCANCEL;
    else
      flags |= MB_YESNOCANCEL;

    int r = MessageBoxW(nullptr, text.c_str(), title.c_str(), flags);
    if (count <= 1)
      return r == IDOK ? 0 : -1;
    if (count == 2) {
      if (r == IDOK)
        return 0;
      if (r == IDCANCEL)
        return 1;
      return -1;
    }
    if (r == IDYES)
      return 0;
    if (r == IDNO)
      return 1;
    if (r == IDCANCEL)
      return 2;
    return -1;
  }

  int show_notification(const notification_options& opts) {
    int id = g_notification_seq.fetch_add(1);
    ensure_hidden_window();
    if (show_winrt_notification(id, opts))
      return id;
    if (show_tray_balloon_notification(id, opts))
      return id;
    return 0;
  }

  int show_notification(
      const notification_options& opts,
      std::function<void(const std::string& action_id, std::optional<std::string> input)>
          on_action) {
    int id = show_notification(opts);
    if (id > 0 && on_action)
      on_notification_action(id, std::move(on_action));
    return id;
  }

  void on_notification_click(int id, std::function<void()> cb) {
    if (id <= 0)
      return;
    ensure_hidden_window();
    std::lock_guard<std::mutex> lock(g_win32_mu);
    if (cb)
      g_notification_callbacks[id] = std::move(cb);
    else
      g_notification_callbacks.erase(id);
  }

  void on_notification_action(
      int id,
      std::function<void(const std::string& action_id, std::optional<std::string> input)> cb) {
    if (id <= 0)
      return;
    ensure_hidden_window();
    std::lock_guard<std::mutex> lock(g_win32_mu);
    if (cb)
      g_notification_action_callbacks[id] = std::move(cb);
    else
      g_notification_action_callbacks.erase(id);
  }

  void set_application_menu(const std::vector<menu_item>& items) {
    menu_build_result next = build_menu_result(items, true);
    if (!next.menu)
      return;

    HMENU old = nullptr;
    HWND hwnd = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      old = g_application_menu;
      g_application_menu = next.menu;
      g_application_menu_ids = std::move(next.command_ids);
      g_application_menu_items = std::move(next.item_handles);
      hwnd = g_active_hwnd;
      apply_application_menu_locked();
    }
    if (old && old != next.menu)
      DestroyMenu(old);
    if (!hwnd || !IsWindow(hwnd)) {
      static bool logged = false;
      log_once(logged, "storing Win32 application menu until an active HWND is registered");
    }
  }

  bool update_menu_item(std::string_view id, const menu_item_patch& patch) {
    std::lock_guard<std::mutex> lock(g_win32_mu);
    auto found = g_application_menu_items.find(std::string(id));
    if (found == g_application_menu_items.end())
      return false;
    menu_item_handle& entry = found->second;
    sync_menu_positions_locked(entry.parent);
    if (patch.label) {
      entry.label = widen(*patch.label);
      if (entry.label.empty() && !patch.label->empty())
        entry.label = L" ";
    }
    if (patch.enabled) {
      entry.state_flags &= ~(MF_GRAYED | MF_DISABLED);
      if (!*patch.enabled)
        entry.state_flags |= MF_GRAYED;
    }
    if (patch.checked) {
      entry.state_flags &= ~MF_CHECKED;
      if (*patch.checked)
        entry.state_flags |= MF_CHECKED;
    }
    if (patch.enabled && entry.visible) {
      EnableMenuItem(entry.parent, entry.is_submenu ? entry.position : entry.command_id,
                     (entry.is_submenu ? MF_BYPOSITION : MF_BYCOMMAND) |
                         (*patch.enabled ? MF_ENABLED : MF_GRAYED));
    }
    if (patch.checked && entry.visible) {
      CheckMenuItem(entry.parent, entry.is_submenu ? entry.position : entry.command_id,
                    (entry.is_submenu ? MF_BYPOSITION : MF_BYCOMMAND) |
                        (*patch.checked ? MF_CHECKED : MF_UNCHECKED));
    }
    if (patch.accelerator) {
      // Win32 exposes menu accelerators as a tab-suffixed label here; actual
      // keyboard accelerator dispatch remains owned by the caller/menu binding.
      entry.accelerator = widen(*patch.accelerator);
    }
    if (patch.visible && !set_menu_item_visible_locked(entry, *patch.visible))
      return false;
    if (!apply_menu_item_locked(entry))
      return false;
    redraw_application_menu_locked();
    return true;
  }

  bool menu_item_exists(std::string_view id) {
    std::lock_guard<std::mutex> lock(g_win32_mu);
    return g_application_menu_items.find(std::string(id)) != g_application_menu_items.end();
  }

  void show_context_menu(const std::vector<menu_item>& items, int x, int y,
                         std::function<void(const std::string&)> on_select) {
    HWND owner = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      owner = g_active_hwnd;
    }
    if (!owner || !IsWindow(owner))
      owner = ensure_hidden_window();
    if (!owner) {
      if (on_select)
        post_main_thread_dispatch([cb = std::move(on_select)]() mutable { cb(std::string{}); });
      return;
    }

    menu_build_result popup = build_menu_result(items, false);
    if (!popup.menu) {
      if (on_select)
        post_main_thread_dispatch([cb = std::move(on_select)]() mutable { cb(std::string{}); });
      return;
    }
    // TrackPopupMenu wants screen coordinates; (x, y) arrive in window-client
    // coordinates (top-left origin, same as Win32 client space — matches GLFW
    // cursor-pos reporting on Windows). Convert through the owner HWND.
    POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
    ClientToScreen(owner, &pt);
    UINT cmd =
        TrackPopupMenu(popup.menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
    std::string id;
    auto it = popup.command_ids.find(cmd);
    if (it != popup.command_ids.end())
      id = it->second;
    DestroyMenu(popup.menu);
    if (on_select)
      post_main_thread_dispatch([cb = std::move(on_select), id]() mutable { cb(id); });
  }

  tray_handle tray_create(std::string_view icon_path, std::string_view tooltip) {
    HWND hwnd = ensure_hidden_window();
    if (!hwnd)
      return tray_handle{};

    int id = g_tray_seq.fetch_add(1);
    HICON icon = load_png_icon(icon_path);
    bool owns_icon = icon != nullptr;
    if (!icon)
      icon = LoadIconW(nullptr, IDI_APPLICATION);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = static_cast<UINT>(id);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = icon;
    copy_tip(nid.szTip, widen(tooltip));
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
      destroy_icon_if_owned(icon, owns_icon);
      return tray_handle{};
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    tray_entry entry;
    entry.uid = static_cast<UINT>(id);
    entry.icon = icon;
    entry.owns_icon = owns_icon;
    entry.tooltip = widen(tooltip);
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      g_trays.emplace(id, std::move(entry));
    }
    return tray_handle{id};
  }

  void tray_set_menu(tray_handle handle, const std::vector<menu_item>& items) {
    if (!handle)
      return;
    menu_build_result menu = build_menu_result(items, false);
    if (!menu.menu)
      return;
    HMENU old = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      auto it = g_trays.find(handle.id);
      if (it == g_trays.end()) {
        old = menu.menu;
      } else {
        old = it->second.menu;
        it->second.menu = menu.menu;
        it->second.command_ids = std::move(menu.command_ids);
      }
    }
    if (old)
      DestroyMenu(old);
  }

  bool tray_set_image(tray_handle handle, std::string_view icon_path) {
    if (!handle)
      return false;
    std::wstring path = widen(icon_path);
    HICON icon = nullptr;
    if (!path.empty()) {
      icon = reinterpret_cast<HICON>(
          LoadImageW(nullptr, path.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    }
    if (!icon)
      icon = load_png_icon(icon_path);
    if (!icon)
      return false;

    HICON old_icon = nullptr;
    bool old_owned = false;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      HWND hwnd = g_hidden_hwnd;
      auto it = g_trays.find(handle.id);
      if (!hwnd || it == g_trays.end()) {
        DestroyIcon(icon);
        return false;
      }
      NOTIFYICONDATAW nid{};
      nid.cbSize = sizeof(nid);
      nid.hWnd = hwnd;
      nid.uID = it->second.uid;
      nid.uFlags = NIF_ICON;
      nid.hIcon = icon;
      if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        DestroyIcon(icon);
        return false;
      }
      old_icon = it->second.icon;
      old_owned = it->second.owns_icon;
      it->second.icon = icon;
      it->second.owns_icon = true;
    }
    destroy_icon_if_owned(old_icon, old_owned);
    return true;
  }

  bool tray_set_title(tray_handle handle, std::string_view title) {
    // Win32 notification-area icons have only one visible text channel; expose
    // title through the tooltip rather than pretending a separate title exists.
    return tray_set_tooltip(handle, title);
  }

  bool tray_set_tooltip(tray_handle handle, std::string_view tip) {
    if (!handle)
      return false;
    std::wstring wtip = widen(tip);
    std::lock_guard<std::mutex> lock(g_win32_mu);
    HWND hwnd = g_hidden_hwnd;
    auto it = g_trays.find(handle.id);
    if (!hwnd || it == g_trays.end())
      return false;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = it->second.uid;
    nid.uFlags = NIF_TIP;
    copy_tip(nid.szTip, wtip);
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
      return false;
    it->second.tooltip = std::move(wtip);
    return true;
  }

  int tray_on(tray_handle handle, tray_event_kind kind, std::function<void()> cb) {
    if (!handle || !cb)
      return -1;
    std::lock_guard<std::mutex> lock(g_win32_mu);
    if (g_trays.find(handle.id) == g_trays.end())
      return -1;
    int token = g_tray_listener_seq.fetch_add(1);
    g_tray_listeners[token] = tray_listener_entry{handle.id, kind, std::move(cb)};
    return token;
  }

  void tray_off(tray_handle handle, int token) {
    std::lock_guard<std::mutex> lock(g_win32_mu);
    auto found = g_tray_listeners.find(token);
    if (found != g_tray_listeners.end() && found->second.tray_id == handle.id)
      g_tray_listeners.erase(found);
  }

  void tray_destroy(tray_handle handle) {
    if (!handle)
      return;
    HWND hwnd = nullptr;
    tray_entry entry;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      hwnd = g_hidden_hwnd;
      auto it = g_trays.find(handle.id);
      if (it != g_trays.end()) {
        entry = std::move(it->second);
        g_trays.erase(it);
        for (auto listener = g_tray_listeners.begin(); listener != g_tray_listeners.end();) {
          if (listener->second.tray_id == handle.id)
            listener = g_tray_listeners.erase(listener);
          else
            ++listener;
        }
        found = true;
      }
    }
    if (!found || !hwnd)
      return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = entry.uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (entry.menu)
      DestroyMenu(entry.menu);
    destroy_icon_if_owned(entry.icon, entry.owns_icon);
  }

  bool global_shortcut_register(std::string_view accelerator, std::function<void()> cb) {
    auto parsed = parse_accelerator(accelerator);
    if (!parsed || !cb)
      return false;
    HWND hwnd = ensure_hidden_window();
    if (!hwnd)
      return false;

    std::string key = normalize_accelerator(accelerator);
    global_shortcut_unregister(key);
    int id = g_hotkey_seq.fetch_add(1);
    if (!RegisterHotKey(hwnd, id, parsed->modifiers, parsed->vk))
      return false;

    hotkey_entry entry;
    entry.id = id;
    entry.accelerator = key;
    entry.callback = std::move(cb);
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      g_hotkeys_by_accelerator[key] = id;
      g_hotkeys_by_id[id] = std::move(entry);
    }
    return true;
  }

  void global_shortcut_unregister(std::string_view accelerator) {
    std::string key = normalize_accelerator(accelerator);
    HWND hwnd = nullptr;
    int id = 0;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      hwnd = g_hidden_hwnd;
      auto it = g_hotkeys_by_accelerator.find(key);
      if (it == g_hotkeys_by_accelerator.end())
        return;
      id = it->second;
      g_hotkeys_by_accelerator.erase(it);
      g_hotkeys_by_id.erase(id);
    }
    if (hwnd)
      UnregisterHotKey(hwnd, id);
  }

  void global_shortcut_unregister_all() {
    HWND hwnd = nullptr;
    std::vector<int> ids;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      hwnd = g_hidden_hwnd;
      ids.reserve(g_hotkeys_by_id.size());
      for (const auto& [id, _] : g_hotkeys_by_id)
        ids.push_back(id);
      g_hotkeys_by_id.clear();
      g_hotkeys_by_accelerator.clear();
    }
    if (hwnd) {
      for (int id : ids)
        UnregisterHotKey(hwnd, id);
    }
  }

  void win32_register_active_hwnd(HWND hwnd) {
    if (hwnd && !IsWindow(hwnd))
      return;
    int badge = 0;
    {
      std::lock_guard<std::mutex> lock(g_win32_mu);
      if (g_active_hwnd && g_active_hwnd != hwnd && IsWindow(g_active_hwnd)) {
        RemoveWindowSubclass(g_active_hwnd, active_hwnd_subclass, kSubclassId);
        SetMenu(g_active_hwnd, nullptr);
      }
      g_active_hwnd = hwnd;
      if (g_active_hwnd)
        apply_application_menu_locked();
      badge = g_pending_badge_count;
    }
    if (hwnd)
      apply_badge_to_hwnd(hwnd, badge);
  }

  HMENU win32_build_menu_for_test(const std::vector<menu_item>& items) {
    return build_hmenu(items, nullptr, nullptr, true);
  }

  bool clipboard_set_html(std::string_view utf8) {
    std::string html = cf_html_from_fragment(utf8);
    return set_clipboard_format(html_clipboard_format(), hglobal_from_string(html, true));
  }

  std::optional<std::string> clipboard_get_html() {
    auto bytes = get_clipboard_format(html_clipboard_format());
    if (!bytes)
      return std::nullopt;
    return fragment_from_cf_html(std::move(*bytes));
  }

  bool clipboard_set_rtf(std::string_view rtf) {
    return set_clipboard_format(rtf_clipboard_format(), hglobal_from_string(rtf, true));
  }

  std::optional<std::string> clipboard_get_rtf() {
    auto bytes = get_clipboard_format(rtf_clipboard_format());
    if (!bytes)
      return std::nullopt;
    while (!bytes->empty() && bytes->back() == 0)
      bytes->pop_back();
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  }

  bool clipboard_set_mime(std::string_view mime, const std::vector<u8>& bytes) {
    std::wstring wide = widen(mime);
    if (wide.empty())
      return false;
    UINT format = RegisterClipboardFormatW(wide.c_str());
    return set_clipboard_format(format, hglobal_from_bytes(bytes.data(), bytes.size(), false));
  }

  std::optional<std::vector<u8>> clipboard_get_mime(std::string_view mime) {
    std::wstring wide = widen(mime);
    if (wide.empty())
      return std::nullopt;
    return get_clipboard_format(RegisterClipboardFormatW(wide.c_str()));
  }

  void post_main_thread_dispatch(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(g_mu);
    g_q.push(std::move(fn));
  }
  void pump_main_thread_dispatches() {
    for (;;) {
      std::function<void()> fn;
      {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_q.empty())
          break;
        fn = std::move(g_q.front());
        g_q.pop();
      }
      if (fn)
        fn();
    }
  }

  // ---- NEW: single-instance handoff / deep-link / file-open helpers -------
  namespace single_instance_detail {
    std::string encode_handoff(int argc, char** argv);
    bool decode_handoff(std::string_view data, std::vector<std::string>& argv, std::string& cwd);
    void dispatch_launch(std::vector<std::string> argv, std::string cwd);
  } // namespace single_instance_detail

  namespace {
    std::atomic<bool> g_single_instance_pipe_started{false};

    std::string sanitize_single_instance_id(std::string id) {
      if (id.empty())
        id = "fxe";
      for (char& ch : id) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '.' && ch != '_' && ch != '-')
          ch = '_';
      }
      return id;
    }

    std::wstring current_module_path() {
      std::vector<wchar_t> buf(MAX_PATH);
      for (;;) {
        DWORD written = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (written == 0)
          return {};
        if (written < static_cast<DWORD>(buf.size()))
          return std::wstring(buf.data(), written);
        buf.resize(buf.size() * 2);
      }
    }

    std::string win_single_instance_id() {
      if (const char* env = std::getenv("FXE_BUNDLE_ID"); env && *env)
        return sanitize_single_instance_id(env);
      std::wstring exe = current_module_path();
      usize start = exe.find_last_of(L"\\\\/");
      start = start == std::wstring::npos ? 0 : start + 1;
      usize end = exe.find_last_of(L'.');
      if (end == std::wstring::npos || end < start)
        end = exe.size();
      return sanitize_single_instance_id(narrow(exe.substr(start, end - start).c_str()));
    }

    std::wstring win_single_instance_pipe_name() {
      return L"\\\\.\\pipe\\" + widen(win_single_instance_id());
    }

    bool write_all_pipe(HANDLE pipe, const std::string& payload) {
      const char* data = payload.data();
      DWORD left = static_cast<DWORD>(payload.size());
      while (left > 0) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, data, left, &chunk, nullptr) || chunk == 0)
          return false;
        data += chunk;
        left -= chunk;
      }
      return true;
    }

    bool forward_to_primary_pipe(const std::wstring& pipe_name, const std::string& payload) {
      for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE pipe =
            CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
          bool ok = write_all_pipe(pipe, payload);
          FlushFileBuffers(pipe);
          CloseHandle(pipe);
          return ok;
        }
        if (GetLastError() != ERROR_PIPE_BUSY)
          return false;
        if (!WaitNamedPipeW(pipe_name.c_str(), 250))
          return false;
      }
      return false;
    }

    void handle_single_instance_pipe(HANDLE pipe) {
      std::string payload;
      char buffer[4096];
      for (;;) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0)
          break;
        payload.append(buffer, read);
      }
      FlushFileBuffers(pipe);
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      std::vector<std::string> argv;
      std::string cwd;
      if (single_instance_detail::decode_handoff(payload, argv, cwd))
        single_instance_detail::dispatch_launch(std::move(argv), std::move(cwd));
    }

    void start_single_instance_pipe_listener(const std::wstring& pipe_name) {
      if (pipe_name.empty() || g_single_instance_pipe_started.exchange(true))
        return;
      std::thread([pipe_name] {
        for (;;) {
          HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_INBOUND,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                         PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
          if (pipe == INVALID_HANDLE_VALUE)
            return;
          BOOL connected =
              ConnectNamedPipe(pipe, nullptr) ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
          if (connected)
            handle_single_instance_pipe(pipe);
          else
            CloseHandle(pipe);
        }
      }).detach();
    }

    bool valid_scheme(std::string_view scheme) {
      if (scheme.empty() || !std::isalpha(static_cast<unsigned char>(scheme.front())))
        return false;
      for (char ch : scheme) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '+' && ch != '-' && ch != '.')
          return false;
      }
      return true;
    }

    bool set_reg_string(HKEY root, const std::wstring& path, const wchar_t* name,
                        const std::wstring& value) {
      HKEY key = nullptr;
      if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                          nullptr) != ERROR_SUCCESS)
        return false;
      LONG rc = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
      RegCloseKey(key);
      return rc == ERROR_SUCCESS;
    }

    bool set_open_command(const std::wstring& class_name, const std::wstring& command) {
      return set_reg_string(HKEY_CURRENT_USER,
                            L"Software\\Classes\\" + class_name + L"\\shell\\open\\command",
                            nullptr, command);
    }

    std::wstring recent_folder_path() {
      return widen(known_folder(FOLDERID_Recent));
    }

    Microsoft::WRL::ComPtr<IShellLinkW> make_recent_shell_link(const std::wstring& document) {
      Microsoft::WRL::ComPtr<IShellLinkW> link;
      std::wstring exe = current_module_path();
      if (document.empty() || exe.empty())
        return link;
      if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link))))
        return {};
      if (FAILED(link->SetPath(exe.c_str())) ||
          FAILED(link->SetArguments(quote_arg(document).c_str())) ||
          FAILED(link->SetDescription(document.c_str())))
        return {};

      Microsoft::WRL::ComPtr<IPropertyStore> store;
      if (SUCCEEDED(link.As(&store))) {
        std::wstring title = std::filesystem::path(document).filename().wstring();
        if (title.empty())
          title = document;
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<PWSTR>(title.c_str());
        (void)store->SetValue(PKEY_Title, value);
        (void)store->Commit();
      }
      return link;
    }

    void rebuild_jump_list(const std::vector<std::wstring>& documents) {
      co_scope co;
      if (!co.usable())
        return;
      Microsoft::WRL::ComPtr<ICustomDestinationList> list;
      if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&list))))
        return;
      UINT min_slots = 0;
      Microsoft::WRL::ComPtr<IObjectArray> removed;
      if (FAILED(list->BeginList(&min_slots, IID_PPV_ARGS(&removed))))
        return;

      Microsoft::WRL::ComPtr<IObjectCollection> collection;
      if (SUCCEEDED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection)))) {
        usize count = 0;
        for (const auto& document : documents) {
          if (count++ >= 10)
            break;
          auto link = make_recent_shell_link(document);
          if (link)
            (void)collection->AddObject(link.Get());
        }
        Microsoft::WRL::ComPtr<IObjectArray> array;
        if (SUCCEEDED(collection.As(&array)))
          (void)list->AppendCategory(L"Frequent", array.Get());
      }
      (void)list->CommitList();
    }

    std::vector<std::wstring> win_recent_documents_wide() {
      std::vector<std::wstring> out;
      co_scope co;
      if (!co.usable())
        return out;
      std::wstring recent = recent_folder_path();
      if (recent.empty())
        return out;
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(recent, ec)) {
        if (ec)
          break;
        if (!entry.is_regular_file(ec))
          continue;
        std::filesystem::path p = entry.path();
        std::wstring ext = p.extension().wstring();
        if (ext != L".lnk" && ext != L".LNK")
          continue;

        Microsoft::WRL::ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&link))))
          continue;
        Microsoft::WRL::ComPtr<IPersistFile> persist;
        if (FAILED(link.As(&persist)) || FAILED(persist->Load(p.c_str(), STGM_READ)))
          continue;
        std::vector<wchar_t> target(MAX_PATH);
        WIN32_FIND_DATAW data{};
        if (SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), &data,
                                    SLGP_UNCPRIORITY)) &&
            target[0] != L'\0') {
          out.emplace_back(target.data());
        }
      }
      return out;
    }
  } // namespace

  namespace app {
    bool add_recent_document(std::string_view path) {
      std::wstring document = widen(path);
      if (document.empty())
        return false;
      co_scope co;
      if (!co.usable())
        return false;
      SHAddToRecentDocs(SHARD_PATHW, document.c_str());

      std::vector<std::wstring> documents;
      documents.push_back(document);
      for (const auto& existing : win_recent_documents_wide()) {
        if (existing != document)
          documents.push_back(existing);
      }
      rebuild_jump_list(documents);
      return true;
    }

    std::vector<std::string> recent_documents() {
      std::vector<std::string> out;
      for (const auto& document : win_recent_documents_wide())
        out.push_back(narrow(document.c_str()));
      return out;
    }

    bool clear_recent_documents() {
      co_scope co;
      if (!co.usable())
        return false;
      SHAddToRecentDocs(SHARD_PIDL, nullptr);
      rebuild_jump_list({});
      return true;
    }
  } // namespace app

  bool single_instance_platform_acquire_or_forward(int argc, char** argv) {
    std::string id = win_single_instance_id();
    std::wstring pipe_name = win_single_instance_pipe_name();
    if (request_single_instance_lock(id)) {
      start_single_instance_pipe_listener(pipe_name);
      return true;
    }
    (void)forward_to_primary_pipe(pipe_name, single_instance_detail::encode_handoff(argc, argv));
    return false;
  }

  void single_instance_platform_install_open_handlers() {}

  bool single_instance_platform_set_default_protocol_client(const std::string& scheme) {
    if (!valid_scheme(scheme))
      return false;
    std::wstring wscheme = widen(scheme);
    std::wstring exe = current_module_path();
    if (wscheme.empty() || exe.empty())
      return false;
    std::wstring base = L"Software\\Classes\\" + wscheme;
    std::wstring command = quote_arg(exe) + L" \"%1\"";
    return set_reg_string(HKEY_CURRENT_USER, base, nullptr, L"URL:" + wscheme) &&
           set_reg_string(HKEY_CURRENT_USER, base, L"URL Protocol", L"") &&
           set_open_command(wscheme, command);
  }

  bool single_instance_platform_set_default_file_handler(const std::string& ext) {
    std::string clean = ext;
    if (!clean.empty() && clean.front() == '.')
      clean.erase(clean.begin());
    if (clean.empty())
      return false;
    for (char ch : clean) {
      unsigned char c = static_cast<unsigned char>(ch);
      if (!std::isalnum(c) && ch != '_' && ch != '-' && ch != '.')
        return false;
    }
    std::wstring dot_ext = L"." + widen(clean);
    std::wstring class_name = L"fxe." + widen(clean);
    std::wstring exe = current_module_path();
    if (dot_ext == L"." || exe.empty())
      return false;
    std::wstring command = quote_arg(exe) + L" \"%1\"";
    return set_reg_string(HKEY_CURRENT_USER, L"Software\\Classes\\" + dot_ext, nullptr,
                          class_name) &&
           set_open_command(class_name, command);
  }
} // namespace fxe::os

void __fxe_os_register_active_hwnd(HWND hwnd) {
  fxe::os::win32_register_active_hwnd(hwnd);
}

HMENU __fxe_os_win32_build_menu_for_test(const std::vector<fxe::os::menu_item>& items) {
  return fxe::os::win32_build_menu_for_test(items);
}

#else
} // namespace fxe::os
#endif // _WIN32
