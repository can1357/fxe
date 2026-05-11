// v0 Win32 IME bridge for GLFW windows.
// Limitations: keystroke routing through TSF is still deferred, so both the TSF
// and IMM32 paths only forward composition/result text through the existing IME
// event callback.

#include "../os/os.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <imm.h>
#include <msctf.h>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace fxe::os {
  namespace {
    constexpr DWORD kInvalidSinkCookie = static_cast<DWORD>(TF_INVALID_COOKIE);

    struct win32_ime_bridge {
      void* owner = nullptr;
      win32_ime_emit_fn emit = nullptr;
      WNDPROC original_wndproc = nullptr;
      bool tsf_enabled = false;
      bool com_initialized = false;
      TfClientId tsf_client_id = TF_CLIENTID_NULL;
      DWORD tsf_text_sink_cookie = kInvalidSinkCookie;
      IUnknown* tsf_thread_mgr = nullptr;
      IUnknown* tsf_doc_mgr = nullptr;
      IUnknown* tsf_context = nullptr;
      IUnknown* tsf_context_source = nullptr;
      IUnknown* tsf_sink = nullptr;
    };

    std::mutex& bridge_mutex() {
      static std::mutex mu;
      return mu;
    }

    std::unordered_map<HWND, win32_ime_bridge>& bridge_map() {
      static std::unordered_map<HWND, win32_ime_bridge> map;
      return map;
    }

    void release_unknown(IUnknown*& value) {
      if (!value)
        return;
      value->Release();
      value = nullptr;
    }

    std::string utf8_from_utf16(const wchar_t* value, int length) {
      if (!value || length <= 0)
        return {};
      const int needed =
          WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
      if (needed <= 0)
        return {};
      std::string out(static_cast<usize>(needed), '\0');
      if (WideCharToMultiByte(CP_UTF8, 0, value, length, out.data(), needed, nullptr, nullptr) <= 0)
        return {};
      return out;
    }

    std::string read_composition_utf8(HIMC himc, DWORD which) {
      if (!himc)
        return {};
      const LONG bytes = ImmGetCompositionStringW(himc, which, nullptr, 0);
      if (bytes <= 0)
        return {};
      std::vector<wchar_t> buffer(static_cast<usize>(bytes) / sizeof(wchar_t));
      const LONG copied = ImmGetCompositionStringW(himc, which, buffer.data(), bytes);
      if (copied <= 0)
        return {};
      return utf8_from_utf16(buffer.data(), static_cast<int>(copied / sizeof(wchar_t)));
    }

    int read_cursor_pos(HIMC himc) {
      if (!himc)
        return 0;
      const LONG cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
      return cursor < 0 ? 0 : static_cast<int>(cursor);
    }

    int range_text_length(ITfRange* range, TfEditCookie read_cookie) {
      if (!range)
        return 0;
      wchar_t buffer[64];
      ULONG copied = 0;
      int total = 0;
      do {
        copied = 0;
        if (FAILED(range->GetText(read_cookie, 0, buffer, 64, &copied)))
          return total;
        total += static_cast<int>(copied);
      } while (copied != 0);
      return total;
    }

    std::string read_context_utf8(ITfContext* context, TfEditCookie read_cookie) {
      if (!context)
        return {};
      ITfRange* start = nullptr;
      ITfRange* end = nullptr;
      ITfRange* span = nullptr;
      std::string out;
      if (FAILED(context->GetStart(read_cookie, &start)) || !start)
        return {};
      if (FAILED(context->GetEnd(read_cookie, &end)) || !end) {
        start->Release();
        return {};
      }
      if (SUCCEEDED(start->Clone(&span)) && span) {
        if (SUCCEEDED(span->ShiftEndToRange(read_cookie, end, TF_ANCHOR_END))) {
          std::vector<wchar_t> buffer;
          buffer.reserve(64);
          for (;;) {
            wchar_t chunk[64];
            ULONG copied = 0;
            if (FAILED(span->GetText(read_cookie, 0, chunk, 64, &copied)) || copied == 0)
              break;
            buffer.insert(buffer.end(), chunk, chunk + copied);
          }
          if (!buffer.empty())
            out = utf8_from_utf16(buffer.data(), static_cast<int>(buffer.size()));
        }
      }
      if (span)
        span->Release();
      end->Release();
      start->Release();
      return out;
    }

    int read_context_cursor(ITfContext* context, TfEditCookie read_cookie) {
      if (!context)
        return 0;
      ITfRange* start = nullptr;
      if (FAILED(context->GetStart(read_cookie, &start)) || !start)
        return 0;
      TF_SELECTION selection{};
      ULONG fetched = 0;
      int cursor = 0;
      if (SUCCEEDED(
              context->GetSelection(read_cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) &&
          fetched != 0 && selection.range) {
        ITfRange* prefix = nullptr;
        if (SUCCEEDED(start->Clone(&prefix)) && prefix) {
          if (SUCCEEDED(prefix->ShiftEndToRange(read_cookie, selection.range, TF_ANCHOR_START)))
            cursor = range_text_length(prefix, read_cookie);
          prefix->Release();
        }
        selection.range->Release();
      }
      start->Release();
      return cursor;
    }

    struct tsf_text_sink final : ITfTextEditSink, ITfKeyEventSink {
      explicit tsf_text_sink(win32_ime_bridge* bridge_in) : bridge(bridge_in) {}

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
          return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfTextEditSink) {
          *out = static_cast<ITfTextEditSink*>(this);
        } else if (iid == IID_ITfKeyEventSink) {
          *out = static_cast<ITfKeyEventSink*>(this);
        } else {
          return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return refs.fetch_add(1, std::memory_order_relaxed) + 1;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (value == 0)
          delete this;
        return value;
      }

      HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* context, TfEditCookie read_cookie,
                                          ITfEditRecord*) override {
        if (!bridge || !bridge->emit)
          return S_OK;
        std::string preedit = read_context_utf8(context, read_cookie);
        const int cursor = read_context_cursor(context, read_cookie);
        if (!preedit.empty()) {
          last_preedit = preedit;
          bridge->emit(bridge->owner, preedit.c_str(), cursor, "");
          return S_OK;
        }
        if (!last_preedit.empty()) {
          std::string committed = last_preedit;
          last_preedit.clear();
          bridge->emit(bridge->owner, "", 0, committed.c_str());
          return S_OK;
        }
        bridge->emit(bridge->owner, "", 0, "");
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL) override {
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext*, WPARAM, LPARAM, BOOL* eaten) override {
        if (eaten)
          *eaten = FALSE;
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) override {
        if (eaten)
          *eaten = FALSE;
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext*, WPARAM, LPARAM, BOOL* eaten) override {
        if (eaten)
          *eaten = FALSE;
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) override {
        if (eaten)
          *eaten = FALSE;
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) override {
        if (eaten)
          *eaten = FALSE;
        return S_OK;
      }

      std::atomic<ULONG> refs{1};
      win32_ime_bridge* bridge = nullptr;
      std::string last_preedit;
    };

    void teardown_tsf(win32_ime_bridge& bridge) {
      if (!bridge.tsf_enabled)
        return;
      if (bridge.tsf_context_source && bridge.tsf_text_sink_cookie != kInvalidSinkCookie) {
        auto* source = reinterpret_cast<ITfSource*>(bridge.tsf_context_source);
        (void)source->UnadviseSink(bridge.tsf_text_sink_cookie);
        bridge.tsf_text_sink_cookie = kInvalidSinkCookie;
      }
      if (bridge.tsf_thread_mgr && bridge.tsf_client_id != TF_CLIENTID_NULL) {
        ITfKeystrokeMgr* key_mgr = nullptr;
        if (SUCCEEDED(
                reinterpret_cast<ITfThreadMgr*>(bridge.tsf_thread_mgr)
                    ->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&key_mgr))) &&
            key_mgr) {
          (void)key_mgr->UnadviseKeyEventSink(bridge.tsf_client_id);
          key_mgr->Release();
        }
      }
      if (bridge.tsf_doc_mgr)
        (void)reinterpret_cast<ITfDocumentMgr*>(bridge.tsf_doc_mgr)->Pop(TF_POPF_ALL);
      if (bridge.tsf_thread_mgr)
        (void)reinterpret_cast<ITfThreadMgr*>(bridge.tsf_thread_mgr)->Deactivate();
      release_unknown(bridge.tsf_context_source);
      release_unknown(bridge.tsf_context);
      release_unknown(bridge.tsf_doc_mgr);
      release_unknown(bridge.tsf_thread_mgr);
      release_unknown(bridge.tsf_sink);
      if (bridge.com_initialized)
        CoUninitialize();
      bridge.com_initialized = false;
      bridge.tsf_client_id = TF_CLIENTID_NULL;
      bridge.tsf_enabled = false;
    }

    [[nodiscard]] bool init_tsf(HWND hwnd, win32_ime_bridge& out) {
      HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return false;
      const bool com_initialized = hr == S_OK || hr == S_FALSE;

      ITfThreadMgr* thread_mgr = nullptr;
      hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                            reinterpret_cast<void**>(&thread_mgr));
      if (FAILED(hr) || !thread_mgr) {
        if (com_initialized)
          CoUninitialize();
        return false;
      }

      TfClientId client_id = TF_CLIENTID_NULL;
      TfEditCookie edit_cookie = 0;
      ITfDocumentMgr* doc_mgr = nullptr;
      ITfContext* context = nullptr;
      ITfSource* source = nullptr;
      ITfKeystrokeMgr* key_mgr = nullptr;
      DWORD text_cookie = kInvalidSinkCookie;
      tsf_text_sink* sink = new (std::nothrow) tsf_text_sink(&out);
      if (!sink)
        goto fail;

      hr = thread_mgr->Activate(&client_id);
      if (FAILED(hr) || client_id == TF_CLIENTID_NULL)
        goto fail;
      hr = thread_mgr->CreateDocumentMgr(&doc_mgr);
      if (FAILED(hr) || !doc_mgr)
        goto fail;
      hr = doc_mgr->CreateContext(client_id, 0, static_cast<ITfTextEditSink*>(sink), &context,
                                  &edit_cookie);
      if (FAILED(hr) || !context)
        goto fail;
      hr = doc_mgr->Push(context);
      if (FAILED(hr))
        goto fail;
      hr = context->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
      if (FAILED(hr) || !source)
        goto fail;
      hr = source->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(sink),
                              &text_cookie);
      if (FAILED(hr))
        goto fail;
      hr = thread_mgr->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&key_mgr));
      if (SUCCEEDED(hr) && key_mgr) {
        (void)key_mgr->AdviseKeyEventSink(client_id, static_cast<ITfKeyEventSink*>(sink), TRUE);
        key_mgr->Release();
      }
      (void)thread_mgr->SetFocus(doc_mgr);

      out.tsf_enabled = true;
      out.com_initialized = com_initialized;
      out.tsf_client_id = client_id;
      out.tsf_text_sink_cookie = text_cookie;
      out.tsf_thread_mgr = thread_mgr;
      out.tsf_doc_mgr = doc_mgr;
      out.tsf_context = context;
      out.tsf_context_source = source;
      out.tsf_sink = sink;
      return true;

    fail:
      if (source)
        source->Release();
      if (context)
        context->Release();
      if (doc_mgr)
        doc_mgr->Release();
      if (thread_mgr) {
        if (client_id != TF_CLIENTID_NULL)
          (void)thread_mgr->Deactivate();
        thread_mgr->Release();
      }
      if (sink)
        sink->Release();
      if (com_initialized)
        CoUninitialize();
      return false;
    }

    void destroy_bridge(HWND hwnd, win32_ime_bridge& bridge) {
      teardown_tsf(bridge);
      if (bridge.original_wndproc)
        (void)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                reinterpret_cast<LONG_PTR>(bridge.original_wndproc));
    }

    LRESULT CALLBACK ime_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
      win32_ime_bridge bridge;
      {
        std::lock_guard<std::mutex> lock(bridge_mutex());
        auto it = bridge_map().find(hwnd);
        if (it != bridge_map().end())
          bridge = it->second;
      }
      if (!bridge.original_wndproc)
        return DefWindowProcW(hwnd, msg, wp, lp);

      switch (msg) {
      case WM_IME_STARTCOMPOSITION:
        if (!bridge.tsf_enabled && bridge.emit)
          bridge.emit(bridge.owner, "", 0, "");
        break;
      case WM_IME_COMPOSITION: {
        if (bridge.tsf_enabled)
          break;
        HIMC himc = ImmGetContext(hwnd);
        if (himc) {
          if ((lp & GCS_COMPSTR) != 0 && bridge.emit) {
            std::string preedit = read_composition_utf8(himc, GCS_COMPSTR);
            bridge.emit(bridge.owner, preedit.c_str(), read_cursor_pos(himc), "");
          }
          if ((lp & GCS_RESULTSTR) != 0 && bridge.emit) {
            std::string committed = read_composition_utf8(himc, GCS_RESULTSTR);
            bridge.emit(bridge.owner, "", 0, committed.c_str());
          }
          ImmReleaseContext(hwnd, himc);
        }
        break;
      }
      case WM_IME_ENDCOMPOSITION:
        if (!bridge.tsf_enabled && bridge.emit)
          bridge.emit(bridge.owner, "", 0, "");
        break;
      case WM_NCDESTROY: {
        {
          std::lock_guard<std::mutex> lock(bridge_mutex());
          auto it = bridge_map().find(hwnd);
          if (it != bridge_map().end()) {
            destroy_bridge(hwnd, it->second);
            bridge_map().erase(it);
          }
        }
        return CallWindowProcW(bridge.original_wndproc, hwnd, msg, wp, lp);
      }
      default:
        break;
      }

      return CallWindowProcW(bridge.original_wndproc, hwnd, msg, wp, lp);
    }
  } // namespace

  void install_win32_ime_bridge(void* hwnd_void, void* owner, win32_ime_emit_fn emit) {
    HWND hwnd = static_cast<HWND>(hwnd_void);
    if (!hwnd || !emit)
      return;

    std::lock_guard<std::mutex> lock(bridge_mutex());
    auto& bridges = bridge_map();
    auto it = bridges.find(hwnd);
    if (it != bridges.end()) {
      it->second.owner = owner;
      it->second.emit = emit;
      return;
    }

    SetLastError(0);
    LONG_PTR previous =
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ime_wndproc));
    if (previous == 0 && GetLastError() != 0)
      return;

    // Insert first, then initialise TSF against the map slot so the TF text sink's `bridge`
    // pointer references the long-lived bridge owned by `bridges`, not a dead stack frame.
    auto [it, inserted] = bridges.try_emplace(hwnd);
    if (!inserted)
      return;
    auto& bridge = it->second;
    bridge.owner = owner;
    bridge.emit = emit;
    bridge.original_wndproc = previous != 0 ? reinterpret_cast<WNDPROC>(previous) : DefWindowProcW;
    (void)init_tsf(hwnd, bridge);
  }
} // namespace fxe::os
#endif
