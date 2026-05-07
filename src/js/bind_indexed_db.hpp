#pragma once

#include <v8.h>

namespace fxe::js {
  // Installs `indexedDB` (IDBFactory), `IDBKeyRange`, and the related class
  // templates (IDBDatabase / IDBTransaction / IDBObjectStore / IDBIndex /
  // IDBRequest / IDBOpenDBRequest / IDBCursor / IDBCursorWithValue) on the
  // global object template. Backing storage is per-database SQLite under
  // `${App.getPath('userData')}/idb/<safe(name)>.sqlite3`. v1 supports a
  // faithful subset of the IDB spec — see types/fxe.d.ts for the surface.
  void install_indexed_db_bindings(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js
