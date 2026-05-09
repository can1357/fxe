import { constants, Database, type SQLBindings, version } from 'fxe:sqlite';
import { assert, assertDeepEqual, assertEqual, assertThrows, test } from './ts_harness.ts';

interface Row {
  id: number;
  name: string;
}

test('sqlite: open in-memory and run simple select', () => {
  const db = new Database(':memory:');
  try {
    const row = db.query<{ message: string }>("select 'hello' as message").get();
    assertDeepEqual(row, { message: 'hello' });
  } finally {
    db.close();
  }
});

test('sqlite: default constructor opens in-memory', () => {
  const db = new Database();
  try {
    const row = db.query<{ x: number }>('select 1 as x').get();
    assert(row !== null);
    assertEqual(row.x, 1);
  } finally {
    db.close();
  }
});

test('sqlite: positional params via run/all', () => {
  const db = new Database(':memory:');
  try {
    db.run('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)');
    const ins = db.query('INSERT INTO t (name) VALUES (?1)');
    ins.run('alice');
    ins.run('bob');
    const rows = db.query<Row>('SELECT id, name FROM t ORDER BY id').all();
    assertDeepEqual(rows, [
      { id: 1, name: 'alice' },
      { id: 2, name: 'bob' },
    ]);
  } finally {
    db.close();
  }
});

test('sqlite: named params with $ prefix', () => {
  const db = new Database(':memory:');
  try {
    const r = db
      .query<{ n: string; a: number }>('select $name as n, $age as a')
      .get({ $name: 'x', $age: 7 });
    assertDeepEqual(r, { n: 'x', a: 7 });
  } finally {
    db.close();
  }
});

test('sqlite: strict mode binds bare keys and errors on missing', () => {
  const db = new Database(':memory:', { strict: true });
  try {
    const row = db.query<{ n: string }>('select $name as n').get({ name: 'ok' });
    assertDeepEqual(row, { n: 'ok' });
    assertThrows(() => {
      const stmt = db.query<{ n: string }>('select $name as n');
      const params: SQLBindings = { wrong: 'x' };
      stmt.get(params);
    }, /Missing parameter/);
  } finally {
    db.close();
  }
});

test('sqlite: run returns lastInsertRowid + changes', () => {
  const db = new Database(':memory:');
  try {
    db.run('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    const r1 = db.query('INSERT INTO t (v) VALUES (?1)').run('a');
    assertEqual(r1.changes, 1);
    assertEqual(r1.lastInsertRowid, 1);
    const r2 = db.run('INSERT INTO t (v) VALUES (?), (?)', ['b', 'c']);
    assertEqual(r2.changes, 2);
    assertEqual(r2.lastInsertRowid, 3);
  } finally {
    db.close();
  }
});

test('sqlite: values returns row arrays', () => {
  const db = new Database(':memory:');
  try {
    db.run('CREATE TABLE t (a INTEGER, b TEXT)');
    db.run("INSERT INTO t VALUES (1, 'one'), (2, 'two')");
    const vals = db.query('SELECT a, b FROM t ORDER BY a').values();
    assertDeepEqual(vals, [
      [1, 'one'],
      [2, 'two'],
    ]);
  } finally {
    db.close();
  }
});

test('sqlite: iterate yields rows lazily', () => {
  const db = new Database(':memory:');
  try {
    db.run('CREATE TABLE t (n INTEGER)');
    db.run('INSERT INTO t VALUES (1), (2), (3)');
    const stmt = db.query<{ n: number }>('SELECT n FROM t ORDER BY n');
    const collected: number[] = [];
    for (const row of stmt.iterate()) {
      collected.push(row.n);
    }
    assertDeepEqual(collected, [1, 2, 3]);

    // Symbol.iterator works directly on the statement too.
    const again: number[] = [];
    for (const row of stmt) {
      again.push(row.n);
    }
    assertDeepEqual(again, [1, 2, 3]);
  } finally {
    db.close();
  }
});

test('sqlite: as(Class) maps rows onto class prototype', () => {
  class Movie {
    title!: string;
    year!: number;
    get isOld(): boolean {
      return this.year < 2000;
    }
  }
  const db = new Database(':memory:');
  try {
    db.run('CREATE TABLE m (title TEXT, year INTEGER)');
    db.run("INSERT INTO m VALUES ('Alien', 1979), ('Tenet', 2020)");
    const stmt = db.query('SELECT title, year FROM m ORDER BY year').as(Movie);
    const rows = stmt.all();
    assertEqual(rows.length, 2);
    assert(rows[0] instanceof Movie);
    assertEqual(rows[0].title, 'Alien');
    assertEqual(rows[0].isOld, true);
    assertEqual(rows[1].isOld, false);
  } finally {
    db.close();
  }
});

test('sqlite: BLOB roundtrips as Uint8Array', () => {
  const db = new Database(':memory:');
  try {
    db.run('CREATE TABLE b (id INTEGER PRIMARY KEY, data BLOB)');
    const payload = new Uint8Array([1, 2, 3, 4, 250]);
    db.query('INSERT INTO b (data) VALUES (?1)').run(payload);
    const got = db.query<{ data: Uint8Array }>('SELECT data FROM b LIMIT 1').get();
    assert(got !== null);
    assert(got.data instanceof Uint8Array);
    assertDeepEqual(Array.from(got.data), [1, 2, 3, 4, 250]);
  } finally {
    db.close();
  }
});

test('sqlite: NULL and boolean coercion', () => {
  const db = new Database(':memory:');
  try {
    const row = db
      .query<{ n: number | null; t: number; f: number }>('select $n as n, $t as t, $f as f')
      .get({ $n: null, $t: true, $f: false });
    assertDeepEqual(row, { n: null, t: 1, f: 0 });
  } finally {
    db.close();
  }
});

test('sqlite: safeIntegers returns BigInt rowids and large columns', () => {
  const db = new Database(':memory:', { safeIntegers: true });
  try {
    db.run('CREATE TABLE t (v INTEGER)');
    const big = BigInt(Number.MAX_SAFE_INTEGER) + 100n;
    db.query('INSERT INTO t (v) VALUES (?1)').run(big);
    const r = db.query<{ v: bigint }>('SELECT v FROM t').get();
    assert(r !== null);
    assertEqual(typeof r.v, 'bigint');
    assertEqual(r.v, big);
  } finally {
    db.close();
  }
});

test('sqlite: transaction commits then rolls back on throw', () => {
  const db = new Database(':memory:');
  try {
    db.run('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    const insert = db.query('INSERT INTO t (v) VALUES (?1)');

    const insertMany = db.transaction((items: string[]) => {
      for (const item of items) insert.run(item);
      return items.length;
    });

    const n = insertMany(['a', 'b', 'c']);
    assertEqual(n, 3);
    assertEqual(db.query<{ c: number }>('SELECT count(*) as c FROM t').get()?.c, 3);

    const rollbackMany = db.transaction((items: string[]) => {
      for (const item of items) {
        if (item === 'BOOM') throw new Error('rollback me');
        insert.run(item);
      }
    });
    assertThrows(() => rollbackMany(['x', 'BOOM']), /rollback me/);

    // Rollback should undo only the failed transaction; first 3 rows survive.
    assertEqual(db.query<{ c: number }>('SELECT count(*) as c FROM t').get()?.c, 3);
  } finally {
    db.close();
  }
});

test('sqlite: nested transaction uses savepoints', () => {
  const db = new Database(':memory:');
  try {
    db.run('CREATE TABLE t (v TEXT)');
    const ins = db.query('INSERT INTO t (v) VALUES (?1)');
    const inner = db.transaction((items: string[]) => {
      for (const v of items) ins.run(v);
    });
    const outer = db.transaction(() => {
      ins.run('outer');
      inner(['a', 'b']);
    });
    outer();
    const rows = db.query<{ v: string }>('SELECT v FROM t ORDER BY rowid').all();
    assertDeepEqual(
      rows.map((r) => r.v),
      ['outer', 'a', 'b'],
    );
  } finally {
    db.close();
  }
});

test('sqlite: serialize + Database.deserialize roundtrip', () => {
  const a = new Database(':memory:');
  try {
    a.run('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    a.run("INSERT INTO t (v) VALUES ('alpha'), ('beta')");
    const bytes = a.serialize();
    assert(bytes instanceof Uint8Array);
    assert(bytes.byteLength > 0);

    const b = Database.deserialize(bytes);
    try {
      const rows = b.query<{ v: string }>('SELECT v FROM t ORDER BY id').all();
      assertDeepEqual(
        rows.map((r) => r.v),
        ['alpha', 'beta'],
      );
    } finally {
      b.close();
    }
  } finally {
    a.close();
  }
});

test('sqlite: columnNames / paramsCount / toString reflect statement', () => {
  const db = new Database(':memory:');
  try {
    const stmt = db.query('SELECT $a as a, $b as b');
    assertEqual(stmt.paramsCount, 2);
    stmt.run({ $a: 1, $b: 2 });
    assertDeepEqual(stmt.columnNames, ['a', 'b']);
    const expanded = stmt.toString();
    assert(expanded.includes('1'));
    assert(expanded.includes('2'));
  } finally {
    db.close();
  }
});

test('sqlite: closed database rejects further queries', () => {
  const db = new Database(':memory:');
  db.close();
  assertThrows(() => db.query('select 1').get(), /database is closed/);
});

test('sqlite: constants + version are exposed', () => {
  assertEqual(typeof version(), 'string');
  assert(constants.SQLITE_OPEN_READONLY > 0);
  assertEqual(typeof constants.SQLITE_FCNTL_PERSIST_WAL, 'number');
});
