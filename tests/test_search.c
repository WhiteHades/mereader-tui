#include "test_support.h"

#include "baca/search.h"

#include <stdlib.h>

static BacaTestResult test_index_search_refresh_and_ownership(void) {
    TEST_ASSERT(baca_test_write_text("search/alpha-book.epub", "alpha"));
    TEST_ASSERT(baca_test_write_text("search/nested/beta-manual.pdf", "beta"));
    char *root = baca_test_path("search");
    TEST_ASSERT(root != NULL);

    BacaSearchIndex index = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_search_index_open(&index, root, false, &error), "%s", error.message);
    TEST_ASSERT_STR(index.root, root);

    BacaSearchFiles files = {0};
    TEST_ASSERT_MSG(baca_search_files(&index, "alpha", 0U, 32U, &files, &error), "%s", error.message);
    TEST_ASSERT(files.length >= 1U);
    TEST_ASSERT_STR(files.items[0].relative_path, "alpha-book.epub");
    TEST_ASSERT(files.items[0].score > 0);
    baca_search_files_free(&files);

    TEST_ASSERT(baca_test_write_text("search/gamma-notes.txt", "gamma"));
    TEST_ASSERT_MSG(baca_search_index_refresh(&index, &error), "%s", error.message);
    TEST_ASSERT_MSG(baca_search_files(&index, "gamma", 0U, 32U, &files, &error), "%s", error.message);
    TEST_ASSERT(files.length >= 1U);
    TEST_ASSERT_STR(files.items[0].relative_path, "gamma-notes.txt");
    baca_search_files_free(&files);

    baca_search_index_close(&index);
    TEST_ASSERT(index.handle == NULL);
    TEST_ASSERT(index.root == NULL);
    free(root);
    return BACA_TEST_PASS;
}

static BacaTestResult test_invalid_requests(void) {
    BacaSearchIndex index = {0};
    BacaSearchFiles files = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_search_index_open(&index, "", false, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_error_clear(&error);
    TEST_ASSERT(!baca_search_files(&index, "query", 0U, 10U, &files, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    baca_search_index_close(&index);
    baca_search_files_free(&files);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_search_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "index_search_refresh_and_ownership", .function = test_index_search_refresh_and_ownership},
        {.name = "invalid_requests", .function = test_invalid_requests},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
