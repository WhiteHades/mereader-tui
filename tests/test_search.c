#include "test_support.h"

#include "mereader-tui/search.h"

#include <stdlib.h>

static MereaderTuiTestResult test_index_search_refresh_and_ownership(void) {
    TEST_ASSERT(mereader_tui_test_write_text("search/alpha-book.epub", "alpha"));
    TEST_ASSERT(mereader_tui_test_write_text("search/nested/beta-manual.pdf", "beta"));
    char *root = mereader_tui_test_path("search");
    TEST_ASSERT(root != NULL);

    MereaderTuiSearchIndex index = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_search_index_open(&index, root, false, &error), "%s", error.message);
    TEST_ASSERT_STR(index.root, root);

    MereaderTuiSearchFiles files = {0};
    TEST_ASSERT_MSG(mereader_tui_search_files(&index, "alpha", 0U, 32U, &files, &error), "%s", error.message);
    TEST_ASSERT(files.length >= 1U);
    TEST_ASSERT_STR(files.items[0].relative_path, "alpha-book.epub");
    TEST_ASSERT(files.items[0].score > 0);
    mereader_tui_search_files_free(&files);

    TEST_ASSERT(mereader_tui_test_write_text("search/gamma-notes.txt", "gamma"));
    TEST_ASSERT_MSG(mereader_tui_search_index_refresh(&index, &error), "%s", error.message);
    TEST_ASSERT_MSG(mereader_tui_search_files(&index, "gamma", 0U, 32U, &files, &error), "%s", error.message);
    TEST_ASSERT(files.length >= 1U);
    TEST_ASSERT_STR(files.items[0].relative_path, "gamma-notes.txt");
    mereader_tui_search_files_free(&files);

    mereader_tui_search_index_close(&index);
    TEST_ASSERT(index.handle == NULL);
    TEST_ASSERT(index.root == NULL);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_invalid_requests(void) {
    MereaderTuiSearchIndex index = {0};
    MereaderTuiSearchFiles files = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_search_index_open(&index, "", false, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_search_files(&index, "query", 0U, 10U, &files, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_search_index_close(&index);
    mereader_tui_search_files_free(&files);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_search_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "index_search_refresh_and_ownership", .function = test_index_search_refresh_and_ownership},
        {.name = "invalid_requests", .function = test_invalid_requests},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
