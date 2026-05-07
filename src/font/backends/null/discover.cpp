// Always-available no-op discoverer. Used by the bare-FreeType backend and
// as a fallback when no discovery library is wired in.

#include <fxe/font/discover.hpp>

namespace fxe::font {
  namespace {
    class NoneDiscover final : public Discover {
    public:
      [[nodiscard]] std::vector<Descriptor> find(const Descriptor&) override {
        return {};
      }
    };
  } // namespace

  std::unique_ptr<Discover> make_none_discover() {
    return std::make_unique<NoneDiscover>();
  }

} // namespace fxe::font
