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
