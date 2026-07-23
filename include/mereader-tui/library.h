#pragma once

#include "mereader-tui/database.h"

#include <time.h>

typedef enum MereaderTuiLibrarySort : uint8_t {
    MEREADER_TUI_LIBRARY_SORT_RECENT = 0,
    MEREADER_TUI_LIBRARY_SORT_TITLE,
    MEREADER_TUI_LIBRARY_SORT_AUTHOR,
    MEREADER_TUI_LIBRARY_SORT_RELEVANCE,
} MereaderTuiLibrarySort;

typedef struct MereaderTuiLibraryRow {
    const MereaderTuiHistoryEntry *entry;
    char *title;
    char *author;
    char *path;
    char *title_key;
    char *author_key;
    char *path_key;
} MereaderTuiLibraryRow;

typedef struct MereaderTuiLibraryView {
    MereaderTuiLibraryRow *rows;
    size_t length;
    MereaderTuiLibrarySort sort;
} MereaderTuiLibraryView;

[[nodiscard]] char *mereader_tui_library_sanitize_text(const char *text, size_t max_columns, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_library_parse_timestamp(const char *value, struct timespec *timestamp);
[[nodiscard]] bool mereader_tui_library_view_build(MereaderTuiLibraryView *view, const MereaderTuiHistory *history, const char *filter,
                                           MereaderTuiLibrarySort sort, MereaderTuiError *error);
void mereader_tui_library_view_free(MereaderTuiLibraryView *view);
[[nodiscard]] size_t mereader_tui_library_preserve_selection(const MereaderTuiLibraryView *view, const char *filepath,
                                                      size_t fallback_index);
[[nodiscard]] MereaderTuiLibrarySort mereader_tui_library_sort_next(MereaderTuiLibrarySort sort);
[[nodiscard]] const char *mereader_tui_library_sort_name(MereaderTuiLibrarySort sort);
