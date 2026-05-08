import { assert, assertEqual, test } from './ts_harness.ts';

type Node = FXEMarkdown.Node;

function findFirst(root: Node, type: Node['type']): Node | undefined {
  if (root.type === type) return root;
  const children = (root as { children?: Node[] }).children;
  if (!children) return undefined;
  for (const c of children) {
    const hit = findFirst(c, type);
    if (hit) return hit;
  }
  return undefined;
}

function textOf(n: Node): string {
  if (n.type === 'text' || n.type === 'entity' || n.type === 'raw_html') return n.text;
  if (n.type === 'soft_break') return '\n';
  if (n.type === 'hard_break') return '\\n';
  const children = (n as { children?: Node[] }).children;
  if (!children) return '';
  return children.map(textOf).join('');
}

test('Markdown.parse returns a document root', () => {
  const doc = Markdown.parse('hello');
  assertEqual(doc.type, 'document');
  assert(Array.isArray(doc.children));
  assertEqual(doc.children.length, 1);
  assertEqual(doc.children[0]?.type, 'paragraph');
});

test('headings carry levels', () => {
  const doc = Markdown.parse('# h1\n## h2\n### h3');
  assertEqual(doc.children.length, 3);
  for (let i = 0; i < 3; i++) {
    const h = doc.children[i] as FXEMarkdown.HeadingNode;
    assertEqual(h.type, 'heading');
    assertEqual(h.level, (i + 1) as FXEMarkdown.HeadingNode['level']);
  }
});

test('emphasis, strong, code spans', () => {
  const doc = Markdown.parse('*em* **strong** `code`');
  assert(findFirst(doc, 'emph'), 'emph');
  assert(findFirst(doc, 'strong'), 'strong');
  assert(findFirst(doc, 'code_span'), 'code_span');
});

test('fenced code block exposes lang and info', () => {
  const doc = Markdown.parse('```ts highlight\nlet a = 1;\n```');
  const code = findFirst(doc, 'code_block') as FXEMarkdown.CodeBlockNode | undefined;
  assert(code, 'code_block present');
  assertEqual(code.lang, 'ts');
  assert(code.info.includes('highlight'));
  assertEqual(textOf(code), 'let a = 1;\n');
});

test('lists report ordered/tight and start', () => {
  const ul = findFirst(Markdown.parse('- a\n- b'), 'list') as FXEMarkdown.ListNode;
  assertEqual(ul.ordered, false);
  assertEqual(ul.tight, true);
  const ol = findFirst(Markdown.parse('3. a\n4. b'), 'list') as FXEMarkdown.ListNode;
  assertEqual(ol.ordered, true);
  assertEqual(ol.start, 3);
});

test('GFM task list items', () => {
  const list = findFirst(Markdown.parse('- [x] done\n- [ ] todo'), 'list') as FXEMarkdown.ListNode;
  const items = list.children;
  assertEqual(items.length, 2);
  assertEqual(items[0]?.task, true);
  assertEqual(items[0]?.checked, true);
  assertEqual(items[1]?.task, true);
  assertEqual(items[1]?.checked, false);
});

test('links carry href + title; autolinks flagged', () => {
  const explicit = findFirst(
    Markdown.parse('[fxe](https://example.com "home")'),
    'link',
  ) as FXEMarkdown.LinkNode;
  assertEqual(explicit.href, 'https://example.com');
  assertEqual(explicit.title, 'home');
  assert(!explicit.autolink);

  const auto = findFirst(
    Markdown.parse('see https://example.com here'),
    'link',
  ) as FXEMarkdown.LinkNode;
  assertEqual(auto.href, 'https://example.com');
  assertEqual(auto.autolink, true);
});

test('images expose src + alt children', () => {
  const img = findFirst(
    Markdown.parse('![alt text](https://example.com/x.png)'),
    'image',
  ) as FXEMarkdown.ImageNode;
  assertEqual(img.src, 'https://example.com/x.png');
  assertEqual(textOf(img), 'alt text');
});

test('GFM tables yield rows + cell alignment', () => {
  const doc = Markdown.parse('| a | b |\n|:-:|--:|\n| 1 | 2 |');
  const tbl = findFirst(doc, 'table') as FXEMarkdown.TableNode | undefined;
  assert(tbl);
  const head = findFirst(tbl, 'table_head') as FXEMarkdown.TableHeadNode;
  const row = head.children[0] as FXEMarkdown.TableRowNode;
  assertEqual(row.children.length, 2);
  assertEqual(row.children[0]?.align, 'center');
  assertEqual(row.children[1]?.align, 'right');
});

test('strikethrough is GFM only', () => {
  const gfm = Markdown.parse('~~gone~~');
  assert(findFirst(gfm, 'strikethrough'));
  const cm = Markdown.parse('~~gone~~', { dialect: 'commonmark' });
  assert(!findFirst(cm, 'strikethrough'));
});

test('explicit flags override dialect', () => {
  const doc = Markdown.parse('| a | b |\n|---|---|\n| 1 | 2 |', {
    dialect: 'commonmark',
    flags: Markdown.FLAG_TABLES,
  });
  assert(findFirst(doc, 'table'));
});

test('thematic break, hard break, raw html, entity', () => {
  assert(findFirst(Markdown.parse('---'), 'thematic_break'));
  assert(findFirst(Markdown.parse('a  \nb'), 'hard_break'));
  assert(findFirst(Markdown.parse('<div>x</div>'), 'html_block'));
  assert(findFirst(Markdown.parse('a &amp; b'), 'entity'));
});

test('Markdown.parse: bad opts throws TypeError', () => {
  let threw = false;
  try {
    // @ts-expect-error: invalid dialect string
    Markdown.parse('x', { dialect: 'bogus' });
  } catch (e) {
    threw = e instanceof TypeError;
  }
  assert(threw, 'expected TypeError');
});
