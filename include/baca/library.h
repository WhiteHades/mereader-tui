#pragma once

#include "baca/database.h"

#include <time.h>

typedef enum BacaLibrarySort : uint8_t {
    BACA_LIBRARY_SORT_RECENT = 0,
    BACA_LIBRARY_SORT_TITLE,
    BACA_LIBRARY_SORT_AUTHOR,
    BACA_LIBRARY_SORT_RELEVANCE,
} BacaLibrarySort;

typedef struct BacaLibraryRow {
    const BacaHistoryEntry *entry;
    char *title;
    char *author;
    char *path;
    char *title_key;
    char *author_key;
    char *path_key;
} BacaLibraryRow;

typedef struct BacaLibraryView {
    BacaLibraryRow *rows;
    size_t length;
    BacaLibrarySort sort;
} BacaLibraryView;

[[nodiscard]] char *baca_library_sanitize_text(const char *text, size_t max_columns, BacaError *error);
[[nodiscard]] bool baca_library_parse_timestamp(const char *value, struct timespec *timestamp);
[[nodiscard]] bool baca_library_view_build(BacaLibraryView *view, const BacaHistory *history, const char *filter,
                                           BacaLibrarySort sort, BacaError *error);
void baca_library_view_free(BacaLibraryView *view);
[[nodiscard]] size_t baca_library_preserve_selection(const BacaLibraryView *view, const char *filepath,
                                                      size_t fallback_index);
[[nodiscard]] BacaLibrarySort baca_library_sort_next(BacaLibrarySort sort);
[[nodiscard]] const char *baca_library_sort_name(BacaLibrarySort sort);
