#pragma once

#include "mereader-tui/common.h"

#include <sqlite3.h>

typedef struct MereaderTuiDatabase {
    sqlite3 *handle;
    char *path;
} MereaderTuiDatabase;

typedef struct MereaderTuiHistoryEntry {
    char *filepath;
    char *title;
    char *author;
    double reading_progress;
    char *last_read;
} MereaderTuiHistoryEntry;

typedef struct MereaderTuiHistory {
    MereaderTuiHistoryEntry *items;
    size_t length;
    size_t capacity;
} MereaderTuiHistory;

typedef struct MereaderTuiBookmark {
    int64_t id;
    double reading_progress;
    char *created_at;
} MereaderTuiBookmark;

typedef struct MereaderTuiBookmarks {
    MereaderTuiBookmark *items;
    size_t length;
    size_t capacity;
} MereaderTuiBookmarks;

typedef struct MereaderTuiFormatPreference {
    char *book_key;
    char *relative_path;
} MereaderTuiFormatPreference;

typedef struct MereaderTuiFormatPreferences {
    MereaderTuiFormatPreference *items;
    size_t length;
    size_t capacity;
} MereaderTuiFormatPreferences;

[[nodiscard]] bool mereader_tui_database_open(MereaderTuiDatabase *database, const char *path, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_database_open_default(MereaderTuiDatabase *database, MereaderTuiError *error);
void mereader_tui_database_close(MereaderTuiDatabase *database);
[[nodiscard]] bool mereader_tui_database_migrate(MereaderTuiDatabase *database, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_database_history(MereaderTuiDatabase *database, bool remove_missing, MereaderTuiHistory *history,
                                         MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_database_save_progress(MereaderTuiDatabase *database, const MereaderTuiHistoryEntry *entry,
                                               MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_database_remove_history(MereaderTuiDatabase *database, const char *filepath, MereaderTuiError *error);
void mereader_tui_history_free(MereaderTuiHistory *history);
[[nodiscard]] const MereaderTuiHistoryEntry *mereader_tui_history_nth(const MereaderTuiHistory *history, size_t one_based_index);
[[nodiscard]] const MereaderTuiHistoryEntry *mereader_tui_history_best_match(const MereaderTuiHistory *history, const char *query);
[[nodiscard]] bool mereader_tui_database_add_bookmark(MereaderTuiDatabase *database, const char *filepath, double reading_progress,
                                              MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_database_bookmarks(MereaderTuiDatabase *database, const char *filepath, MereaderTuiBookmarks *bookmarks,
                                           MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_database_remove_bookmark(MereaderTuiDatabase *database, const char *filepath, int64_t id,
                                                 MereaderTuiError *error);
void mereader_tui_bookmarks_free(MereaderTuiBookmarks *bookmarks);
[[nodiscard]] bool mereader_tui_database_format_preferences(MereaderTuiDatabase *database, const char *library_root,
                                                    MereaderTuiFormatPreferences *preferences, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_database_save_format_preference(MereaderTuiDatabase *database, const char *library_root,
                                                        const char *book_key, const char *relative_path,
                                                        MereaderTuiError *error);
void mereader_tui_format_preferences_free(MereaderTuiFormatPreferences *preferences);
