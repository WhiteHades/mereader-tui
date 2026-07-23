#pragma once

#include "mereader-tui/database.h"
#include "mereader-tui/search.h"

typedef struct MereaderTuiCatalogFormat {
    char *path;
    char *relative_path;
    char *name;
    double reading_progress;
    char *last_read;
} MereaderTuiCatalogFormat;

typedef struct MereaderTuiCatalogBook {
    char *title;
    char *author;
    char *directory;
    char *group_key;
    MereaderTuiCatalogFormat *formats;
    size_t format_count;
    size_t format_capacity;
    size_t preferred_format;
} MereaderTuiCatalogBook;

typedef struct MereaderTuiCatalog {
    MereaderTuiSearchIndex search;
    MereaderTuiCatalogBook *books;
    size_t length;
    size_t capacity;
    bool calibre;
} MereaderTuiCatalog;

typedef struct MereaderTuiCatalogMatches {
    size_t *book_indices;
    size_t length;
} MereaderTuiCatalogMatches;

[[nodiscard]] bool mereader_tui_catalog_open(MereaderTuiCatalog *catalog, const char *root, const MereaderTuiHistory *history, bool watch,
                                     MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_catalog_refresh(MereaderTuiCatalog *catalog, const MereaderTuiHistory *history, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_catalog_update_progress(MereaderTuiCatalog *catalog, const MereaderTuiHistory *history, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_catalog_is_open(const MereaderTuiCatalog *catalog);
void mereader_tui_catalog_close(MereaderTuiCatalog *catalog);
[[nodiscard]] bool mereader_tui_catalog_search(MereaderTuiCatalog *catalog, const char *query, MereaderTuiCatalogMatches *matches,
                                       MereaderTuiError *error);
void mereader_tui_catalog_matches_free(MereaderTuiCatalogMatches *matches);
[[nodiscard]] const MereaderTuiCatalogFormat *mereader_tui_catalog_preferred_format(const MereaderTuiCatalogBook *book);
[[nodiscard]] bool mereader_tui_catalog_apply_format_preferences(MereaderTuiCatalog *catalog,
                                                         const MereaderTuiFormatPreferences *preferences, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_catalog_prefer_path(MereaderTuiCatalog *catalog, const char *path, const char **book_key,
                                            const char **relative_path);
