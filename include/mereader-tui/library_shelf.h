#pragma once

#include "mereader-tui/catalog.h"

typedef enum MereaderTuiLibraryShelfKind : uint8_t {
    MEREADER_TUI_LIBRARY_SHELF_AUTHORS = 0,
    MEREADER_TUI_LIBRARY_SHELF_BOOKS,
    MEREADER_TUI_LIBRARY_SHELF_ALL,
    MEREADER_TUI_LIBRARY_SHELF_FORMATS,
} MereaderTuiLibraryShelfKind;

typedef struct MereaderTuiLibraryShelfReference {
    size_t book_index;
    size_t format_index;
} MereaderTuiLibraryShelfReference;

typedef struct MereaderTuiLibraryShelf {
    MereaderTuiHistory entries;
    MereaderTuiLibraryShelfReference *references;
    size_t reference_capacity;
} MereaderTuiLibraryShelf;

[[nodiscard]] bool mereader_tui_library_shelf_build(MereaderTuiLibraryShelf *shelf, MereaderTuiCatalog *catalog,
                                             MereaderTuiLibraryShelfKind kind, const char *author,
                                             size_t format_book, const char *query, MereaderTuiError *error);
void mereader_tui_library_shelf_free(MereaderTuiLibraryShelf *shelf);
[[nodiscard]] const MereaderTuiLibraryShelfReference *mereader_tui_library_shelf_reference(
    const MereaderTuiLibraryShelf *shelf, const MereaderTuiHistoryEntry *entry);
