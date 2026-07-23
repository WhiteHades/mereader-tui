#include "test_support.h"

#include "mereader-tui/library_shelf.h"

#include <stdlib.h>

static const MereaderTuiHistoryEntry *shelf_find_entry(const MereaderTuiLibraryShelf *shelf, const char *title) {
    for (size_t index = 0U; index < shelf->entries.length; ++index) {
        if (strcmp(shelf->entries.items[index].title, title) == 0) {
            return &shelf->entries.items[index];
        }
    }
    return NULL;
}

static MereaderTuiTestResult test_calibre_shelf_hierarchy_and_references(void) {
    TEST_ASSERT(mereader_tui_test_write_text("shelf-calibre/metadata.db", ""));
    TEST_ASSERT(mereader_tui_test_write_text("shelf-calibre/Alice Writer/First Book (42)/first.epub", "epub"));
    TEST_ASSERT(mereader_tui_test_write_text("shelf-calibre/Alice Writer/First Book (42)/first.pdf", "pdf"));
    TEST_ASSERT(mereader_tui_test_write_text("shelf-calibre/Bob Writer/Second Book (7)/second.epub", "epub"));
    char *root = mereader_tui_test_path("shelf-calibre");
    TEST_ASSERT(root != NULL);

    MereaderTuiCatalog catalog = {0};
    MereaderTuiLibraryShelf shelf = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_catalog_open(&catalog, root, NULL, false, &error), "%s", error.message);
    TEST_ASSERT(mereader_tui_catalog_is_open(&catalog));

    TEST_ASSERT_MSG(mereader_tui_library_shelf_build(&shelf, &catalog, MEREADER_TUI_LIBRARY_SHELF_AUTHORS, NULL, 0U, "", &error),
                    "%s", error.message);
    TEST_ASSERT_SIZE(shelf.entries.length, 2U);
    const MereaderTuiHistoryEntry *alice = shelf_find_entry(&shelf, "Alice Writer");
    TEST_ASSERT(alice != NULL);
    TEST_ASSERT_STR(alice->author, "1 book");
    const MereaderTuiLibraryShelfReference *alice_reference = mereader_tui_library_shelf_reference(&shelf, alice);
    TEST_ASSERT(alice_reference != NULL);
    const size_t first_book = alice_reference->book_index;
    TEST_ASSERT_STR(catalog.books[first_book].title, "First Book");
    mereader_tui_library_shelf_free(&shelf);

    TEST_ASSERT_MSG(mereader_tui_library_shelf_build(&shelf, &catalog, MEREADER_TUI_LIBRARY_SHELF_BOOKS, "Alice Writer", 0U, "",
                                              &error),
                    "%s", error.message);
    TEST_ASSERT_SIZE(shelf.entries.length, 1U);
    TEST_ASSERT_STR(shelf.entries.items[0].title, "First Book [EPUB +1]");
    TEST_ASSERT_STR(shelf.entries.items[0].author, "Alice Writer");
    mereader_tui_library_shelf_free(&shelf);

    TEST_ASSERT_MSG(mereader_tui_library_shelf_build(&shelf, &catalog, MEREADER_TUI_LIBRARY_SHELF_FORMATS, NULL, first_book, "",
                                              &error),
                    "%s", error.message);
    TEST_ASSERT_SIZE(shelf.entries.length, 2U);
    const MereaderTuiHistoryEntry *epub = shelf_find_entry(&shelf, "First Book [EPUB]");
    const MereaderTuiHistoryEntry *pdf = shelf_find_entry(&shelf, "First Book [PDF]");
    TEST_ASSERT(epub != NULL && pdf != NULL);
    const MereaderTuiLibraryShelfReference *epub_reference = mereader_tui_library_shelf_reference(&shelf, epub);
    const MereaderTuiLibraryShelfReference *pdf_reference = mereader_tui_library_shelf_reference(&shelf, pdf);
    TEST_ASSERT(epub_reference != NULL && pdf_reference != NULL);
    TEST_ASSERT_SIZE(epub_reference->book_index, first_book);
    TEST_ASSERT_SIZE(pdf_reference->book_index, first_book);
    TEST_ASSERT(epub_reference->format_index != pdf_reference->format_index);
    mereader_tui_library_shelf_free(&shelf);

    TEST_ASSERT_MSG(mereader_tui_library_shelf_build(&shelf, &catalog, MEREADER_TUI_LIBRARY_SHELF_FORMATS, NULL, first_book,
                                              "does-not-exist", &error),
                    "%s", error.message);
    TEST_ASSERT_SIZE(shelf.entries.length, 0U);
    mereader_tui_library_shelf_free(&shelf);

    TEST_ASSERT_MSG(mereader_tui_library_shelf_build(&shelf, &catalog, MEREADER_TUI_LIBRARY_SHELF_ALL, NULL, 0U, "second", &error),
                    "%s", error.message);
    TEST_ASSERT_SIZE(shelf.entries.length, 1U);
    TEST_ASSERT_STR(shelf.entries.items[0].title, "Second Book");
    mereader_tui_library_shelf_free(&shelf);
    mereader_tui_catalog_close(&catalog);
    TEST_ASSERT(!mereader_tui_catalog_is_open(&catalog));
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_shelf_rejects_nonempty_output(void) {
    MereaderTuiLibraryShelf shelf = {.entries = {.length = 1U}};
    MereaderTuiCatalog catalog = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_library_shelf_build(&shelf, &catalog, MEREADER_TUI_LIBRARY_SHELF_ALL, NULL, 0U, "", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    TEST_ASSERT_SIZE(shelf.entries.length, 1U);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_library_shelf_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "calibre_shelf_hierarchy_and_references", .function = test_calibre_shelf_hierarchy_and_references},
        {.name = "rejects_nonempty_output", .function = test_shelf_rejects_nonempty_output},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
