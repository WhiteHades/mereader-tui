#pragma once

#include "mereader-tui/common.h"

typedef struct MereaderTuiSearchIndex {
    void *handle;
    char *root;
} MereaderTuiSearchIndex;

typedef struct MereaderTuiSearchFile {
    char *relative_path;
    int score;
} MereaderTuiSearchFile;

typedef struct MereaderTuiSearchFiles {
    MereaderTuiSearchFile *items;
    size_t length;
    size_t total;
} MereaderTuiSearchFiles;

[[nodiscard]] bool mereader_tui_search_index_open(MereaderTuiSearchIndex *index, const char *root, bool watch, MereaderTuiError *error);
void mereader_tui_search_index_close(MereaderTuiSearchIndex *index);
[[nodiscard]] bool mereader_tui_search_index_refresh(MereaderTuiSearchIndex *index, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_search_files(MereaderTuiSearchIndex *index, const char *query, size_t offset, size_t limit,
                                     MereaderTuiSearchFiles *files, MereaderTuiError *error);
void mereader_tui_search_files_free(MereaderTuiSearchFiles *files);
