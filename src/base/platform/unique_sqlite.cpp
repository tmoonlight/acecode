#include "unique_sqlite.hpp"
#include <sqlite3.h>

namespace acecode::platform {
void SqliteTraits::close(sqlite3* db) noexcept {
    // Statements normally finish before their database. If a native statement
    // still exists, SQLite defers freeing the connection until it is finalized.
    sqlite3_close_v2(db);
}
} // namespace acecode::platform
