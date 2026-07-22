#pragma once

#include "baca/catalog.h"

typedef enum BacaLibraryShelfKind : uint8_t {
    BACA_LIBRARY_SHELF_AUTHORS = 0,
    BACA_LIBRARY_SHELF_BOOKS,
    BACA_LIBRARY_SHELF_ALL,
    BACA_LIBRARY_SHELF_FORMATS,
} BacaLibraryShelfKind;

typedef struct BacaLibraryShelfReference {
    size_t book_index;
    size_t format_index;
} BacaLibraryShelfReference;

typedef struct BacaLibraryShelf {
    BacaHistory entries;
    BacaLibraryShelfReference *references;
    size_t reference_capacity;
} BacaLibraryShelf;

[[nodiscard]] bool baca_library_shelf_build(BacaLibraryShelf *shelf, BacaCatalog *catalog,
                                             BacaLibraryShelfKind kind, const char *author,
                                             size_t format_book, const char *query, BacaError *error);
void baca_library_shelf_free(BacaLibraryShelf *shelf);
[[nodiscard]] const BacaLibraryShelfReference *baca_library_shelf_reference(
    const BacaLibraryShelf *shelf, const BacaHistoryEntry *entry);
