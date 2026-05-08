// Coverage for fxe::markdown::parse — exercises every block kind, span kind,
// and text kind we expose through the public AST. No external test
// framework: a tiny CHECK macro mirroring the rest of the repo.
#include <fxe/markdown.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace md = fxe::markdown;

namespace {

  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  // Concatenate every text-leaf descendant of `n`.
  std::string text_of(const md::node& n) {
    std::string out;
    for (const auto& c : n.children) {
      switch (c->kind) {
      case md::node_kind::text:
      case md::node_kind::entity:
      case md::node_kind::raw_html:
      case md::node_kind::null_char:
        out += c->text;
        break;
      case md::node_kind::soft_break:
        out += '\n';
        break;
      case md::node_kind::hard_break:
        out += "\\n";
        break;
      default:
        out += text_of(*c);
        break;
      }
    }
    return out;
  }

  // Find the first descendant matching `kind` (DFS, pre-order).
  const md::node* find_first(const md::node& root, md::node_kind kind) {
    if (root.kind == kind)
      return &root;
    for (const auto& c : root.children) {
      if (auto* hit = find_first(*c, kind))
        return hit;
    }
    return nullptr;
  }

  size_t count_kind(const md::node& root, md::node_kind kind) {
    size_t n = (root.kind == kind) ? 1 : 0;
    for (const auto& c : root.children)
      n += count_kind(*c, kind);
    return n;
  }

} // namespace

int main() {
  // Empty input → just a document.
  {
    auto doc = md::parse("");
    CHECK(doc != nullptr);
    CHECK(doc->kind == md::node_kind::document);
    CHECK(doc->children.empty());
  }

  // Plain paragraph.
  {
    auto doc = md::parse("hello world\n");
    CHECK(doc->children.size() == 1);
    CHECK(doc->children[0]->kind == md::node_kind::paragraph);
    CHECK(text_of(*doc->children[0]) == "hello world");
  }

  // ATX headings up to level 6.
  {
    auto doc = md::parse("# h1\n## h2\n### h3\n#### h4\n##### h5\n###### h6\n");
    CHECK(doc->children.size() == 6);
    for (int i = 0; i < 6; ++i) {
      const auto& h = *doc->children[static_cast<size_t>(i)];
      CHECK(h.kind == md::node_kind::heading);
      CHECK(h.heading_level == i + 1);
    }
    CHECK(text_of(*doc->children[0]) == "h1");
  }

  // Emphasis + strong.
  {
    auto doc = md::parse("*em* **strong** ***both***\n");
    auto* p = doc->children[0].get();
    CHECK(p->kind == md::node_kind::paragraph);
    CHECK(count_kind(*p, md::node_kind::emph) >= 2);
    CHECK(count_kind(*p, md::node_kind::strong) >= 2);
  }

  // Inline code span and fenced code block (with info string).
  {
    auto doc = md::parse("Inline `x = 1` code.\n\n```ts highlight\nlet a = 1;\n```\n");
    CHECK(count_kind(*doc, md::node_kind::code_span) == 1);
    auto* code = find_first(*doc, md::node_kind::code_block);
    CHECK(code != nullptr);
    CHECK(code->lang == "ts");
    CHECK(code->info.find("highlight") != std::string::npos);
    CHECK(!code->children.empty());
    CHECK(text_of(*code) == "let a = 1;\n");
  }

  // Blockquote containing a paragraph.
  {
    auto doc = md::parse("> quoted\n");
    auto* bq = find_first(*doc, md::node_kind::blockquote);
    CHECK(bq != nullptr);
    CHECK(text_of(*bq) == "quoted");
  }

  // Bulleted list, three items, tight.
  {
    auto doc = md::parse("- a\n- b\n- c\n");
    auto* list = find_first(*doc, md::node_kind::list);
    CHECK(list != nullptr);
    CHECK(!list->ordered);
    CHECK(list->tight);
    size_t items = 0;
    for (const auto& c : list->children)
      if (c->kind == md::node_kind::list_item)
        ++items;
    CHECK(items == 3);
  }

  // Ordered list with custom start.
  {
    auto doc = md::parse("3. first\n4. second\n");
    auto* list = find_first(*doc, md::node_kind::list);
    CHECK(list != nullptr);
    CHECK(list->ordered);
    CHECK(list->list_start == 3);
  }

  // Task list (GFM).
  {
    auto doc = md::parse("- [x] done\n- [ ] todo\n");
    size_t tasks = 0, checked = 0;
    auto* list = find_first(*doc, md::node_kind::list);
    CHECK(list != nullptr);
    for (const auto& c : list->children) {
      if (c->kind != md::node_kind::list_item)
        continue;
      if (c->task)
        ++tasks;
      if (c->checked)
        ++checked;
    }
    CHECK(tasks == 2);
    CHECK(checked == 1);
  }

  // Thematic break.
  {
    auto doc = md::parse("---\n");
    CHECK(count_kind(*doc, md::node_kind::thematic_break) == 1);
  }

  // Inline link with title and explicit href.
  {
    auto doc = md::parse("[fxe](https://example.com \"home\")\n");
    auto* link = find_first(*doc, md::node_kind::link);
    CHECK(link != nullptr);
    CHECK(link->href == "https://example.com");
    CHECK(link->title == "home");
    CHECK(!link->autolink);
    CHECK(text_of(*link) == "fxe");
  }

  // Autolink (GFM permissive URL).
  {
    auto doc = md::parse("see https://example.com for more\n");
    auto* link = find_first(*doc, md::node_kind::link);
    CHECK(link != nullptr);
    CHECK(link->href == "https://example.com");
    CHECK(link->autolink);
  }

  // Image with alt + src.
  {
    auto doc = md::parse("![alt text](https://example.com/x.png)\n");
    auto* img = find_first(*doc, md::node_kind::image);
    CHECK(img != nullptr);
    CHECK(img->src == "https://example.com/x.png");
    CHECK(text_of(*img) == "alt text");
  }

  // Strikethrough (GFM).
  {
    auto doc = md::parse("~~gone~~\n");
    auto* s = find_first(*doc, md::node_kind::strikethrough);
    CHECK(s != nullptr);
    CHECK(text_of(*s) == "gone");
  }

  // Tables (GFM).
  {
    auto doc = md::parse("| a | b |\n|:-:|--:|\n| 1 | 2 |\n");
    auto* tbl = find_first(*doc, md::node_kind::table);
    CHECK(tbl != nullptr);
    auto* head = find_first(*tbl, md::node_kind::table_head);
    CHECK(head != nullptr);
    auto* row = find_first(*head, md::node_kind::table_row);
    CHECK(row != nullptr);
    CHECK(row->children.size() == 2);
    CHECK(row->children[0]->kind == md::node_kind::table_cell);
    CHECK(row->children[0]->align == "center");
    CHECK(row->children[1]->align == "right");
  }

  // Hard break (two trailing spaces).
  {
    auto doc = md::parse("a  \nb\n");
    auto* p = doc->children[0].get();
    CHECK(p->kind == md::node_kind::paragraph);
    CHECK(count_kind(*p, md::node_kind::hard_break) == 1);
  }

  // HTML block + raw inline HTML.
  {
    auto doc = md::parse("<div>raw</div>\n\nparagraph with <em>tag</em>\n");
    CHECK(count_kind(*doc, md::node_kind::html_block) == 1);
    CHECK(count_kind(*doc, md::node_kind::raw_html) >= 1);
  }

  // Entity passes through.
  {
    auto doc = md::parse("a &amp; b\n");
    CHECK(count_kind(*doc, md::node_kind::entity) == 1);
  }

  // CommonMark dialect disables tables/strikethrough.
  {
    md::parse_options opts;
    opts.flags = md::dialect_commonmark;
    auto doc = md::parse("~~x~~\n\n| a | b |\n|---|---|\n| 1 | 2 |\n", opts);
    CHECK(count_kind(*doc, md::node_kind::strikethrough) == 0);
    CHECK(count_kind(*doc, md::node_kind::table) == 0);
  }

  // kind_tag round-trip on a representative subset.
  {
    using K = md::node_kind;
    CHECK(md::kind_tag(K::document) == "document");
    CHECK(md::kind_tag(K::paragraph) == "paragraph");
    CHECK(md::kind_tag(K::heading) == "heading");
    CHECK(md::kind_tag(K::code_block) == "code_block");
    CHECK(md::kind_tag(K::list_item) == "list_item");
    CHECK(md::kind_tag(K::table_cell) == "table_cell");
    CHECK(md::kind_tag(K::strikethrough) == "strikethrough");
    CHECK(md::kind_tag(K::soft_break) == "soft_break");
  }

  return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
