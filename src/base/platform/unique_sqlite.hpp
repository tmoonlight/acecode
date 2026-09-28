#pragma once

#include "unique_resource.hpp"

struct sqlite3;

namespace acecode::platform {
struct SqliteTraits {
    using handle_type = sqlite3*;
    static sqlite3* invalid() noexcept { return nullptr; }
    static bool valid(sqlite3* db) noexcept { return db != nullptr; }
    static void close(sqlite3* db) noexcept;
};
using UniqueSqlite = UniqueResource<SqliteTraits>;
} // namespace acecode::platform
