#pragma once

// Minimal PostgreSQL ("hiss" database) access for trainer.exe.
//
// Trainer is deliberately self-contained and does NOT use the heavyweight
// CTablemap / CTablemapDB classes. These helpers wrap libpq directly so the
// trainer can read its balance regions and bitmap fonts from the database and
// write colour/radius and font edits back. libpq is already linked into the
// trainer project (..\lib\libpq.lib, include dir ..\postgres).

#include <vector>

struct pconn;   // == PGconn (forward declaration; avoids libpq-fe.h in headers)

// Opens a new connection to the hiss database (caller must PQfinish the result).
// Returns NULL on failure. The connection string defaults to the local server
// (postgres/dbpass@127.0.0.1:5432, dbname=hiss) and can be overridden via the
// registry value "hiss_conn".
pconn *TrainerDB_Connect();

// Lists every tablemap name in the database (sorted). Returns false on error.
bool TrainerDB_ListTablemaps(std::vector<CString> *names);

// Returns the tablemaps.id for a name, or -1 if not found / on error.
long TrainerDB_TablemapId(pconn *conn, const CString &name);

// Bumps tablemaps.updated_at=now() for the given id, on an already-open connection.
// Lets Hiss's heartbeat detect granular region edits (which only touch tm_regions).
void TrainerDB_TouchTablemap(pconn *conn, long id);

// --- Global application settings (the "settings" table: key TEXT -> JSONB) -----
// Each call opens and closes its own connection. `key` is the settings row;
// `field` is a named field inside that row's JSON value.
CString TrainerDB_GetSetting(const CString &key, const CString &field);
bool    TrainerDB_SetSetting(const CString &key, const CString &field, const CString &value);
// Writes `values` as a JSON array into <key>.value->'<field>' (mirrors
// CTablemapDB::SetSettingArray, the same format Hiss/Vision read via GetSettingArray).
bool    TrainerDB_SetSettingArray(const CString &key, const CString &field, const std::vector<CString> &values);

// --- Persisted UI window placement --------------------------------------------
// Position + size are stored as "x,y,w,h" (screen coords) under the settings key
// "trainer_windows", one field per window (e.g. "main", "screenshot").
void TrainerDB_SaveWindowRect(HWND hwnd, const char *field);
// Restores a previously saved rect. Returns false (leaving the window where it is)
// when nothing is saved or the saved rect no longer lands on any monitor.
bool TrainerDB_RestoreWindowRect(HWND hwnd, const char *field);
