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

typedef struct MereaderTuiSearchDirectory {
    char *relative_path;
    int score;
} MereaderTuiSearchDirectory;

typedef struct MereaderTuiSearchDirectories {
    MereaderTuiSearchDirectory *items;
    size_t length;
    size_t total;
} MereaderTuiSearchDirectories;

[[nodiscard]] bool mereader_tui_search_index_open(MereaderTuiSearchIndex *index, const char *root, bool watch, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_search_index_start(MereaderTuiSearchIndex *index, const char *root, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_search_index_scanning(MereaderTuiSearchIndex *index, bool *scanning, MereaderTuiError *error);
void mereader_tui_search_index_close(MereaderTuiSearchIndex *index);
[[nodiscard]] bool mereader_tui_search_index_refresh(MereaderTuiSearchIndex *index, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_search_files(MereaderTuiSearchIndex *index, const char *query, size_t offset, size_t limit,
                                     MereaderTuiSearchFiles *files, MereaderTuiError *error);
void mereader_tui_search_files_free(MereaderTuiSearchFiles *files);
[[nodiscard]] bool mereader_tui_search_directories(MereaderTuiSearchIndex *index, const char *query, size_t offset,
                                           size_t limit, MereaderTuiSearchDirectories *directories,
                                           MereaderTuiError *error);
void mereader_tui_search_directories_free(MereaderTuiSearchDirectories *directories);
