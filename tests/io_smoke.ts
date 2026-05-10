// Type-only smoke for the I/O bindings (fs/path/process/timers). Real IO is
// gated behind `if (false)` so tsc --noEmit exercises the typings without the
// runner touching the filesystem.

if (globalThis.__FXE_TYPECHECK_ONLY__ === true) {
  const utf: string = fs.readFileSync('foo.txt', 'utf8');
  const raw: Uint8Array = fs.readFileSync('foo.bin');
  fs.writeFileSync('out.bin', raw);
  fs.appendFileSync('out.txt', utf);
  const ok: boolean = fs.existsSync('out.txt');
  const st = fs.statSync('out.txt');
  const _size: number = st.size;
  const _isf: boolean = st.isFile;
  const _isd: boolean = st.isDirectory;
  const _mt: number = st.mtimeMs;
  const names: string[] = fs.readdirSync('.');
  const ents = fs.readdirSync('.', { withFileTypes: true });
  fs.mkdirSync('a/b/c', { recursive: true });
  fs.rmSync('a', { recursive: true, force: true });
  fs.renameSync('out.txt', 'out2.txt');
  const rp: string = fs.realpathSync('out2.txt');

  // async
  fs.readFile('foo.txt', 'utf8').then((s: string) => console.log(s));
  fs.readFile('foo.bin').then((b: Uint8Array) => console.log(b.byteLength));
  fs.writeFile('o', 'data').then(() => {});
  fs.appendFile('o', 'data').then(() => {});
  fs.stat('o').then((s) => console.log(s.size));
  fs.readdir('.').then((xs: string[]) => console.log(xs.length));
  fs.mkdir('d', { recursive: true }).then(() => {});
  fs.rm('d', { recursive: true, force: true }).then(() => {});
  fs.rename('o', 'o2').then(() => {});
  fs.realpath('o2').then((s: string) => console.log(s));
  fs.exists('o2').then((b: boolean) => console.log(b));

  const dir: string = path.join(process.cwd(), 'a', 'b');
  const abs: string = path.resolve('a', 'b', 'c');
  const dn: string = path.dirname('/a/b/c');
  const bn: string = path.basename('/a/b/c.txt', '.txt');
  const ext: string = path.extname('foo.txt');
  const rel: string = path.relative('/a', '/a/b');
  const nm: string = path.normalize('/a/./b/../c');
  const isAbs: boolean = path.isAbsolute('/x');
  const _sep: string = path.sep;
  const _delim: string = path.delimiter;
  console.log(utf, names.length, ents.length, ok, dir, abs, dn, bn, ext, rel, nm, isAbs, rp);

  const argv: string[] = process.argv;
  const cwd: string = process.cwd();
  process.chdir('/tmp');
  const _plat: string = process.platform;
  const _arch: string = process.arch;
  const _pid: number = process.pid;
  const _v8: string = process.versions.v8;
  const _fxe: string = process.versions.fxe;
  process.stdout.write('hi');
  process.stderr.write('hi');
  process.on('exit', (code: number) => console.log(code));
  process.off('exit', () => {});
  process.nextTick(() => {});
  const home: string | undefined = process.env.HOME;
  process.env.MY_VAR = 'value';
  delete process.env.MY_VAR;
  console.log(argv.length, cwd, home);
  process.exit(0);
}

const tid = setTimeout(() => {}, 100);
clearTimeout(tid);
const iid = setInterval(() => {}, 100);
clearInterval(iid);
const imid = setImmediate(() => {});
clearTimeout(imid);
const rid = requestAnimationFrame((t: number) => console.log(t));
cancelAnimationFrame(rid);
queueMicrotask(() => {});

console.log(`io-smoke=ok`);
