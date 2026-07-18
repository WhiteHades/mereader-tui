#pragma once

#include "baca/common.h"

#include <sqlite3.h>

typedef struct BacaDatabase {
    sqlite3 *handle;
    char *path;
} BacaDatabase;

typedef struct BacaHistoryEntry {
    char *filepath;
    char *title;
    char *author;
    double reading_progress;
    char *last_read;
} BacaHistoryEntry;

typedef struct BacaHistory {
    BacaHistoryEntry *items;
    size_t length;
    size_t capacity;
} BacaHistory;

[[nodiscard]] bool baca_database_open(BacaDatabase *database, const char *path, BacaError *error);
[[nodiscard]] bool baca_database_open_default(BacaDatabase *database, BacaError *error);
void baca_database_close(BacaDatabase *database);
[[nodiscard]] bool baca_database_migrate(BacaDatabase *database, BacaError *error);
[[nodiscard]] bool baca_database_history(BacaDatabase *database, bool remove_missing, BacaHistory *history,
                                         BacaError *error);
[[nodiscard]] bool baca_database_save_progress(BacaDatabase *database, const BacaHistoryEntry *entry,
                                               BacaError *error);
[[nodiscard]] bool baca_database_remove_history(BacaDatabase *database, const char *filepath, BacaError *error);
void baca_history_free(BacaHistory *history);
[[nodiscard]] const BacaHistoryEntry *baca_history_nth(const BacaHistory *history, size_t one_based_index);
[[nodiscard]] const BacaHistoryEntry *baca_history_best_match(const BacaHistory *history, const char *query);
