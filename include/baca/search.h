#pragma once

#include "baca/common.h"

typedef struct BacaSearchIndex {
    void *handle;
    char *root;
} BacaSearchIndex;

typedef struct BacaSearchFile {
    char *relative_path;
    int score;
} BacaSearchFile;

typedef struct BacaSearchFiles {
    BacaSearchFile *items;
    size_t length;
    size_t total;
} BacaSearchFiles;

[[nodiscard]] bool baca_search_index_open(BacaSearchIndex *index, const char *root, bool watch, BacaError *error);
void baca_search_index_close(BacaSearchIndex *index);
[[nodiscard]] bool baca_search_index_refresh(BacaSearchIndex *index, BacaError *error);
[[nodiscard]] bool baca_search_files(BacaSearchIndex *index, const char *query, size_t offset, size_t limit,
                                     BacaSearchFiles *files, BacaError *error);
void baca_search_files_free(BacaSearchFiles *files);
