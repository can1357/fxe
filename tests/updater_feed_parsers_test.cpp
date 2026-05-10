#include "runtime/updater.hpp"

#include <cstdio>
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

  void test_sparkle_happy_path() {
    const std::string xml = "<?xml version=\"1.0\"?>"
                            "<rss><channel><item>"
                            "<title>Release</title>"
                            "<sparkle:version>1.2.3</sparkle:version>"
                            "<sparkle:shortVersionString>1.2.3</sparkle:shortVersionString>"
                            "<sparkle:channel>beta</sparkle:channel>"
                            "<enclosure url=\"https://example.com/x.zip\" length=\"12345\" "
                            "sparkle:installerSha256="
                            "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"/>"
                            "</item></channel></rss>";
    std::string error;
    auto manifest = fxe::runtime::parse_appcast_xml(xml, error);
    CHECK(manifest.has_value());
    CHECK(error.empty());
    if (manifest) {
      CHECK(manifest->version == "1.2.3");
      CHECK(manifest->channel == fxe::runtime::update_channel::beta);
      CHECK(manifest->artifacts.size() == 1);
      if (manifest->artifacts.size() == 1) {
        CHECK(manifest->artifacts[0].kind == "full");
        CHECK(manifest->artifacts[0].url == "https://example.com/x.zip");
        CHECK(manifest->artifacts[0].sha256 ==
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        CHECK(manifest->artifacts[0].size == 12345);
      }
    }
  }

  void test_sparkle_malformed() {
    const std::string xml = "<rss><item/></rss>";
    std::string error;
    auto manifest = fxe::runtime::parse_appcast_xml(xml, error);
    CHECK(!manifest.has_value());
    CHECK(!error.empty());
  }

  void test_sparkle_missing_sha256() {
    const std::string xml = "<rss><channel><item><sparkle:version>1.2.3</sparkle:version>"
                            "<enclosure url=\"https://example.com/x.zip\" length=\"123\"/>"
                            "</item></channel></rss>";
    std::string error;
    auto manifest = fxe::runtime::parse_appcast_xml(xml, error);
    CHECK(!manifest.has_value());
    CHECK(error.find("sha256") != std::string::npos);
  }

  void test_sparkle_doctype_rejected() {
    const std::string xml = "<!DOCTYPE rss [<!ENTITY xxe SYSTEM \"file:///etc/passwd\">]>"
                            "<rss><channel><item><sparkle:version>1.0.0</sparkle:version>"
                            "<enclosure url=\"https://example.com/x.zip\" length=\"1\" "
                            "sparkle:installerSha256="
                            "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"/>"
                            "</item></channel></rss>";
    std::string error;
    auto manifest = fxe::runtime::parse_appcast_xml(xml, error);
    CHECK(!manifest.has_value());
    CHECK(!error.empty());
  }

  void test_sparkle_entity_decode() {
    const std::string xml =
        "<rss><channel><item><title>A &amp; B</title><sparkle:version>1.0.0</sparkle:version>"
        "<enclosure url=\"https://example.com/x.zip\" length=\"9\" "
        "sparkle:installerSha256="
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"/>"
        "</item></channel></rss>";
    std::string error;
    auto manifest = fxe::runtime::parse_appcast_xml(xml, error);
    CHECK(manifest.has_value());
    CHECK(error.empty());
    if (manifest)
      CHECK(manifest->version == "1.0.0");
  }

  void test_squirrel_happy_path() {
    const std::string full_sha1 = "0123456789abcdef0123456789abcdef01234567";
    const std::string delta_sha1 = "89abcdef0123456789abcdef0123456789abcdef";
    const std::string feed =
        full_sha1 + " MyApp-1.0.0-full.nupkg 123\n" + delta_sha1 + " MyApp-1.0.0-delta.nupkg 45\n";
    std::string error;
    auto manifest = fxe::runtime::parse_squirrel_releases(feed, error);
    CHECK(manifest.has_value());
    CHECK(error.empty());
    if (manifest) {
      CHECK(manifest->version == "1.0.0");
      CHECK(manifest->artifacts.size() == 2);
      if (manifest->artifacts.size() == 2) {
        CHECK(manifest->artifacts[0].kind == "full");
        CHECK(manifest->artifacts[0].url == "MyApp-1.0.0-full.nupkg");
        CHECK(manifest->artifacts[1].kind == "bsdiff");
        CHECK(manifest->artifacts[1].code_signature == "sha1=" + delta_sha1);
      }
    }
  }

  void test_squirrel_comments_and_blanks() {
    const std::string feed = "# comment\n"
                             "\n"
                             "0123456789abcdef0123456789abcdef01234567 MyApp-1.0.0-full.nupkg 123\n"
                             "\n"
                             "# trailing comment\n";
    std::string error;
    auto manifest = fxe::runtime::parse_squirrel_releases(feed, error);
    CHECK(manifest.has_value());
    CHECK(error.empty());
    if (manifest)
      CHECK(manifest->artifacts.size() == 1);
  }

  void test_squirrel_malformed() {
    std::string error;
    auto manifest = fxe::runtime::parse_squirrel_releases(
        "0123456789abcdef0123456789abcdef01234567 MyApp-1.0.0-full.nupkg\n", error);
    CHECK(!manifest.has_value());
    CHECK(!error.empty());
  }

  void test_squirrel_no_full_entry() {
    std::string error;
    auto manifest = fxe::runtime::parse_squirrel_releases(
        "0123456789abcdef0123456789abcdef01234567 MyApp-1.0.0-delta.nupkg 12\n", error);
    CHECK(!manifest.has_value());
    CHECK(error.find("no full") != std::string::npos);
  }

  void test_squirrel_version_selection() {
    const std::string feed = "0123456789abcdef0123456789abcdef01234567 MyApp-1.0.0-full.nupkg 1\n"
                             "1111111111111111111111111111111111111111 MyApp-1.2.0-full.nupkg 1\n"
                             "2222222222222222222222222222222222222222 MyApp-1.1.5-full.nupkg 1\n";
    std::string error;
    auto manifest = fxe::runtime::parse_squirrel_releases(feed, error);
    CHECK(manifest.has_value());
    CHECK(error.empty());
    if (manifest)
      CHECK(manifest->version == "1.2.0");
  }
} // namespace

int main() {
  test_sparkle_happy_path();
  test_sparkle_malformed();
  test_sparkle_missing_sha256();
  test_sparkle_doctype_rejected();
  test_sparkle_entity_decode();
  test_squirrel_happy_path();
  test_squirrel_comments_and_blanks();
  test_squirrel_malformed();
  test_squirrel_no_full_entry();
  test_squirrel_version_selection();
  std::fprintf(stderr, "updater_feed_parsers_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
