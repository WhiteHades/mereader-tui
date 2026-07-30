#include "mereader-tui/search.h"

#include "fff.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool search_result_error(FffResult *result, const char *operation, MereaderTuiError *error) {
    if (result == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "%s: fff returned no result", operation);
        return true;
    }
    if (!result->success) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "%s: %s", operation,
                       result->error == NULL ? "fff failed" : result->error);
        fff_free_result(result);
        return true;
    }
    return false;
}

static bool search_index_create(MereaderTuiSearchIndex *index, const char *root, bool watch, bool allow_broad_root,
                                MereaderTuiError *error) {
    if (index == NULL || root == NULL || root[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid search index root");
        return false;
    }
    if (index->handle != NULL || index->root != NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "search index output is not empty");
        return false;
    }

    char *stored_root = mereader_tui_realpath(root, error);
    if (stored_root == NULL) {
        return false;
    }
    const FffCreateOptions options = {
        .version = FFF_CREATE_OPTIONS_VERSION,
        .base_path = stored_root,
        .frecency_db_path = NULL,
        .history_db_path = NULL,
        .enable_mmap_cache = false,
        .enable_content_indexing = false,
        .watch = watch,
        .ai_mode = false,
        .log_file_path = NULL,
        .log_level = NULL,
        .cache_budget_max_files = 0U,
        .cache_budget_max_bytes = 0U,
        .cache_budget_max_file_size = 0U,
        .enable_fs_root_scanning = allow_broad_root,
        .enable_home_dir_scanning = allow_broad_root,
        .follow_symlinks = false,
    };
    FffResult *created = fff_create_instance_with(&options);
    if (search_result_error(created, "could not create library index", error)) {
        free(stored_root);
        return false;
    }
    void *handle = created->handle;
    fff_free_result(created);
    if (handle == NULL) {
        free(stored_root);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not create library index: fff returned no handle");
        return false;
    }

    *index = (MereaderTuiSearchIndex){.handle = handle, .root = stored_root};
    return true;
}

bool mereader_tui_search_index_start(MereaderTuiSearchIndex *index, const char *root, MereaderTuiError *error) {
    return search_index_create(index, root, false, true, error);
}

bool mereader_tui_search_index_open(MereaderTuiSearchIndex *index, const char *root, bool watch, MereaderTuiError *error) {
    if (!search_index_create(index, root, watch, false, error)) {
        return false;
    }
    FffResult *waited = fff_wait_for_scan(index->handle, 30000U);
    if (search_result_error(waited, "could not scan library", error)) {
        mereader_tui_search_index_close(index);
        return false;
    }
    const bool complete = waited->int_value != 0;
    fff_free_result(waited);
    if (!complete) {
        mereader_tui_search_index_close(index);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not scan library: fff timed out after 30 seconds");
        return false;
    }
    return true;
}

bool mereader_tui_search_index_scanning(MereaderTuiSearchIndex *index, bool *scanning, MereaderTuiError *error) {
    if (index == NULL || index->handle == NULL || scanning == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid search index progress request");
        return false;
    }
    FffResult *progress_result = fff_get_scan_progress(index->handle);
    if (search_result_error(progress_result, "could not inspect library index", error)) {
        return false;
    }
    FffScanProgress *progress = progress_result->handle;
    fff_free_result(progress_result);
    if (progress == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                       "could not inspect library index: fff returned no progress");
        return false;
    }
    *scanning = progress->is_scanning;
    fff_free_scan_progress(progress);
    return true;
}

void mereader_tui_search_index_close(MereaderTuiSearchIndex *index) {
    if (index == NULL) {
        return;
    }
    fff_destroy(index->handle);
    free(index->root);
    *index = (MereaderTuiSearchIndex){0};
}

bool mereader_tui_search_index_refresh(MereaderTuiSearchIndex *index, MereaderTuiError *error) {
    if (index == NULL || index->handle == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "search index is not open");
        return false;
    }
    FffResult *scanned = fff_scan_files(index->handle);
    if (search_result_error(scanned, "could not refresh library index", error)) {
        return false;
    }
    fff_free_result(scanned);

    FffResult *waited = fff_wait_for_scan(index->handle, 30000U);
    if (search_result_error(waited, "could not refresh library index", error)) {
        return false;
    }
    const bool complete = waited->int_value != 0;
    fff_free_result(waited);
    if (!complete) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not refresh library index: fff timed out after 30 seconds");
    }
    return complete;
}

void mereader_tui_search_files_free(MereaderTuiSearchFiles *files) {
    if (files == NULL) {
        return;
    }
    for (size_t index = 0U; index < files->length; ++index) {
        free(files->items[index].relative_path);
    }
    free(files->items);
    *files = (MereaderTuiSearchFiles){0};
}

void mereader_tui_search_directories_free(MereaderTuiSearchDirectories *directories) {
    if (directories == NULL) {
        return;
    }
    for (size_t index = 0U; index < directories->length; ++index) {
        free(directories->items[index].relative_path);
    }
    free(directories->items);
    *directories = (MereaderTuiSearchDirectories){0};
}

bool mereader_tui_search_files(MereaderTuiSearchIndex *index, const char *query, size_t offset, size_t limit,
                       MereaderTuiSearchFiles *files, MereaderTuiError *error) {
    if (index == NULL || index->handle == NULL || query == NULL || files == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid search request");
        return false;
    }
    if (files->items != NULL || files->length != 0U || files->total != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "search output is not empty");
        return false;
    }
    if (offset > UINT32_MAX || limit == 0U || limit > UINT32_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "search page is out of range");
        return false;
    }

    FffResult *searched = fff_search(index->handle, query, NULL, 0U, (uint32_t)offset, (uint32_t)limit, 0, 0U);
    if (search_result_error(searched, "could not search library", error)) {
        return false;
    }
    FffSearchResult *payload = searched->handle;
    fff_free_result(searched);
    if (payload == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not search library: fff returned no payload");
        return false;
    }

    const size_t count = payload->count;
    MereaderTuiSearchFile *items = count == 0U ? NULL : mereader_tui_reallocarray(NULL, count, sizeof(*items), error);
    if (count > 0U && items == NULL) {
        fff_free_search_result(payload);
        return false;
    }
    if (count > 0U) {
        memset(items, 0, count * sizeof(*items));
    }
    size_t copied = 0U;
    for (; copied < count; ++copied) {
        const FffFileItem *item = fff_search_result_get_item(payload, (uint32_t)copied);
        const FffScore *score = fff_search_result_get_score(payload, (uint32_t)copied);
        if (item == NULL || item->relative_path == NULL) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL, "could not search library: fff returned an invalid item");
            break;
        }
        items[copied].relative_path = mereader_tui_strdup(item->relative_path, error);
        items[copied].score = score == NULL ? 0 : score->total;
        if (items[copied].relative_path == NULL) {
            break;
        }
    }
    const size_t total = payload->total_matched;
    fff_free_search_result(payload);
    if (copied != count) {
        MereaderTuiSearchFiles partial = {.items = items, .length = copied};
        mereader_tui_search_files_free(&partial);
        return false;
    }

    *files = (MereaderTuiSearchFiles){.items = items, .length = count, .total = total};
    return true;
}

bool mereader_tui_search_directories(MereaderTuiSearchIndex *index, const char *query, size_t offset, size_t limit,
                             MereaderTuiSearchDirectories *directories, MereaderTuiError *error) {
    if (index == NULL || index->handle == NULL || query == NULL || directories == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid directory search request");
        return false;
    }
    if (directories->items != NULL || directories->length != 0U || directories->total != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "directory search output is not empty");
        return false;
    }
    if (offset > UINT32_MAX || limit == 0U || limit > UINT32_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "directory search page is out of range");
        return false;
    }

    FffResult *searched =
        fff_search_directories(index->handle, query, NULL, 0U, (uint32_t)offset, (uint32_t)limit);
    if (search_result_error(searched, "could not search library folders", error)) {
        return false;
    }
    FffDirSearchResult *payload = searched->handle;
    fff_free_result(searched);
    if (payload == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                       "could not search library folders: fff returned no payload");
        return false;
    }

    const size_t count = payload->count;
    MereaderTuiSearchDirectory *items =
        count == 0U ? NULL : mereader_tui_reallocarray(NULL, count, sizeof(*items), error);
    if (count > 0U && items == NULL) {
        fff_free_dir_search_result(payload);
        return false;
    }
    if (count > 0U) {
        memset(items, 0, count * sizeof(*items));
    }
    size_t copied = 0U;
    for (; copied < count; ++copied) {
        const FffDirItem *item = fff_dir_search_result_get_item(payload, (uint32_t)copied);
        const FffScore *score = fff_dir_search_result_get_score(payload, (uint32_t)copied);
        if (item == NULL || item->relative_path == NULL) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_EXTERNAL,
                           "could not search library folders: fff returned an invalid item");
            break;
        }
        items[copied].relative_path = mereader_tui_strdup(item->relative_path, error);
        items[copied].score = score == NULL ? 0 : score->total;
        if (items[copied].relative_path == NULL) {
            break;
        }
        size_t path_length = strlen(items[copied].relative_path);
        while (path_length > 1U && items[copied].relative_path[path_length - 1U] == '/') {
            items[copied].relative_path[--path_length] = '\0';
        }
    }
    const size_t total = payload->total_matched;
    fff_free_dir_search_result(payload);
    if (copied != count) {
        MereaderTuiSearchDirectories partial = {.items = items, .length = copied};
        mereader_tui_search_directories_free(&partial);
        return false;
    }

    *directories =
        (MereaderTuiSearchDirectories){.items = items, .length = count, .total = total};
    return true;
}
