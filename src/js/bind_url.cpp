// JS bindings for the global URL and URLSearchParams classes.
//
// Implements a focused subset of WHATWG URL — enough for fetch/WebSocket
// callers to read+mutate `protocol`, `host`, `hostname`, `port`, `pathname`,
// `search`, `hash`, `href`, `origin`, plus a real URLSearchParams with
// get/set/append/delete/has/forEach/toString. We deliberately do not pull in
// a giant URL library; the rules implemented match the *common* path. Edge
// cases (IDN, percent-encoded host, file:// quirks) are out of scope for v0.

#include "bind_url.hpp"
#include "weak_holder.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <fxe/js_bindings.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr u32 TAG_URL = 0x55524C5Fu;       // 'URL_'
    constexpr u32 TAG_URLSEARCH = 0x55534552u; // 'USER' (URL Search)

    using TplGlobal = Global<FunctionTemplate>;
    std::unordered_map<Isolate*, TplGlobal>& url_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& usp_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    void url_reset_for_isolate(Isolate* iso) {
      auto& t = url_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    void usp_reset_for_isolate(Isolate* iso) {
      auto& t = usp_tpl_table();
      auto it = t.find(iso);
      if (it != t.end()) {
        it->second.Reset();
        t.erase(it);
      }
    }
    struct url_resetter_register {
      url_resetter_register() {
        register_template_resetter(&url_reset_for_isolate);
        register_template_resetter(&usp_reset_for_isolate);
      }
    };
    static url_resetter_register s_url_resetter_register;

    // ---------------- URL parsing -------------------------------------------

    struct url_data {
      std::string protocol; // includes trailing ':' e.g. "https:"
      std::string username;
      std::string password;
      std::string hostname;
      std::string port;        // empty when default
      std::string pathname;    // includes leading '/'
      std::string search;      // empty or starts with '?'
      std::string hash;        // empty or starts with '#'
      bool is_special = false; // http/https/ws/wss/ftp/file
      bool has_authority = false;
    };

    bool is_special_scheme(const std::string& s) {
      return s == "http:" || s == "https:" || s == "ws:" || s == "wss:" || s == "ftp:" ||
             s == "file:";
    }

    int default_port_for(const std::string& s) {
      if (s == "http:" || s == "ws:")
        return 80;
      if (s == "https:" || s == "wss:")
        return 443;
      if (s == "ftp:")
        return 21;
      return -1;
    }

    std::string ascii_lower(std::string s) {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    bool parse_url(const std::string& input, const url_data* base, url_data& out) {
      // Strip leading/trailing tabs/newlines/spaces (basic).
      usize start = 0, end = input.size();
      while (start < end && (input[start] == ' ' || input[start] == '\t' || input[start] == '\n' ||
                             input[start] == '\r'))
        ++start;
      while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\t' ||
                             input[end - 1] == '\n' || input[end - 1] == '\r'))
        --end;
      std::string s = input.substr(start, end - start);

      // Detect scheme: first ':' before any '/', '?', '#'.
      usize colon = std::string::npos;
      for (usize i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '/' || c == '?' || c == '#')
          break;
        if (c == ':' && i > 0) {
          colon = i;
          break;
        }
      }
      bool has_scheme = false;
      std::string rest = s;
      if (colon != std::string::npos) {
        // valid scheme: ALPHA ( ALPHA / DIGIT / + / - / . )*
        bool ok = std::isalpha(static_cast<unsigned char>(s[0]));
        for (usize i = 1; ok && i < colon; ++i) {
          char c = s[i];
          ok = std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
        }
        if (ok) {
          out.protocol = ascii_lower(s.substr(0, colon + 1));
          rest = s.substr(colon + 1);
          has_scheme = true;
        }
      }

      if (!has_scheme) {
        if (!base)
          return false;
        out = *base;
        out.search.clear();
        out.hash.clear();
        // Resolve relative reference against base. Minimal handling:
        if (rest.rfind("//", 0) == 0) {
          // protocol-relative
          rest = out.protocol + rest;
          return parse_url(rest, nullptr, out);
        }
        // path/query/fragment
        usize hp = rest.find('#');
        std::string before_hash = hp == std::string::npos ? rest : rest.substr(0, hp);
        if (hp != std::string::npos)
          out.hash = rest.substr(hp);
        usize qp = before_hash.find('?');
        std::string path_part = qp == std::string::npos ? before_hash : before_hash.substr(0, qp);
        if (qp != std::string::npos)
          out.search = before_hash.substr(qp);
        if (!path_part.empty()) {
          if (path_part.front() == '/') {
            out.pathname = path_part;
          } else {
            // Replace last segment of base pathname.
            usize slash = out.pathname.find_last_of('/');
            std::string dir = slash == std::string::npos ? "/" : out.pathname.substr(0, slash + 1);
            if (dir.empty())
              dir = "/";
            out.pathname = dir + path_part;
          }
        }
        return true;
      }

      out.is_special = is_special_scheme(out.protocol);

      // Authority?
      if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
        out.has_authority = true;
        rest = rest.substr(2);
        usize end_auth = rest.size();
        for (usize i = 0; i < rest.size(); ++i) {
          char c = rest[i];
          if (c == '/' || c == '?' || c == '#') {
            end_auth = i;
            break;
          }
        }
        std::string authority = rest.substr(0, end_auth);
        rest = rest.substr(end_auth);

        // Userinfo
        usize at = authority.rfind('@');
        if (at != std::string::npos) {
          std::string ui = authority.substr(0, at);
          authority = authority.substr(at + 1);
          usize cn = ui.find(':');
          if (cn == std::string::npos) {
            out.username = ui;
          } else {
            out.username = ui.substr(0, cn);
            out.password = ui.substr(cn + 1);
          }
        }
        // Port
        usize cn = std::string::npos;
        // Be careful: ipv6 literals would have '[' brackets; skip until ']'.
        if (!authority.empty() && authority.front() == '[') {
          usize rb = authority.find(']');
          if (rb == std::string::npos)
            return false;
          if (rb + 1 < authority.size() && authority[rb + 1] == ':')
            cn = rb + 1;
          out.hostname = authority.substr(0, rb + 1);
          if (cn != std::string::npos)
            out.port = authority.substr(cn + 1);
        } else {
          cn = authority.find(':');
          if (cn == std::string::npos) {
            out.hostname = authority;
          } else {
            out.hostname = authority.substr(0, cn);
            out.port = authority.substr(cn + 1);
          }
          out.hostname = ascii_lower(out.hostname);
        }
        // Drop default ports.
        if (!out.port.empty()) {
          int dp = default_port_for(out.protocol);
          int p = 0;
          for (char c : out.port) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
              p = -2;
              break;
            }
            p = p * 10 + (c - '0');
          }
          if (p == dp)
            out.port.clear();
        }
      } else if (out.is_special) {
        // Special schemes require authority.
        return false;
      }

      // Path / search / hash from `rest`.
      usize hp = rest.find('#');
      std::string head = hp == std::string::npos ? rest : rest.substr(0, hp);
      if (hp != std::string::npos)
        out.hash = rest.substr(hp);
      usize qp = head.find('?');
      std::string path = qp == std::string::npos ? head : head.substr(0, qp);
      if (qp != std::string::npos)
        out.search = head.substr(qp);
      if (out.is_special && (path.empty() || path.front() != '/'))
        path = "/" + path;
      out.pathname = path;
      return true;
    }

    std::string serialize_url(const url_data& u) {
      std::string out;
      out += u.protocol;
      if (u.has_authority || u.is_special) {
        out += "//";
        if (!u.username.empty() || !u.password.empty()) {
          out += u.username;
          if (!u.password.empty()) {
            out += ":";
            out += u.password;
          }
          out += "@";
        }
        out += u.hostname;
        if (!u.port.empty()) {
          out += ":";
          out += u.port;
        }
      }
      out += u.pathname;
      out += u.search;
      out += u.hash;
      return out;
    }

    std::string serialize_origin(const url_data& u) {
      if (u.protocol == "http:" || u.protocol == "https:" || u.protocol == "ws:" ||
          u.protocol == "wss:" || u.protocol == "ftp:") {
        std::string s = u.protocol + "//" + u.hostname;
        if (!u.port.empty()) {
          s += ":";
          s += u.port;
        }
        return s;
      }
      return "null";
    }

    // ---------------- URLSearchParams ---------------------------------------

    struct usp_data {
      std::vector<std::pair<std::string, std::string>> pairs;
    };

    int from_hex(char c) {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    }

    std::string percent_decode_form(const std::string& s) {
      std::string out;
      out.reserve(s.size());
      for (usize i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
          out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size()) {
          int hi = from_hex(s[i + 1]);
          int lo = from_hex(s[i + 2]);
          if (hi >= 0 && lo >= 0) {
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
          } else {
            out.push_back(c);
          }
        } else {
          out.push_back(c);
        }
      }
      return out;
    }

    std::string percent_encode_form(const std::string& s) {
      static const char* hex = "0123456789ABCDEF";
      std::string out;
      out.reserve(s.size());
      for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == ' ') {
          out.push_back('+');
        } else if (std::isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') {
          out.push_back(ch);
        } else {
          out.push_back('%');
          out.push_back(hex[(c >> 4) & 0xF]);
          out.push_back(hex[c & 0xF]);
        }
      }
      return out;
    }

    void usp_parse(const std::string& q, usp_data& out) {
      out.pairs.clear();
      if (q.empty())
        return;
      usize i = 0;
      while (i <= q.size()) {
        usize amp = q.find('&', i);
        std::string seg = amp == std::string::npos ? q.substr(i) : q.substr(i, amp - i);
        if (!seg.empty()) {
          usize eq = seg.find('=');
          std::string k = eq == std::string::npos ? seg : seg.substr(0, eq);
          std::string v = eq == std::string::npos ? "" : seg.substr(eq + 1);
          out.pairs.emplace_back(percent_decode_form(k), percent_decode_form(v));
        }
        if (amp == std::string::npos)
          break;
        i = amp + 1;
      }
    }

    std::string usp_serialize(const usp_data& d) {
      std::string out;
      for (usize i = 0; i < d.pairs.size(); ++i) {
        if (i)
          out.push_back('&');
        out += percent_encode_form(d.pairs[i].first);
        out.push_back('=');
        out += percent_encode_form(d.pairs[i].second);
      }
      return out;
    }

    Local<String> s8(Isolate* iso, const std::string& s) {
      return String::NewFromUtf8(iso, s.c_str(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    std::string to_str(Isolate* iso, Local<Value> v) {
      auto ctx = iso->GetCurrentContext();
      Local<String> s;
      if (!v->ToString(ctx).ToLocal(&s))
        return {};
      String::Utf8Value u(iso, s);
      return std::string(*u ? *u : "", *u ? u.length() : 0);
    }

    void throw_type(Isolate* iso, const char* msg) {
      (void)throw_type_error(iso, msg);
    }

    // ---------------- url_data <-> object plumbing --------------------------

    struct url_holder : weak_holder<url_holder> {
      std::unique_ptr<url_data> data;
    };
    struct usp_holder : weak_holder<usp_holder> {
      std::unique_ptr<usp_data> data;
      // Optional weak reference to a parent URL holder so writes to the
      // search params propagate back to the URL's `search`.
      url_holder* parent = nullptr;
    };

    Local<Object> wrap_usp(Isolate* iso, Local<Context> ctx, std::unique_ptr<usp_data> d) {
      auto tpl = usp_tpl_table()[iso].Get(iso);
      Local<Object> obj = tpl->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      auto* h = new usp_holder{{}, std::move(d)};
      set_native(iso, obj, h->data.get(), TAG_URLSEARCH);
      h->bind(iso, obj);
      return obj;
    }

    url_data* unwrap_url(Local<Object> o) {
      return static_cast<url_data*>(unwrap(o, TAG_URL));
    }
    usp_data* unwrap_usp(Local<Object> o) {
      return static_cast<usp_data*>(unwrap(o, TAG_URLSEARCH));
    }

    // ---------------- URL ctor + accessors ----------------------------------

    void url_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (!info.IsConstructCall()) {
        throw_type(iso, "URL must be called with new");
        return;
      }
      if (info.Length() < 1) {
        throw_type(iso, "URL: missing url");
        return;
      }
      std::string input = to_str(iso, info[0]);
      url_data base{};
      url_data* base_ptr = nullptr;
      if (info.Length() >= 2 && !info[1]->IsUndefined() && !info[1]->IsNull()) {
        std::string base_str = to_str(iso, info[1]);
        url_data b;
        if (!parse_url(base_str, nullptr, b)) {
          throw_type(iso, "URL: invalid base");
          return;
        }
        base = b;
        base_ptr = &base;
      }
      auto d = std::make_unique<url_data>();
      if (!parse_url(input, base_ptr, *d)) {
        throw_type(iso, "URL: invalid url");
        return;
      }
      // Replace `this`'s instance bookkeeping by re-wrapping. We installed
      // an instance template with internal field count 2; populate them
      // directly on `info.This()`.
      auto* h = new url_holder{{}, std::move(d)};
      auto self = info.This();
      set_native(iso, self, h->data.get(), TAG_URL);
      h->bind(iso, self);
      info.GetReturnValue().Set(self);
    }

    void url_get_href(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(s8(iso, serialize_url(*d)));
    }
    void url_set_href(Local<Name>, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      url_data next;
      if (!parse_url(to_str(iso, v), nullptr, next)) {
        throw_type(iso, "URL.href: invalid url");
        return;
      }
      *d = std::move(next);
    }

#define URL_STRING_GETTER(NAME, FIELD)                                                             \
  void url_get_##NAME(Local<Name>, const PropertyCallbackInfo<Value>& info) {                      \
    auto* iso = info.GetIsolate();                                                                 \
    auto* d = unwrap_url(info.HolderV2());                                                         \
    if (!d)                                                                                        \
      return;                                                                                      \
    info.GetReturnValue().Set(s8(iso, d->FIELD));                                                  \
  }

    URL_STRING_GETTER(protocol, protocol)
    URL_STRING_GETTER(hostname, hostname)
    URL_STRING_GETTER(port, port)
    URL_STRING_GETTER(pathname, pathname)
    URL_STRING_GETTER(search, search)
    URL_STRING_GETTER(hash, hash)
    URL_STRING_GETTER(username, username)
    URL_STRING_GETTER(password, password)
#undef URL_STRING_GETTER

    void url_get_host(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      std::string h = d->hostname;
      if (!d->port.empty()) {
        h += ":";
        h += d->port;
      }
      info.GetReturnValue().Set(s8(iso, h));
    }

    void url_get_origin(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      info.GetReturnValue().Set(s8(iso, serialize_origin(*d)));
    }

    void url_set_search(Local<Name>, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      std::string s = to_str(iso, v);
      if (s.empty())
        d->search.clear();
      else if (s.front() == '?')
        d->search = s;
      else
        d->search = "?" + s;
    }
    void url_set_hash(Local<Name>, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      std::string s = to_str(iso, v);
      if (s.empty())
        d->hash.clear();
      else if (s.front() == '#')
        d->hash = s;
      else
        d->hash = "#" + s;
    }
    void url_set_pathname(Local<Name>, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      std::string s = to_str(iso, v);
      if (d->is_special && (s.empty() || s.front() != '/'))
        s = "/" + s;
      d->pathname = s;
    }
    void url_set_hostname(Local<Name>, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      d->hostname = ascii_lower(to_str(iso, v));
    }
    void url_set_port(Local<Name>, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      d->port = to_str(iso, v);
    }
    void url_set_protocol(Local<Name>, Local<Value> v, const PropertyCallbackInfo<void>& info) {
      auto* iso = info.GetIsolate();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      std::string s = ascii_lower(to_str(iso, v));
      if (!s.empty() && s.back() != ':')
        s.push_back(':');
      d->protocol = s;
      d->is_special = is_special_scheme(d->protocol);
    }

    void url_to_string(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_url(info.This());
      if (!d)
        return;
      info.GetReturnValue().Set(s8(iso, serialize_url(*d)));
    }

    void url_get_search_params(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_url(info.HolderV2());
      if (!d)
        return;
      auto usp = std::make_unique<usp_data>();
      std::string q = d->search;
      if (!q.empty() && q.front() == '?')
        q.erase(0, 1);
      usp_parse(q, *usp);
      info.GetReturnValue().Set(wrap_usp(iso, ctx, std::move(usp)));
    }

    // ---------------- URLSearchParams ---------------------------------------

    void usp_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "URLSearchParams must be called with new");
        return;
      }
      auto data = std::make_unique<usp_data>();
      if (info.Length() >= 1 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        if (info[0]->IsString()) {
          std::string s = to_str(iso, info[0]);
          if (!s.empty() && s.front() == '?')
            s.erase(0, 1);
          usp_parse(s, *data);
        } else if (info[0]->IsArray()) {
          auto a = info[0].As<Array>();
          for (u32 i = 0; i < a->Length(); ++i) {
            Local<Value> entry;
            if (!a->Get(ctx, i).ToLocal(&entry) || !entry->IsArray())
              continue;
            auto e = entry.As<Array>();
            Local<Value> k, v;
            if (e->Get(ctx, 0).ToLocal(&k) && e->Get(ctx, 1).ToLocal(&v))
              data->pairs.emplace_back(to_str(iso, k), to_str(iso, v));
          }
        } else if (info[0]->IsObject()) {
          auto o = info[0].As<Object>();
          Local<Array> names;
          if (o->GetOwnPropertyNames(ctx).ToLocal(&names)) {
            for (u32 i = 0; i < names->Length(); ++i) {
              Local<Value> k;
              if (!names->Get(ctx, i).ToLocal(&k))
                continue;
              Local<Value> v;
              if (!o->Get(ctx, k).ToLocal(&v))
                continue;
              data->pairs.emplace_back(to_str(iso, k), to_str(iso, v));
            }
          }
        }
      }
      auto* h = new usp_holder{{}, std::move(data), nullptr};
      auto self = info.This();
      set_native(iso, self, h->data.get(), TAG_URLSEARCH);
      h->bind(iso, self);
      info.GetReturnValue().Set(self);
    }

    void usp_get(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_usp(info.This());
      if (!d || info.Length() < 1)
        return;
      std::string k = to_str(iso, info[0]);
      for (auto& [pk, pv] : d->pairs)
        if (pk == k) {
          info.GetReturnValue().Set(s8(iso, pv));
          return;
        }
      info.GetReturnValue().SetNull();
    }
    void usp_get_all(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_usp(info.This());
      if (!d || info.Length() < 1)
        return;
      std::string k = to_str(iso, info[0]);
      Local<Array> out = Array::New(iso);
      u32 n = 0;
      for (auto& [pk, pv] : d->pairs)
        if (pk == k)
          out->Set(ctx, n++, s8(iso, pv)).Check();
      info.GetReturnValue().Set(out);
    }
    void usp_has(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_usp(info.This());
      if (!d || info.Length() < 1)
        return;
      std::string k = to_str(iso, info[0]);
      for (auto& [pk, pv] : d->pairs)
        if (pk == k) {
          info.GetReturnValue().Set(true);
          return;
        }
      info.GetReturnValue().Set(false);
    }
    void usp_set(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_usp(info.This());
      if (!d || info.Length() < 2)
        return;
      std::string k = to_str(iso, info[0]);
      std::string v = to_str(iso, info[1]);
      bool replaced = false;
      auto it = d->pairs.begin();
      while (it != d->pairs.end()) {
        if (it->first == k) {
          if (!replaced) {
            it->second = v;
            replaced = true;
            ++it;
          } else {
            it = d->pairs.erase(it);
          }
        } else
          ++it;
      }
      if (!replaced)
        d->pairs.emplace_back(std::move(k), std::move(v));
    }
    void usp_append(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_usp(info.This());
      if (!d || info.Length() < 2)
        return;
      d->pairs.emplace_back(to_str(iso, info[0]), to_str(iso, info[1]));
    }
    void usp_delete(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_usp(info.This());
      if (!d || info.Length() < 1)
        return;
      std::string k = to_str(iso, info[0]);
      auto it = d->pairs.begin();
      while (it != d->pairs.end()) {
        if (it->first == k)
          it = d->pairs.erase(it);
        else
          ++it;
      }
    }
    void usp_to_string(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto* d = unwrap_usp(info.This());
      if (!d)
        return;
      info.GetReturnValue().Set(s8(iso, usp_serialize(*d)));
    }
    void usp_for_each(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* d = unwrap_usp(info.This());
      if (!d || info.Length() < 1 || !info[0]->IsFunction())
        return;
      auto fn = info[0].As<Function>();
      Local<Object> self = info.This();
      for (auto& [pk, pv] : d->pairs) {
        Local<Value> argv[3] = {s8(iso, pv), s8(iso, pk), self};
        Local<Value> ignored;
        (void)fn->Call(ctx, Undefined(iso), 3, argv).ToLocal(&ignored);
      }
    }

  } // namespace

  void install_url_globals(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);

    // URL
    auto utpl = FunctionTemplate::New(iso, url_ctor);
    utpl->SetClassName("URL"_v8(iso));
    utpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto uproto = utpl->PrototypeTemplate();
    auto uinst = utpl->InstanceTemplate();

    uinst->SetNativeDataProperty("href"_v8(iso), url_get_href, url_set_href);
    uinst->SetNativeDataProperty("protocol"_v8(iso), url_get_protocol, url_set_protocol);
    uinst->SetNativeDataProperty("host"_v8(iso), url_get_host);
    uinst->SetNativeDataProperty("hostname"_v8(iso), url_get_hostname, url_set_hostname);
    uinst->SetNativeDataProperty("port"_v8(iso), url_get_port, url_set_port);
    uinst->SetNativeDataProperty("pathname"_v8(iso), url_get_pathname, url_set_pathname);
    uinst->SetNativeDataProperty("search"_v8(iso), url_get_search, url_set_search);
    uinst->SetNativeDataProperty("hash"_v8(iso), url_get_hash, url_set_hash);
    uinst->SetNativeDataProperty("origin"_v8(iso), url_get_origin);
    uinst->SetNativeDataProperty("username"_v8(iso), url_get_username);
    uinst->SetNativeDataProperty("password"_v8(iso), url_get_password);
    uinst->SetNativeDataProperty("searchParams"_v8(iso), url_get_search_params, nullptr);
    uproto->Set(iso, "toString", FunctionTemplate::New(iso, url_to_string));
    uproto->Set(iso, "toJSON", FunctionTemplate::New(iso, url_to_string));

    global->Set(iso, "URL", utpl);
    url_tpl_table()[iso].Reset(iso, utpl);

    // URLSearchParams
    auto stpl = FunctionTemplate::New(iso, usp_ctor);
    stpl->SetClassName("URLSearchParams"_v8(iso));
    stpl->InstanceTemplate()->SetInternalFieldCount(2);
    auto sproto = stpl->PrototypeTemplate();
    sproto->Set(iso, "get", FunctionTemplate::New(iso, usp_get));
    sproto->Set(iso, "getAll", FunctionTemplate::New(iso, usp_get_all));
    sproto->Set(iso, "has", FunctionTemplate::New(iso, usp_has));
    sproto->Set(iso, "set", FunctionTemplate::New(iso, usp_set));
    sproto->Set(iso, "append", FunctionTemplate::New(iso, usp_append));
    sproto->Set(iso, "delete", FunctionTemplate::New(iso, usp_delete));
    sproto->Set(iso, "toString", FunctionTemplate::New(iso, usp_to_string));
    sproto->Set(iso, "forEach", FunctionTemplate::New(iso, usp_for_each));

    global->Set(iso, "URLSearchParams", stpl);
    usp_tpl_table()[iso].Reset(iso, stpl);
  }

} // namespace fxe::js
