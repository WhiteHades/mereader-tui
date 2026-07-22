#include "baca/search.h"

#include "fff.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool search_result_error(FffResult *result, const char *operation, BacaError *error) {
    if (result == NULL) {
        baca_error_set(error, BACA_ERROR_EXTERNAL, "%s: fff returned no result", operation);
        return true;
    }
    if (!result->success) {
        baca_error_set(error, BACA_ERROR_EXTERNAL, "%s: %s", operation,
                       result->error == NULL ? "fff failed" : result->error);
        fff_free_result(result);
        return true;
    }
    return false;
}

bool baca_search_index_open(BacaSearchIndex *index, const char *root, bool watch, BacaError *error) {
    if (index == NULL || root == NULL || root[0] == '\0') {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid search index root");
        return false;
    }
    if (index->handle != NULL || index->root != NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "search index output is not empty");
        return false;
    }

    char *stored_root = baca_realpath(root, error);
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
        .enable_fs_root_scanning = false,
        .enable_home_dir_scanning = false,
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
        baca_error_set(error, BACA_ERROR_EXTERNAL, "could not create library index: fff returned no handle");
        return false;
    }

    FffResult *waited = fff_wait_for_scan(handle, 30000U);
    if (search_result_error(waited, "could not scan library", error)) {
        fff_destroy(handle);
        free(stored_root);
        return false;
    }
    const bool complete = waited->int_value != 0;
    fff_free_result(waited);
    if (!complete) {
        fff_destroy(handle);
        free(stored_root);
        baca_error_set(error, BACA_ERROR_EXTERNAL, "could not scan library: fff timed out after 30 seconds");
        return false;
    }

    *index = (BacaSearchIndex){.handle = handle, .root = stored_root};
    return true;
}

void baca_search_index_close(BacaSearchIndex *index) {
    if (index == NULL) {
        return;
    }
    fff_destroy(index->handle);
    free(index->root);
    *index = (BacaSearchIndex){0};
}

bool baca_search_index_refresh(BacaSearchIndex *index, BacaError *error) {
    if (index == NULL || index->handle == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "search index is not open");
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
        baca_error_set(error, BACA_ERROR_EXTERNAL, "could not refresh library index: fff timed out after 30 seconds");
    }
    return complete;
}

void baca_search_files_free(BacaSearchFiles *files) {
    if (files == NULL) {
        return;
    }
    for (size_t index = 0U; index < files->length; ++index) {
        free(files->items[index].relative_path);
    }
    free(files->items);
    *files = (BacaSearchFiles){0};
}

bool baca_search_files(BacaSearchIndex *index, const char *query, size_t offset, size_t limit,
                       BacaSearchFiles *files, BacaError *error) {
    if (index == NULL || index->handle == NULL || query == NULL || files == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid search request");
        return false;
    }
    if (files->items != NULL || files->length != 0U || files->total != 0U) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "search output is not empty");
        return false;
    }
    if (offset > UINT32_MAX || limit == 0U || limit > UINT32_MAX) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "search page is out of range");
        return false;
    }

    FffResult *searched = fff_search(index->handle, query, NULL, 0U, (uint32_t)offset, (uint32_t)limit, 0, 0U);
    if (search_result_error(searched, "could not search library", error)) {
        return false;
    }
    FffSearchResult *payload = searched->handle;
    fff_free_result(searched);
    if (payload == NULL) {
        baca_error_set(error, BACA_ERROR_EXTERNAL, "could not search library: fff returned no payload");
        return false;
    }

    const size_t count = payload->count;
    BacaSearchFile *items = count == 0U ? NULL : baca_reallocarray(NULL, count, sizeof(*items), error);
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
            baca_error_set(error, BACA_ERROR_EXTERNAL, "could not search library: fff returned an invalid item");
            break;
        }
        items[copied].relative_path = baca_strdup(item->relative_path, error);
        items[copied].score = score == NULL ? 0 : score->total;
        if (items[copied].relative_path == NULL) {
            break;
        }
    }
    const size_t total = payload->total_matched;
    fff_free_search_result(payload);
    if (copied != count) {
        BacaSearchFiles partial = {.items = items, .length = copied};
        baca_search_files_free(&partial);
        return false;
    }

    *files = (BacaSearchFiles){.items = items, .length = count, .total = total};
    return true;
}
