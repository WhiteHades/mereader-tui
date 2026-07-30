#include "test_support.h"

#include "../src/library_directory_picker.h"

#include <stdlib.h>
#include <time.h>

static MereaderTuiTestResult test_open_lists_sorted_entries(void) {
    TEST_ASSERT(mereader_tui_test_mkdir("picker-open/zeta"));
    TEST_ASSERT(mereader_tui_test_mkdir("picker-open/Alpha"));
    TEST_ASSERT(mereader_tui_test_mkdir("picker-open/beta"));
    TEST_ASSERT(mereader_tui_test_write_text("picker-open/not-a-directory", "file"));
    char *root = mereader_tui_test_path("picker-open");
    TEST_ASSERT(root != NULL);

    MereaderTuiLibraryDirectoryPicker picker = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_open(&picker, root, &error), "%s", error.message);
    TEST_ASSERT_STR(picker.path, root);
    TEST_ASSERT(mereader_tui_library_directory_picker_has_parent(&picker));
    TEST_ASSERT_SIZE(mereader_tui_library_directory_picker_entry_count(&picker), 5U);

    MereaderTuiLibraryDirectoryPickerEntry entry = {0};
    TEST_ASSERT(mereader_tui_library_directory_picker_entry(&picker, 0U, &entry));
    TEST_ASSERT_INT(entry.kind, MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_USE);
    TEST_ASSERT_STR(entry.name, "[use this folder]");
    TEST_ASSERT(mereader_tui_library_directory_picker_entry(&picker, 1U, &entry));
    TEST_ASSERT_INT(entry.kind, MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_PARENT);
    TEST_ASSERT_STR(entry.name, "..");
    TEST_ASSERT(mereader_tui_library_directory_picker_entry(&picker, 2U, &entry));
    TEST_ASSERT_STR(entry.name, "Alpha");
    TEST_ASSERT(mereader_tui_library_directory_picker_entry(&picker, 3U, &entry));
    TEST_ASSERT_STR(entry.name, "beta");
    TEST_ASSERT(mereader_tui_library_directory_picker_entry(&picker, 4U, &entry));
    TEST_ASSERT_STR(entry.name, "zeta");
    TEST_ASSERT(!mereader_tui_library_directory_picker_entry(&picker, 5U, &entry));

    mereader_tui_library_directory_picker_free(&picker);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_parent_preserves_child_selection(void) {
    TEST_ASSERT(mereader_tui_test_mkdir("picker-parent/Alpha"));
    TEST_ASSERT(mereader_tui_test_mkdir("picker-parent/Current"));
    TEST_ASSERT(mereader_tui_test_mkdir("picker-parent/Zeta"));
    char *child = mereader_tui_test_path("picker-parent/Current");
    TEST_ASSERT(child != NULL);

    MereaderTuiLibraryDirectoryPicker picker = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_open(&picker, child, &error), "%s", error.message);
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_parent(&picker, &error), "%s", error.message);

    MereaderTuiLibraryDirectoryPickerEntry entry = {0};
    TEST_ASSERT(mereader_tui_library_directory_picker_entry(&picker, picker.selected, &entry));
    TEST_ASSERT_INT(entry.kind, MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_DIRECTORY);
    TEST_ASSERT_STR(entry.name, "Current");
    TEST_ASSERT_SIZE(picker.top, 0U);

    mereader_tui_library_directory_picker_free(&picker);
    free(child);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_selection_clamps_and_scrolls(void) {
    MereaderTuiLibraryDirectoryPicker empty = {0};
    mereader_tui_library_directory_picker_move_selection(&empty, 10, 3U);
    TEST_ASSERT_SIZE(empty.selected, 0U);
    TEST_ASSERT_SIZE(empty.top, 0U);

    TEST_ASSERT(mereader_tui_test_mkdir("picker-selection/a"));
    TEST_ASSERT(mereader_tui_test_mkdir("picker-selection/b"));
    TEST_ASSERT(mereader_tui_test_mkdir("picker-selection/c"));
    TEST_ASSERT(mereader_tui_test_mkdir("picker-selection/d"));
    char *root = mereader_tui_test_path("picker-selection");
    TEST_ASSERT(root != NULL);
    MereaderTuiLibraryDirectoryPicker picker = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_open(&picker, root, &error), "%s", error.message);

    mereader_tui_library_directory_picker_move_selection(&picker, -10, 3U);
    TEST_ASSERT_SIZE(picker.selected, 0U);
    TEST_ASSERT_SIZE(picker.top, 0U);
    mereader_tui_library_directory_picker_move_selection(&picker, 100, 3U);
    TEST_ASSERT_SIZE(picker.selected, 5U);
    TEST_ASSERT_SIZE(picker.top, 3U);
    mereader_tui_library_directory_picker_move_selection(&picker, -3, 3U);
    TEST_ASSERT_SIZE(picker.selected, 2U);
    TEST_ASSERT_SIZE(picker.top, 2U);
    picker.selected = 99U;
    picker.top = 99U;
    mereader_tui_library_directory_picker_ensure_selection_visible(&picker, 0U);
    TEST_ASSERT_SIZE(picker.selected, 5U);
    TEST_ASSERT_SIZE(picker.top, 5U);

    mereader_tui_library_directory_picker_free(&picker);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_active_index_fuzzy_activation(void) {
    TEST_ASSERT(mereader_tui_test_write_text(
        "picker-active/Classics Collection/book.epub", "classic"));
    TEST_ASSERT(mereader_tui_test_write_text(
        "picker-active/Unrelated Shelf/other.epub", "other"));
    char *root = mereader_tui_test_path("picker-active");
    TEST_ASSERT(root != NULL);

    MereaderTuiSearchIndex index = {0};
    MereaderTuiLibraryDirectoryPicker picker = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_search_index_open(&index, root, false, &error), "%s", error.message);
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_open(&picker, root, &error), "%s", error.message);
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_set_query(&picker, "clasic", &index, &error),
                    "%s", error.message);
    TEST_ASSERT(mereader_tui_library_directory_picker_searching(&picker));
    TEST_ASSERT(picker.search.handle == NULL);
    TEST_ASSERT(!picker.scanning);
    TEST_ASSERT_SIZE(mereader_tui_library_directory_picker_entry_count(&picker), 1U);

    MereaderTuiLibraryDirectoryPickerEntry entry = {0};
    TEST_ASSERT(mereader_tui_library_directory_picker_entry(&picker, 0U, &entry));
    TEST_ASSERT_INT(entry.kind, MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_MATCH);
    TEST_ASSERT_STR(entry.name, "Classics Collection");
    char *selected = mereader_tui_test_path("picker-active/Classics Collection");
    TEST_ASSERT(selected != NULL);
    MereaderTuiLibraryDirectoryPickerActivation activation = {0};
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_activate(&picker, &index, &activation, &error),
                    "%s", error.message);
    TEST_ASSERT_INT(activation.kind, MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ACTIVATION_BROWSED);
    TEST_ASSERT_STR(picker.path, selected);
    TEST_ASSERT(!mereader_tui_library_directory_picker_searching(&picker));

    free(selected);
    mereader_tui_library_directory_picker_free(&picker);
    mereader_tui_search_index_close(&index);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_async_search_poll_and_clear(void) {
    for (size_t index = 0U; index < 1024U; ++index) {
        char relative[128] = {0};
        (void)snprintf(relative, sizeof(relative), "picker-async/Shelf %04zu/book.epub", index);
        TEST_ASSERT(mereader_tui_test_write_text(relative, "book"));
    }
    TEST_ASSERT(mereader_tui_test_write_text(
        "picker-async/Classics Collection/classic.epub", "classic"));
    TEST_ASSERT(mereader_tui_test_write_text(
        "picker-other/Other Collection/other.epub", "other"));
    char *root = mereader_tui_test_path("picker-async");
    char *other_root = mereader_tui_test_path("picker-other");
    TEST_ASSERT(root != NULL && other_root != NULL);

    MereaderTuiSearchIndex active_index = {0};
    MereaderTuiLibraryDirectoryPicker picker = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT_MSG(mereader_tui_search_index_open(&active_index, other_root, false, &error), "%s", error.message);
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_open(&picker, root, &error), "%s", error.message);
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_set_query(
                        &picker, "clasic", &active_index, &error),
                    "%s", error.message);
    TEST_ASSERT(picker.search.handle != NULL);
    TEST_ASSERT(picker.scanning);

    bool changed = false;
    for (size_t attempt = 0U; attempt < 500U && picker.scanning; ++attempt) {
        const struct timespec delay = {.tv_nsec = 10000000L};
        (void)nanosleep(&delay, NULL);
        bool poll_changed = false;
        TEST_ASSERT_MSG(mereader_tui_library_directory_picker_poll(
                            &picker, &active_index, &poll_changed, &error),
                        "%s", error.message);
        changed = changed || poll_changed;
    }
    TEST_ASSERT(!picker.scanning);
    TEST_ASSERT(changed);
    TEST_ASSERT_SIZE(mereader_tui_library_directory_picker_entry_count(&picker), 1U);
    MereaderTuiLibraryDirectoryPickerEntry entry = {0};
    TEST_ASSERT(mereader_tui_library_directory_picker_entry(&picker, 0U, &entry));
    TEST_ASSERT_STR(entry.name, "Classics Collection");

    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_set_query(
                        &picker, "", &active_index, &error),
                    "%s", error.message);
    TEST_ASSERT(picker.search.handle == NULL);
    TEST_ASSERT_SIZE(picker.matches.length, 0U);
    TEST_ASSERT(!picker.scanning);
    TEST_ASSERT(!mereader_tui_library_directory_picker_searching(&picker));
    TEST_ASSERT(mereader_tui_library_directory_picker_entry_count(&picker) > 1U);

    mereader_tui_library_directory_picker_free(&picker);
    mereader_tui_search_index_close(&active_index);
    free(other_root);
    free(root);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_errors_are_typed_and_open_is_transactional(void) {
    TEST_ASSERT(mereader_tui_test_mkdir("picker-errors/good"));
    TEST_ASSERT(mereader_tui_test_write_text("picker-errors/file", "file"));
    char *good = mereader_tui_test_path("picker-errors/good");
    char *file = mereader_tui_test_path("picker-errors/file");
    char *missing = mereader_tui_test_path("picker-errors/missing");
    TEST_ASSERT(good != NULL && file != NULL && missing != NULL);

    MereaderTuiLibraryDirectoryPicker picker = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(!mereader_tui_library_directory_picker_open(NULL, good, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_library_directory_picker_open(&picker, NULL, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_open(&picker, good, &error), "%s", error.message);
    TEST_ASSERT(!mereader_tui_library_directory_picker_open(&picker, missing, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_NOT_FOUND);
    TEST_ASSERT_STR(picker.path, good);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_library_directory_picker_open(&picker, file, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_NOT_FOUND);
    TEST_ASSERT_STR(picker.path, good);

    mereader_tui_library_directory_picker_free(&picker);
    free(missing);
    free(file);
    free(good);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_free_is_safe_for_all_states(void) {
    MereaderTuiLibraryDirectoryPicker zeroed = {0};
    mereader_tui_library_directory_picker_free(NULL);
    mereader_tui_library_directory_picker_free(&zeroed);
    TEST_ASSERT(zeroed.path == NULL);

    MereaderTuiError error = {0};
    MereaderTuiLibraryDirectoryPicker partial = {
        .path = mereader_tui_strdup("partial", &error),
    };
    TEST_ASSERT(partial.path != NULL);
    mereader_tui_library_directory_picker_free(&partial);
    TEST_ASSERT(partial.path == NULL);

    TEST_ASSERT(mereader_tui_test_mkdir("picker-free/child"));
    char *root = mereader_tui_test_path("picker-free");
    TEST_ASSERT(root != NULL);
    MereaderTuiLibraryDirectoryPicker populated = {0};
    TEST_ASSERT_MSG(mereader_tui_library_directory_picker_open(&populated, root, &error), "%s", error.message);
    mereader_tui_library_directory_picker_free(&populated);
    TEST_ASSERT(populated.path == NULL);
    TEST_ASSERT(populated.names == NULL);
    TEST_ASSERT_SIZE(populated.length, 0U);

    free(root);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_library_directory_picker_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "open_lists_sorted_entries", .function = test_open_lists_sorted_entries},
        {.name = "parent_preserves_child_selection", .function = test_parent_preserves_child_selection},
        {.name = "selection_clamps_and_scrolls", .function = test_selection_clamps_and_scrolls},
        {.name = "active_index_fuzzy_activation", .function = test_active_index_fuzzy_activation},
        {.name = "async_search_poll_and_clear", .function = test_async_search_poll_and_clear},
        {.name = "errors_are_typed_and_open_is_transactional",
         .function = test_errors_are_typed_and_open_is_transactional},
        {.name = "free_is_safe_for_all_states", .function = test_free_is_safe_for_all_states},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
