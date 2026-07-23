#include "mereader-tui/database.h"
#include "mereader-tui/remote.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool mereader_tui_database_ready(const MereaderTuiDatabase *database, MereaderTuiError *error) {
    if (database == nullptr || database->handle == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "database is not open");
        return false;
    }
    return true;
}

static void mereader_tui_database_set_error(MereaderTuiDatabase *database, const char *operation, int status, MereaderTuiError *error) {
    const char *message = database != nullptr && database->handle != nullptr ? sqlite3_errmsg(database->handle)
                                                                            : sqlite3_errstr(status);
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "%s: %s", operation, message);
}

static bool mereader_tui_database_exec(MereaderTuiDatabase *database, const char *sql, const char *operation, MereaderTuiError *error) {
    char *sqlite_error = nullptr;
    int status = sqlite3_exec(database->handle, sql, nullptr, nullptr, &sqlite_error);
    if (status == SQLITE_OK) {
        return true;
    }

    mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "%s: %s", operation,
                   sqlite_error == nullptr ? sqlite3_errmsg(database->handle) : sqlite_error);
    sqlite3_free(sqlite_error);
    return false;
}

static void mereader_tui_database_close_after_failure(sqlite3 *handle, const char *path, MereaderTuiError *error) {
    if (handle == nullptr) {
        return;
    }
    int status = sqlite3_close(handle);
    if (status != SQLITE_OK) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "could not close database '%s' after setup failure: %s", path,
                       sqlite3_errstr(status));
    }
}

bool mereader_tui_database_open(MereaderTuiDatabase *database, const char *path, MereaderTuiError *error) {
    if (database == nullptr || path == nullptr || path[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid database path");
        return false;
    }
    if (database->handle != nullptr || database->path != nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "database output is not empty");
        return false;
    }

    char *stored_path = mereader_tui_strdup(path, error);
    if (stored_path == nullptr) {
        return false;
    }

    sqlite3 *handle = nullptr;
    int status = sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
    if (status != SQLITE_OK) {
        const char *sqlite_message = handle == nullptr ? sqlite3_errstr(status) : sqlite3_errmsg(handle);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "could not open database '%s': %s", path, sqlite_message);
        mereader_tui_database_close_after_failure(handle, path, error);
        free(stored_path);
        return false;
    }

    status = sqlite3_busy_timeout(handle, 5000);
    if (status != SQLITE_OK) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "could not configure database '%s': %s", path,
                       sqlite3_errmsg(handle));
        mereader_tui_database_close_after_failure(handle, path, error);
        free(stored_path);
        return false;
    }

    *database = (MereaderTuiDatabase){.handle = handle, .path = stored_path};
    return true;
}

bool mereader_tui_database_open_default(MereaderTuiDatabase *database, MereaderTuiError *error) {
    if (database == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "database output is null");
        return false;
    }
    if (database->handle != nullptr || database->path != nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "database output is not empty");
        return false;
    }

    char *path = mereader_tui_xdg_cache_path(MEREADER_TUI_NAME ".db", error);
    if (path == nullptr) {
        return false;
    }
    char *directory = mereader_tui_path_dirname(path, error);
    if (directory == nullptr) {
        free(path);
        return false;
    }
    if (!mereader_tui_mkdirs(directory, error)) {
        free(directory);
        free(path);
        return false;
    }
    free(directory);

    bool opened = mereader_tui_database_open(database, path, error);
    free(path);
    return opened;
}

void mereader_tui_database_close(MereaderTuiDatabase *database) {
    if (database == nullptr) {
        return;
    }
    if (database->handle != nullptr && sqlite3_close(database->handle) != SQLITE_OK) {
        return;
    }
    free(database->path);
    *database = (MereaderTuiDatabase){0};
}

bool mereader_tui_database_migrate(MereaderTuiDatabase *database, MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error)) {
        return false;
    }

    static const char metadata_sql[] =
        "CREATE TABLE IF NOT EXISTS \"metadata\" ("
        "\"version\" INTEGER NOT NULL PRIMARY KEY, "
        "\"migrated_at\" DATETIME NOT NULL)";
    static const char reading_history_sql[] =
        "CREATE TABLE IF NOT EXISTS \"reading_history\" ("
        "\"filepath\" VARCHAR(255) NOT NULL PRIMARY KEY, "
        "\"title\" VARCHAR(255), "
        "\"author\" VARCHAR(255), "
        "\"reading_progress\" REAL NOT NULL, "
        "\"last_read\" DATETIME NOT NULL)";
    static const char bookmarks_sql[] =
        "CREATE TABLE IF NOT EXISTS \"bookmarks\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY, "
        "\"filepath\" TEXT NOT NULL, "
        "\"reading_progress\" REAL NOT NULL CHECK(\"reading_progress\" >= 0.0 AND \"reading_progress\" <= 1.0), "
        "\"created_at\" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "UNIQUE(\"filepath\", \"reading_progress\"))";
    static const char format_preferences_sql[] =
        "CREATE TABLE IF NOT EXISTS \"library_format_preferences\" ("
        "\"library_root\" TEXT NOT NULL, "
        "\"book_key\" TEXT NOT NULL, "
        "\"relative_path\" TEXT NOT NULL, "
        "PRIMARY KEY (\"library_root\", \"book_key\"))";
    static const char migration_zero_sql[] =
        "INSERT INTO \"metadata\" (\"version\", \"migrated_at\") "
        "VALUES (0, CURRENT_TIMESTAMP) ON CONFLICT(\"version\") DO NOTHING";
    static const char migration_one_sql[] =
        "INSERT INTO \"metadata\" (\"version\", \"migrated_at\") "
        "VALUES (1, CURRENT_TIMESTAMP) ON CONFLICT(\"version\") DO NOTHING";
    static const char migration_two_sql[] =
        "INSERT INTO \"metadata\" (\"version\", \"migrated_at\") "
        "VALUES (2, CURRENT_TIMESTAMP) ON CONFLICT(\"version\") DO NOTHING";

    if (!mereader_tui_database_exec(database, "BEGIN IMMEDIATE", "could not begin database migration", error)) {
        return false;
    }
    if (!mereader_tui_database_exec(database, metadata_sql, "could not create metadata table", error) ||
        !mereader_tui_database_exec(database, reading_history_sql, "could not create reading history table", error) ||
        !mereader_tui_database_exec(database, migration_zero_sql, "could not record database migration", error) ||
        !mereader_tui_database_exec(database, bookmarks_sql, "could not create bookmarks table", error) ||
        !mereader_tui_database_exec(database, migration_one_sql, "could not record bookmark migration", error) ||
        !mereader_tui_database_exec(database, format_preferences_sql, "could not create format preferences table", error) ||
        !mereader_tui_database_exec(database, migration_two_sql, "could not record format preference migration", error) ||
        !mereader_tui_database_exec(database, "COMMIT", "could not commit database migration", error)) {
        (void)sqlite3_exec(database->handle, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

static void mereader_tui_history_entry_free(MereaderTuiHistoryEntry *entry) {
    free(entry->filepath);
    free(entry->title);
    free(entry->author);
    free(entry->last_read);
    *entry = (MereaderTuiHistoryEntry){0};
}

void mereader_tui_history_free(MereaderTuiHistory *history) {
    if (history == nullptr) {
        return;
    }
    for (size_t index = 0U; index < history->length; ++index) {
        mereader_tui_history_entry_free(&history->items[index]);
    }
    free(history->items);
    *history = (MereaderTuiHistory){0};
}

static bool mereader_tui_database_column_string(sqlite3_stmt *statement, int column, bool required, char **value,
                                        MereaderTuiError *error) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        if (required) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "database contains a null required history value");
            return false;
        }
        *value = nullptr;
        return true;
    }

    const unsigned char *text = sqlite3_column_text(statement, column);
    int byte_count = sqlite3_column_bytes(statement, column);
    if (text == nullptr || byte_count < 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "could not read history text from the database");
        return false;
    }
    if (memchr(text, '\0', (size_t)byte_count) != nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "database history text contains an embedded null byte");
        return false;
    }
    *value = mereader_tui_strndup((const char *)text, (size_t)byte_count, error);
    return *value != nullptr;
}

static bool mereader_tui_database_read_history(MereaderTuiDatabase *database, MereaderTuiHistory *history, MereaderTuiError *error) {
    static const char sql[] =
        "SELECT \"filepath\", \"title\", \"author\", \"reading_progress\", \"last_read\" "
        "FROM \"reading_history\" ORDER BY \"last_read\" DESC";

    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle, sql, -1, &statement, nullptr);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not prepare history query", status, error);
        return false;
    }

    for (;;) {
        status = sqlite3_step(statement);
        if (status == SQLITE_DONE) {
            break;
        }
        if (status != SQLITE_ROW) {
            mereader_tui_database_set_error(database, "could not read history", status, error);
            (void)sqlite3_finalize(statement);
            return false;
        }

        MereaderTuiHistoryEntry entry = {0};
        if (!mereader_tui_database_column_string(statement, 0, true, &entry.filepath, error) ||
            !mereader_tui_database_column_string(statement, 1, false, &entry.title, error) ||
            !mereader_tui_database_column_string(statement, 2, false, &entry.author, error) ||
            !mereader_tui_database_column_string(statement, 4, true, &entry.last_read, error)) {
            mereader_tui_history_entry_free(&entry);
            (void)sqlite3_finalize(statement);
            return false;
        }
        entry.reading_progress = sqlite3_column_double(statement, 3);
        if (!isfinite(entry.reading_progress)) {
            mereader_tui_history_entry_free(&entry);
            (void)sqlite3_finalize(statement);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "database contains non-finite reading progress");
            return false;
        }

        MereaderTuiError reserve_error = {0};
        MereaderTuiHistoryEntry *items = mereader_tui_array_reserve(history->items, &history->capacity, sizeof(*history->items),
                                                     history->length + 1U, &reserve_error);
        if (mereader_tui_error_is_set(&reserve_error)) {
            if (error != nullptr) {
                *error = reserve_error;
            }
            mereader_tui_history_entry_free(&entry);
            (void)sqlite3_finalize(statement);
            return false;
        }
        history->items = items;
        history->items[history->length++] = entry;
    }

    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish history query", status, error);
        return false;
    }
    return true;
}

static bool mereader_tui_database_delete_missing(MereaderTuiDatabase *database, MereaderTuiHistory *history, MereaderTuiError *error) {
    if (history->length == 0U) {
        return true;
    }

    bool *missing = mereader_tui_reallocarray(nullptr, history->length, sizeof(*missing), error);
    if (missing == nullptr) {
        return false;
    }
    size_t missing_count = 0U;
    for (size_t index = 0U; index < history->length; ++index) {
        missing[index] = !mereader_tui_remote_is_url(history->items[index].filepath) &&
                         !mereader_tui_file_exists(history->items[index].filepath);
        if (missing[index]) {
            ++missing_count;
        }
    }
    if (missing_count == 0U) {
        free(missing);
        return true;
    }

    if (!mereader_tui_database_exec(database, "BEGIN IMMEDIATE", "could not begin stale history cleanup", error)) {
        free(missing);
        return false;
    }

    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle,
                                    "DELETE FROM \"reading_history\" WHERE \"filepath\" = ?1", -1,
                                    &statement, nullptr);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not prepare stale history cleanup", status, error);
        (void)sqlite3_exec(database->handle, "ROLLBACK", nullptr, nullptr, nullptr);
        free(missing);
        return false;
    }

    for (size_t index = 0U; index < history->length; ++index) {
        MereaderTuiHistoryEntry *entry = &history->items[index];
        if (!missing[index]) {
            continue;
        }

        status = sqlite3_bind_text(statement, 1, entry->filepath, -1, SQLITE_TRANSIENT);
        if (status == SQLITE_OK) {
            status = sqlite3_step(statement);
        }
        if (status != SQLITE_DONE) {
            mereader_tui_database_set_error(database, "could not delete stale history", status, error);
            (void)sqlite3_finalize(statement);
            (void)sqlite3_exec(database->handle, "ROLLBACK", nullptr, nullptr, nullptr);
            free(missing);
            return false;
        }
        status = sqlite3_reset(statement);
        if (status == SQLITE_OK) {
            status = sqlite3_clear_bindings(statement);
        }
        if (status != SQLITE_OK) {
            mereader_tui_database_set_error(database, "could not reset stale history cleanup", status, error);
            (void)sqlite3_finalize(statement);
            (void)sqlite3_exec(database->handle, "ROLLBACK", nullptr, nullptr, nullptr);
            free(missing);
            return false;
        }
    }

    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish stale history cleanup", status, error);
        (void)sqlite3_exec(database->handle, "ROLLBACK", nullptr, nullptr, nullptr);
        free(missing);
        return false;
    }
    if (!mereader_tui_database_exec(database, "COMMIT", "could not commit stale history cleanup", error)) {
        (void)sqlite3_exec(database->handle, "ROLLBACK", nullptr, nullptr, nullptr);
        free(missing);
        return false;
    }

    size_t retained = 0U;
    for (size_t index = 0U; index < history->length; ++index) {
        MereaderTuiHistoryEntry *entry = &history->items[index];
        if (missing[index]) {
            mereader_tui_history_entry_free(entry);
            continue;
        }
        if (retained != index) {
            history->items[retained] = *entry;
            *entry = (MereaderTuiHistoryEntry){0};
        }
        ++retained;
    }
    history->length = retained;
    free(missing);
    return true;
}

bool mereader_tui_database_history(MereaderTuiDatabase *database, bool remove_missing, MereaderTuiHistory *history, MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error) || history == nullptr) {
        if (history == nullptr && !mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "history output is null");
        }
        return false;
    }
    if (history->items != nullptr || history->length != 0U || history->capacity != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "history output is not empty");
        return false;
    }

    MereaderTuiHistory result = {0};
    if (!mereader_tui_database_read_history(database, &result, error) ||
        (remove_missing && !mereader_tui_database_delete_missing(database, &result, error))) {
        mereader_tui_history_free(&result);
        return false;
    }

    *history = result;
    return true;
}

static bool mereader_tui_database_bind_optional_text(MereaderTuiDatabase *database, sqlite3_stmt *statement, int parameter,
                                             const char *value, const char *operation, MereaderTuiError *error) {
    int status = value == nullptr ? sqlite3_bind_null(statement, parameter)
                                  : sqlite3_bind_text(statement, parameter, value, -1, SQLITE_TRANSIENT);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, operation, status, error);
        return false;
    }
    return true;
}

bool mereader_tui_database_save_progress(MereaderTuiDatabase *database, const MereaderTuiHistoryEntry *entry, MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error) || entry == nullptr || entry->filepath == nullptr ||
        entry->filepath[0] == '\0' || !isfinite(entry->reading_progress)) {
        if (!mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid reading progress entry");
        }
        return false;
    }

    static const char sql[] =
        "INSERT INTO \"reading_history\" "
        "(\"filepath\", \"title\", \"author\", \"reading_progress\", \"last_read\") "
        "VALUES (?1, ?2, ?3, ?4, COALESCE(?5, CURRENT_TIMESTAMP)) "
        "ON CONFLICT(\"filepath\") DO UPDATE SET "
        "\"title\" = excluded.\"title\", "
        "\"author\" = excluded.\"author\", "
        "\"reading_progress\" = excluded.\"reading_progress\", "
        "\"last_read\" = excluded.\"last_read\"";

    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle, sql, -1, &statement, nullptr);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not prepare reading progress update", status, error);
        return false;
    }

    bool bound = mereader_tui_database_bind_optional_text(database, statement, 1, entry->filepath,
                                                   "could not bind reading history path", error) &&
                 mereader_tui_database_bind_optional_text(database, statement, 2, entry->title,
                                                   "could not bind reading history title", error) &&
                 mereader_tui_database_bind_optional_text(database, statement, 3, entry->author,
                                                   "could not bind reading history author", error);
    if (bound) {
        status = sqlite3_bind_double(statement, 4, entry->reading_progress);
        if (status != SQLITE_OK) {
            mereader_tui_database_set_error(database, "could not bind reading progress", status, error);
            bound = false;
        }
    }
    if (bound && !mereader_tui_database_bind_optional_text(database, statement, 5, entry->last_read,
                                                    "could not bind last-read time", error)) {
        bound = false;
    }
    if (!bound) {
        (void)sqlite3_finalize(statement);
        return false;
    }

    status = sqlite3_step(statement);
    if (status != SQLITE_DONE) {
        mereader_tui_database_set_error(database, "could not save reading progress", status, error);
        (void)sqlite3_finalize(statement);
        return false;
    }
    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish reading progress update", status, error);
        return false;
    }
    return true;
}

bool mereader_tui_database_remove_history(MereaderTuiDatabase *database, const char *filepath, MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error) || filepath == nullptr || filepath[0] == '\0') {
        if (!mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "history path is empty");
        }
        return false;
    }

    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle,
                                    "DELETE FROM \"reading_history\" WHERE \"filepath\" = ?1", -1,
                                    &statement, nullptr);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not prepare history removal", status, error);
        return false;
    }
    status = sqlite3_bind_text(statement, 1, filepath, -1, SQLITE_TRANSIENT);
    if (status == SQLITE_OK) {
        status = sqlite3_step(statement);
    }
    if (status != SQLITE_DONE) {
        mereader_tui_database_set_error(database, "could not remove history", status, error);
        (void)sqlite3_finalize(statement);
        return false;
    }
    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish history removal", status, error);
        return false;
    }
    return true;
}

void mereader_tui_format_preferences_free(MereaderTuiFormatPreferences *preferences) {
    if (preferences == nullptr) {
        return;
    }
    for (size_t index = 0U; index < preferences->length; ++index) {
        free(preferences->items[index].book_key);
        free(preferences->items[index].relative_path);
    }
    free(preferences->items);
    *preferences = (MereaderTuiFormatPreferences){0};
}

bool mereader_tui_database_format_preferences(MereaderTuiDatabase *database, const char *library_root,
                                      MereaderTuiFormatPreferences *preferences, MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error) || library_root == nullptr || library_root[0] == '\0' ||
        preferences == nullptr || preferences->items != nullptr || preferences->length != 0U) {
        if (!mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid format preference query");
        }
        return false;
    }

    static const char sql[] = "SELECT \"book_key\", \"relative_path\" FROM "
                              "\"library_format_preferences\" "
                              "WHERE \"library_root\" = ?1 ORDER BY \"book_key\"";
    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle, sql, -1, &statement, nullptr);
    if (status == SQLITE_OK) {
        status = sqlite3_bind_text(statement, 1, library_root, -1, SQLITE_TRANSIENT);
    }
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not prepare format preference query", status, error);
        (void)sqlite3_finalize(statement);
        return false;
    }

    MereaderTuiFormatPreferences result = {0};
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        MereaderTuiFormatPreference item = {0};
        if (!mereader_tui_database_column_string(statement, 0, true, &item.book_key, error) ||
            !mereader_tui_database_column_string(statement, 1, true, &item.relative_path, error)) {
            free(item.book_key);
            free(item.relative_path);
            mereader_tui_format_preferences_free(&result);
            (void)sqlite3_finalize(statement);
            return false;
        }
        MereaderTuiFormatPreference *items =
            mereader_tui_array_reserve(result.items, &result.capacity, sizeof(*result.items), result.length + 1U, error);
        if (items == nullptr) {
            free(item.book_key);
            free(item.relative_path);
            mereader_tui_format_preferences_free(&result);
            (void)sqlite3_finalize(statement);
            return false;
        }
        result.items = items;
        result.items[result.length++] = item;
    }
    if (status != SQLITE_DONE) {
        mereader_tui_database_set_error(database, "could not read format preferences", status, error);
        mereader_tui_format_preferences_free(&result);
        (void)sqlite3_finalize(statement);
        return false;
    }
    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish format preference query", status, error);
        mereader_tui_format_preferences_free(&result);
        return false;
    }
    *preferences = result;
    return true;
}

bool mereader_tui_database_save_format_preference(MereaderTuiDatabase *database, const char *library_root, const char *book_key,
                                          const char *relative_path, MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error) || library_root == nullptr || library_root[0] == '\0' ||
        book_key == nullptr || book_key[0] == '\0' || relative_path == nullptr || relative_path[0] == '\0') {
        if (!mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid format preference");
        }
        return false;
    }
    static const char sql[] = "INSERT INTO \"library_format_preferences\" "
                              "(\"library_root\", \"book_key\", \"relative_path\") "
                              "VALUES (?1, ?2, ?3) ON CONFLICT(\"library_root\", "
                              "\"book_key\") DO UPDATE SET "
                              "\"relative_path\" = excluded.\"relative_path\"";
    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle, sql, -1, &statement, nullptr);
    if (status == SQLITE_OK) {
        status = sqlite3_bind_text(statement, 1, library_root, -1, SQLITE_TRANSIENT);
    }
    if (status == SQLITE_OK) {
        status = sqlite3_bind_text(statement, 2, book_key, -1, SQLITE_TRANSIENT);
    }
    if (status == SQLITE_OK) {
        status = sqlite3_bind_text(statement, 3, relative_path, -1, SQLITE_TRANSIENT);
    }
    if (status == SQLITE_OK) {
        status = sqlite3_step(statement);
    }
    if (status != SQLITE_DONE) {
        mereader_tui_database_set_error(database, "could not save format preference", status, error);
        (void)sqlite3_finalize(statement);
        return false;
    }
    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish format preference update", status, error);
        return false;
    }
    return true;
}

const MereaderTuiHistoryEntry *mereader_tui_history_nth(const MereaderTuiHistory *history, size_t one_based_index) {
    if (history == nullptr || one_based_index == 0U || one_based_index > history->length) {
        return nullptr;
    }
    return &history->items[one_based_index - 1U];
}

static unsigned char mereader_tui_history_fold(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static size_t mereader_tui_history_match_score(const char *text, const char *query) {
    if (text == nullptr || query == nullptr || text[0] == '\0' || query[0] == '\0') {
        return 0U;
    }

    size_t text_length = strlen(text);
    size_t query_length = strlen(query);
    if (query_length == SIZE_MAX || text_length > SIZE_MAX - query_length) {
        return 0U;
    }

    MereaderTuiError ignored = {0};
    size_t *row = mereader_tui_reallocarray(nullptr, query_length + 1U, sizeof(*row), &ignored);
    if (row == nullptr) {
        return 0U;
    }
    memset(row, 0, (query_length + 1U) * sizeof(*row));

    for (size_t text_index = 1U; text_index <= text_length; ++text_index) {
        size_t diagonal = 0U;
        for (size_t query_index = 1U; query_index <= query_length; ++query_index) {
            size_t previous = row[query_index];
            if (mereader_tui_history_fold((unsigned char)text[text_index - 1U]) ==
                mereader_tui_history_fold((unsigned char)query[query_index - 1U])) {
                row[query_index] = diagonal + 1U;
            } else if (row[query_index - 1U] > row[query_index]) {
                row[query_index] = row[query_index - 1U];
            }
            diagonal = previous;
        }
    }

    size_t common = row[query_length];
    free(row);
    size_t total = text_length + query_length;
    if (common > SIZE_MAX / 20000U) {
        return 10000U;
    }
    return common * 20000U / total;
}

static size_t mereader_tui_history_metadata_score(const MereaderTuiHistoryEntry *entry, const char *query) {
    MereaderTuiError ignored = {0};
    MereaderTuiString metadata = {0};
    bool built = mereader_tui_string_append(&metadata, entry->title == nullptr ? "None" : entry->title, &ignored) &&
                 mereader_tui_string_append_char(&metadata, ' ', &ignored) &&
                 mereader_tui_string_append(&metadata, entry->author == nullptr ? "None" : entry->author, &ignored);
    if (!built) {
        mereader_tui_string_free(&metadata);
        return 0U;
    }
    size_t score = mereader_tui_history_match_score(metadata.data, query);
    mereader_tui_string_free(&metadata);
    return score;
}

const MereaderTuiHistoryEntry *mereader_tui_history_best_match(const MereaderTuiHistory *history, const char *query) {
    if (history == nullptr || query == nullptr || query[0] == '\0') {
        return nullptr;
    }

    const MereaderTuiHistoryEntry *best = nullptr;
    size_t best_score = 0U;
    for (size_t index = 0U; index < history->length; ++index) {
        const MereaderTuiHistoryEntry *entry = &history->items[index];
        size_t score = mereader_tui_history_match_score(entry->filepath, query);
        size_t metadata_score = mereader_tui_history_metadata_score(entry, query);
        if (metadata_score > score) {
            score = metadata_score;
        }
        if (score > 1000U && score > best_score) {
            best = entry;
            best_score = score;
        }
    }
    return best;
}

void mereader_tui_bookmarks_free(MereaderTuiBookmarks *bookmarks) {
    if (bookmarks == nullptr) {
        return;
    }
    for (size_t index = 0U; index < bookmarks->length; ++index) {
        free(bookmarks->items[index].created_at);
    }
    free(bookmarks->items);
    *bookmarks = (MereaderTuiBookmarks){0};
}

bool mereader_tui_database_add_bookmark(MereaderTuiDatabase *database, const char *filepath, double reading_progress,
                                MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error) || filepath == nullptr || filepath[0] == '\0' ||
        !isfinite(reading_progress) || reading_progress < 0.0 || reading_progress > 1.0) {
        if (!mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid bookmark");
        }
        return false;
    }

    static const char sql[] =
        "INSERT INTO \"bookmarks\" (\"filepath\", \"reading_progress\") VALUES (?1, ?2) "
        "ON CONFLICT(\"filepath\", \"reading_progress\") DO NOTHING";
    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle, sql, -1, &statement, nullptr);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not prepare bookmark update", status, error);
        return false;
    }
    status = sqlite3_bind_text(statement, 1, filepath, -1, SQLITE_TRANSIENT);
    if (status == SQLITE_OK) {
        status = sqlite3_bind_double(statement, 2, reading_progress);
    }
    if (status == SQLITE_OK) {
        status = sqlite3_step(statement);
    }
    if (status != SQLITE_DONE) {
        mereader_tui_database_set_error(database, "could not save bookmark", status, error);
        (void)sqlite3_finalize(statement);
        return false;
    }
    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish bookmark update", status, error);
        return false;
    }
    return true;
}

bool mereader_tui_database_bookmarks(MereaderTuiDatabase *database, const char *filepath, MereaderTuiBookmarks *bookmarks,
                             MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error) || filepath == nullptr || filepath[0] == '\0' || bookmarks == nullptr) {
        if (!mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid bookmark query");
        }
        return false;
    }
    if (bookmarks->items != nullptr || bookmarks->length != 0U || bookmarks->capacity != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "bookmark output is not empty");
        return false;
    }

    static const char sql[] =
        "SELECT \"id\", \"reading_progress\", \"created_at\" FROM \"bookmarks\" "
        "WHERE \"filepath\" = ?1 ORDER BY \"reading_progress\", \"id\"";
    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle, sql, -1, &statement, nullptr);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not prepare bookmark query", status, error);
        return false;
    }
    status = sqlite3_bind_text(statement, 1, filepath, -1, SQLITE_TRANSIENT);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not bind bookmark path", status, error);
        (void)sqlite3_finalize(statement);
        return false;
    }

    MereaderTuiBookmarks result = {0};
    for (;;) {
        status = sqlite3_step(statement);
        if (status == SQLITE_DONE) {
            break;
        }
        if (status != SQLITE_ROW) {
            mereader_tui_database_set_error(database, "could not read bookmarks", status, error);
            (void)sqlite3_finalize(statement);
            mereader_tui_bookmarks_free(&result);
            return false;
        }

        MereaderTuiBookmark bookmark = {
            .id = sqlite3_column_int64(statement, 0),
            .reading_progress = sqlite3_column_double(statement, 1),
        };
        if (bookmark.id <= 0 || !isfinite(bookmark.reading_progress) || bookmark.reading_progress < 0.0 ||
            bookmark.reading_progress > 1.0 ||
            !mereader_tui_database_column_string(statement, 2, true, &bookmark.created_at, error)) {
            free(bookmark.created_at);
            (void)sqlite3_finalize(statement);
            mereader_tui_bookmarks_free(&result);
            if (!mereader_tui_error_is_set(error)) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_DATABASE, "database contains an invalid bookmark");
            }
            return false;
        }

        MereaderTuiError reserve_error = {0};
        MereaderTuiBookmark *items = mereader_tui_array_reserve(result.items, &result.capacity, sizeof(*result.items),
                                                 result.length + 1U, &reserve_error);
        if (mereader_tui_error_is_set(&reserve_error)) {
            if (error != nullptr) {
                *error = reserve_error;
            }
            free(bookmark.created_at);
            (void)sqlite3_finalize(statement);
            mereader_tui_bookmarks_free(&result);
            return false;
        }
        result.items = items;
        result.items[result.length++] = bookmark;
    }

    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish bookmark query", status, error);
        mereader_tui_bookmarks_free(&result);
        return false;
    }
    *bookmarks = result;
    return true;
}

bool mereader_tui_database_remove_bookmark(MereaderTuiDatabase *database, const char *filepath, int64_t id, MereaderTuiError *error) {
    if (!mereader_tui_database_ready(database, error) || filepath == nullptr || filepath[0] == '\0' || id <= 0) {
        if (!mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid bookmark removal");
        }
        return false;
    }

    sqlite3_stmt *statement = nullptr;
    int status = sqlite3_prepare_v2(database->handle,
                                    "DELETE FROM \"bookmarks\" WHERE \"filepath\" = ?1 AND \"id\" = ?2", -1,
                                    &statement, nullptr);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not prepare bookmark removal", status, error);
        return false;
    }
    status = sqlite3_bind_text(statement, 1, filepath, -1, SQLITE_TRANSIENT);
    if (status == SQLITE_OK) {
        status = sqlite3_bind_int64(statement, 2, id);
    }
    if (status == SQLITE_OK) {
        status = sqlite3_step(statement);
    }
    if (status != SQLITE_DONE) {
        mereader_tui_database_set_error(database, "could not remove bookmark", status, error);
        (void)sqlite3_finalize(statement);
        return false;
    }
    status = sqlite3_finalize(statement);
    if (status != SQLITE_OK) {
        mereader_tui_database_set_error(database, "could not finish bookmark removal", status, error);
        return false;
    }
    return true;
}
