#include "test_support.h"

#include "mereader-tui/catalog.h"

#include <stdlib.h>
#include <unistd.h>

static const MereaderTuiCatalogBook *catalog_find_book(const MereaderTuiCatalog *catalog, const char *title) {
    for (size_t index = 0U; index < catalog->length; ++index) {
        if (strcmp(catalog->books[index].title, title) == 0) {
            return &catalog->books[index];
        }
    }
    return NULL;
}

static MereaderTuiTestResult test_calibre_hierarchy_formats_search_and_progress(void) {
    TEST_ASSERT(mereader_tui_test_write_text("catalog-calibre/metadata.db", ""));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-calibre/Alice Writer/First Book (42)/first.epub", "epub"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-calibre/Alice Writer/First Book (42)/first.pdf", "pdf"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-calibre/Bob Writer/Second Book (7)/second.pdf", "pdf"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-calibre/Bob Writer/Second Book (7)/cover.jpg", "cover"));
    char *root = mereader_tui_test_path("catalog-calibre");
    char *epub = mereader_tui_test_path("catalog-calibre/Alice Writer/First Book (42)/first.epub");
    TEST_ASSERT(root != NULL && epub != NULL);
    MereaderTuiHistoryEntry entry = {
        .filepath = epub,
        .reading_progress = 0.625,
        .last_read = "2026-07-22 10:00:00",
    };
    MereaderTuiHistory history = {.items = &entry, .length = 1U};
    MereaderTuiCatalog catalog = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_catalog_open(&catalog, root, &history, false, &error), "%s", error.message);
    TEST_ASSERT(catalog.calibre);
    TEST_ASSERT_SIZE(catalog.length, 2U);
    const MereaderTuiCatalogBook *first = catalog_find_book(&catalog, "First Book");
    TEST_ASSERT(first != NULL);
    TEST_ASSERT_STR(first->author, "Alice Writer");
    TEST_ASSERT_SIZE(first->format_count, 2U);
    const MereaderTuiCatalogFormat *preferred = mereader_tui_catalog_preferred_format(first);
    TEST_ASSERT(preferred != NULL);
    TEST_ASSERT_STR(preferred->name, "EPUB");
    TEST_ASSERT_DOUBLE(preferred->reading_progress, 0.625, 1e-12);
    TEST_ASSERT_STR(preferred->last_read, "2026-07-22 10:00:00");

    TEST_ASSERT_MSG(mereader_tui_catalog_update_progress(&catalog, NULL, &error), "%s", error.message);
    TEST_ASSERT_DOUBLE(preferred->reading_progress, 0.0, 1e-12);
    TEST_ASSERT(preferred->last_read == NULL);
    entry.reading_progress = 0.75;
    entry.last_read = "2026-07-22 11:00:00";
    TEST_ASSERT_MSG(mereader_tui_catalog_update_progress(&catalog, &history, &error), "%s", error.message);
    TEST_ASSERT_DOUBLE(preferred->reading_progress, 0.75, 1e-12);
    TEST_ASSERT_STR(preferred->last_read, "2026-07-22 11:00:00");

    MereaderTuiCatalogMatches matches = {0};
    TEST_ASSERT_MSG(mereader_tui_catalog_search(&catalog, "alice first", &matches, &error), "%s", error.message);
    TEST_ASSERT(matches.length >= 1U);
    TEST_ASSERT_STR(catalog.books[matches.book_indices[0]].title, "First Book");
    mereader_tui_catalog_matches_free(&matches);
    mereader_tui_catalog_close(&catalog);
    free(epub);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_flat_folder_groups_same_stem_only(void) {
    TEST_ASSERT(mereader_tui_test_write_text("catalog-flat/alpha.epub", "epub"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-flat/alpha.pdf", "pdf"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-flat/beta.epub", "epub"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-flat/cover.jpg", "cover"));
    char *root = mereader_tui_test_path("catalog-flat");
    TEST_ASSERT(root != NULL);
    MereaderTuiCatalog catalog = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_catalog_open(&catalog, root, NULL, false, &error), "%s", error.message);
    TEST_ASSERT(!catalog.calibre);
    TEST_ASSERT_SIZE(catalog.length, 2U);
    const MereaderTuiCatalogBook *alpha = catalog_find_book(&catalog, "alpha");
    TEST_ASSERT(alpha != NULL);
    TEST_ASSERT_STR(alpha->author, "");
    TEST_ASSERT_SIZE(alpha->format_count, 2U);
    TEST_ASSERT_STR(mereader_tui_catalog_preferred_format(alpha)->name, "EPUB");
    mereader_tui_catalog_close(&catalog);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_nested_hidden_folder_and_format_preference(void) {
    TEST_ASSERT(mereader_tui_test_write_text("catalog-nested/.ignore", "skipped/\n"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-nested/skipped/topic.pdf", "pdf"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-nested/skipped/topic.epub", "epub"));
    char *root = mereader_tui_test_path("catalog-nested");
    TEST_ASSERT(root != NULL);
    MereaderTuiCatalog catalog = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_catalog_open(&catalog, root, NULL, false, &error), "%s", error.message);
    TEST_ASSERT_SIZE(catalog.length, 1U);
    const MereaderTuiCatalogBook *topic = catalog_find_book(&catalog, "topic");
    TEST_ASSERT(topic != NULL);
    TEST_ASSERT_SIZE(topic->format_count, 2U);
    TEST_ASSERT_STR(topic->formats[0].name, "EPUB");
    TEST_ASSERT_STR(topic->formats[1].name, "PDF");
    TEST_ASSERT_STR(mereader_tui_catalog_preferred_format(topic)->name, "EPUB");

    MereaderTuiFormatPreference item = {.book_key = topic->group_key,
                                 .relative_path = "skipped/topic.pdf"};
    MereaderTuiFormatPreferences preferences = {.items = &item, .length = 1U};
    TEST_ASSERT_MSG(mereader_tui_catalog_apply_format_preferences(&catalog, &preferences, &error), "%s", error.message);
    TEST_ASSERT_STR(mereader_tui_catalog_preferred_format(topic)->name, "PDF");

    MereaderTuiCatalogMatches matches = {0};
    TEST_ASSERT_MSG(mereader_tui_catalog_search(&catalog, "tpc", &matches, &error), "%s", error.message);
    TEST_ASSERT_SIZE(matches.length, 1U);
    TEST_ASSERT_STR(catalog.books[matches.book_indices[0]].title, "topic");
    mereader_tui_catalog_matches_free(&matches);

    item.relative_path = "skipped/topic.mobi";
    TEST_ASSERT_MSG(mereader_tui_catalog_apply_format_preferences(&catalog, &preferences, &error), "%s", error.message);
    TEST_ASSERT_STR(mereader_tui_catalog_preferred_format(topic)->name, "PDF");
    mereader_tui_catalog_close(&catalog);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_symlink_escape_and_exact_duplicate_format_preference(void) {
    TEST_ASSERT(mereader_tui_test_write_text("catalog-symlink/root/inside.epub", "inside"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-symlink/outside/outside.epub", "outside"));
    char *root = mereader_tui_test_path("catalog-symlink/root");
    char *outside = mereader_tui_test_path("catalog-symlink/outside");
    char *link = mereader_tui_test_path("catalog-symlink/root/escape");
    TEST_ASSERT(root != NULL && outside != NULL && link != NULL);
    TEST_ASSERT(symlink(outside, link) == 0);

    MereaderTuiCatalog catalog = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_catalog_open(&catalog, root, NULL, false, &error), "%s", error.message);
    TEST_ASSERT_SIZE(catalog.length, 1U);
    TEST_ASSERT_STR(catalog.books[0].title, "inside");
    mereader_tui_catalog_close(&catalog);

    TEST_ASSERT(mereader_tui_test_write_text("catalog-duplicate/topic.PDF", "upper"));
    TEST_ASSERT(mereader_tui_test_write_text("catalog-duplicate/topic.pdf", "lower"));
    char *duplicate_root = mereader_tui_test_path("catalog-duplicate");
    TEST_ASSERT(duplicate_root != NULL);
    TEST_ASSERT_MSG(mereader_tui_catalog_open(&catalog, duplicate_root, NULL, false, &error), "%s", error.message);
    TEST_ASSERT_SIZE(catalog.length, 1U);
    TEST_ASSERT_SIZE(catalog.books[0].format_count, 2U);
    MereaderTuiFormatPreference item = {.book_key = catalog.books[0].group_key,
                                 .relative_path = "topic.pdf"};
    MereaderTuiFormatPreferences preferences = {.items = &item, .length = 1U};
    TEST_ASSERT_MSG(mereader_tui_catalog_apply_format_preferences(&catalog, &preferences, &error), "%s", error.message);
    TEST_ASSERT_STR(mereader_tui_catalog_preferred_format(&catalog.books[0])->relative_path, "topic.pdf");
    mereader_tui_catalog_close(&catalog);

    free(duplicate_root);
    free(link);
    free(outside);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_catalog_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "calibre_hierarchy_formats_search_and_progress",
         .function = test_calibre_hierarchy_formats_search_and_progress},
        {.name = "flat_folder_groups_same_stem_only", .function = test_flat_folder_groups_same_stem_only},
        {.name = "nested_hidden_folder_and_format_preference",
         .function = test_nested_hidden_folder_and_format_preference},
        {.name = "symlink_escape_and_exact_duplicate_format_preference",
         .function = test_symlink_escape_and_exact_duplicate_format_preference},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
