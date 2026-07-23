#include "test_support.h"

#include "mereader-tui/document_backend.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static MereaderTuiTestResult test_checked_strings_and_buffers(void) {
    MereaderTuiError error = {0};
    MereaderTuiString string = {0};
    TEST_ASSERT(mereader_tui_string_append(&string, "alpha", &error));
    TEST_ASSERT(mereader_tui_string_append_n(&string, string.data + 1, 3U, &error));
    TEST_ASSERT(mereader_tui_string_append_char(&string, '!', &error));
    TEST_ASSERT_STR(string.data, "alphalph!");
    TEST_ASSERT_SIZE(string.length, 9U);
    TEST_ASSERT(string.data[string.length] == '\0');
    char *taken = mereader_tui_string_take(&string);
    TEST_ASSERT_STR(taken, "alphalph!");
    TEST_ASSERT(string.data == NULL && string.length == 0U && string.capacity == 0U);
    free(taken);

    MereaderTuiBuffer buffer = {0};
    static const unsigned char bytes[] = {0x00U, 0x7fU, 0xffU};
    TEST_ASSERT(mereader_tui_buffer_append(&buffer, bytes, sizeof(bytes), &error));
    TEST_ASSERT(mereader_tui_buffer_append(&buffer, buffer.data, buffer.length, &error));
    TEST_ASSERT_SIZE(buffer.length, 6U);
    TEST_ASSERT(memcmp(buffer.data, bytes, sizeof(bytes)) == 0);
    TEST_ASSERT(memcmp(buffer.data + sizeof(bytes), bytes, sizeof(bytes)) == 0);
    TEST_ASSERT(buffer.data[buffer.length] == '\0');
    mereader_tui_buffer_free(&buffer);

    TEST_ASSERT(!mereader_tui_string_append(NULL, "x", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_buffer_append(&buffer, NULL, 1U, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(mereader_tui_reallocarray(NULL, SIZE_MAX, 2U, &error) == NULL);
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_MEMORY);

    size_t capacity = 1U;
    TEST_ASSERT(mereader_tui_array_reserve(NULL, &capacity, sizeof(int), 1U, &error) == NULL);
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_INTERNAL);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_files_and_paths(void) {
    static const unsigned char payload[] = {'a', '\0', 'b', '\n'};
    TEST_ASSERT(mereader_tui_test_write("common/nested/data.bin", payload, sizeof(payload)));
    char *path = mereader_tui_test_path("common/nested/data.bin");
    TEST_ASSERT(path != NULL);
    MereaderTuiError error = {0};
    MereaderTuiBuffer read = {0};
    TEST_ASSERT(mereader_tui_read_file(path, &read, &error));
    TEST_ASSERT_SIZE(read.length, sizeof(payload));
    TEST_ASSERT(memcmp(read.data, payload, sizeof(payload)) == 0);
    TEST_ASSERT(read.data[read.length] == '\0');
    TEST_ASSERT(mereader_tui_file_exists(path));

    char *directory = mereader_tui_path_dirname(path, &error);
    char *basename = mereader_tui_path_basename(path, &error);
    char *stem = mereader_tui_path_stem(path, &error);
    TEST_ASSERT(directory != NULL && basename != NULL && stem != NULL);
    TEST_ASSERT(mereader_tui_directory_exists(directory));
    TEST_ASSERT_STR(basename, "data.bin");
    TEST_ASSERT_STR(stem, "data");
    TEST_ASSERT_STR(mereader_tui_path_extension(path), ".bin");
    free(directory);
    free(basename);
    free(stem);
    mereader_tui_buffer_free(&read);
    free(path);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_utf8_width(void) {
    static const char text[] = "A界e\xcc\x81🙂";
    TEST_ASSERT_SIZE(mereader_tui_utf8_width(text, strlen(text)), 6U);
    int columns = -1;
    size_t offset = mereader_tui_utf8_next(text, strlen(text), 1U, &columns);
    TEST_ASSERT_SIZE(offset, 4U);
    TEST_ASSERT_INT(columns, 2);
    offset = mereader_tui_utf8_next(text, strlen(text), 5U, &columns);
    TEST_ASSERT_SIZE(offset, 7U);
    TEST_ASSERT_INT(columns, 0);
    static const char controls[] = "\x01\x7f";
    TEST_ASSERT_SIZE(mereader_tui_utf8_width(controls, sizeof(controls) - 1U), 0U);
    static const char malformed[] = "\xf0\x28\x8c\x28";
    TEST_ASSERT_SIZE(mereader_tui_utf8_width(malformed, sizeof(malformed) - 1U), 4U);
    TEST_ASSERT_SIZE(mereader_tui_utf8_width(NULL, 20U), 0U);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_uri_components_and_traversal(void) {
    MereaderTuiError error = {0};
    char *decoded = mereader_tui_uri_decode("part%20one%23x%3Fy", &error);
    TEST_ASSERT_STR(decoded, "part one#x?y");
    free(decoded);

    char *resolved = mereader_tui_document_resolve_uri("OPS/Text/chapter.xhtml?old=1#old",
                                               "../Images/a%23b%3Fc.png?size=1%3F2#spot%23one", false,
                                               &error);
    TEST_ASSERT_STR(resolved, "OPS/Images/a%23b%3Fc.png?size=1?2#spot%23one");
    char *member = mereader_tui_document_uri_path(resolved, &error);
    TEST_ASSERT_STR(member, "OPS/Images/a#b?c.png");
    free(member);
    free(resolved);

    resolved = mereader_tui_document_resolve_uri("OPS/Text/one.xhtml", "./nested/../two.xhtml#here", false, &error);
    TEST_ASSERT_STR(resolved, "OPS/Text/two.xhtml#here");
    free(resolved);
    resolved = mereader_tui_document_resolve_uri("OPS/Text/one.xhtml", "../../../escape.xhtml", false, &error);
    TEST_ASSERT(resolved == NULL);
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    mereader_tui_error_clear(&error);

    char *target = mereader_tui_document_fragment_target("OPS/a%23b%3F.xhtml?query#old", "id?#value", &error);
    TEST_ASSERT_STR(target, "OPS/a%23b%3F.xhtml#id?%23value");
    free(target);
    TEST_ASSERT(mereader_tui_is_external_uri("https://example.test/book"));
    TEST_ASSERT(mereader_tui_is_external_uri("mailto:reader@example.test"));
    TEST_ASSERT(!mereader_tui_is_external_uri("Text/chapter.xhtml"));
    TEST_ASSERT(mereader_tui_document_resolve_uri("OPS/a.xhtml", "https://example.test", false, &error) == NULL);
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_xdg_isolation(void) {
    MereaderTuiError error = {0};
    char *config_root = mereader_tui_test_path("xdg-config/mereader-tui");
    char *cache_root = mereader_tui_test_path("xdg-cache/mereader-tui");
    char *expected_config = mereader_tui_test_path("xdg-config/mereader-tui/config.ini");
    char *expected_cache = mereader_tui_test_path("xdg-cache/mereader-tui/history/mereader-tui.db");
    TEST_ASSERT(config_root != NULL && cache_root != NULL);
    TEST_ASSERT(mereader_tui_mkdirs(config_root, &error));
    TEST_ASSERT(mereader_tui_mkdirs(cache_root, &error));
    TEST_ASSERT(chmod(config_root, 0755) == 0);
    TEST_ASSERT(chmod(cache_root, 0755) == 0);
    char *config = mereader_tui_xdg_config_path("config.ini", &error);
    char *cache = mereader_tui_xdg_cache_path("history/mereader-tui.db", &error);
    TEST_ASSERT(expected_config != NULL && expected_cache != NULL && config != NULL && cache != NULL);
    TEST_ASSERT_STR(config, expected_config);
    TEST_ASSERT_STR(cache, expected_cache);
    struct stat config_status;
    struct stat cache_status;
    TEST_ASSERT(stat(config_root, &config_status) == 0 && (config_status.st_mode & 0777U) == 0700U);
    TEST_ASSERT(stat(cache_root, &cache_status) == 0 && (cache_status.st_mode & 0777U) == 0700U);
    free(config_root);
    free(cache_root);
    free(expected_config);
    free(expected_cache);
    free(config);
    free(cache);

    TEST_ASSERT(mereader_tui_xdg_cache_path("../escape", &error) == NULL);
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(mereader_tui_xdg_config_path("absolute/../escape", &error) == NULL);
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);

    char *temporary = mereader_tui_make_temp_directory("common-temp-", &error);
    TEST_ASSERT(temporary != NULL);
    char *temporary_root = mereader_tui_test_path("tmp/");
    TEST_ASSERT(temporary_root != NULL);
    TEST_ASSERT(strncmp(temporary, temporary_root, strlen(temporary_root)) == 0);
    TEST_ASSERT(mereader_tui_directory_exists(temporary));
    TEST_ASSERT(mereader_tui_remove_tree(temporary, &error));
    free(temporary_root);
    free(temporary);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_symlink_safe_tree_removal(void) {
    TEST_ASSERT(mereader_tui_test_write_text("remove/outside/keep.txt", "keep"));
    TEST_ASSERT(mereader_tui_test_write_text("remove/tree/inside.txt", "inside"));
    char *tree = mereader_tui_test_path("remove/tree");
    char *link = mereader_tui_test_path("remove/tree/outside-link");
    char *outside_file = mereader_tui_test_path("remove/outside/keep.txt");
    TEST_ASSERT(tree != NULL && link != NULL && outside_file != NULL);
    TEST_ASSERT(symlink("../outside", link) == 0);

    MereaderTuiError error = {0};
    TEST_ASSERT(mereader_tui_remove_tree(tree, &error));
    TEST_ASSERT(!mereader_tui_directory_exists(tree));
    TEST_ASSERT(mereader_tui_file_exists(outside_file));
    TEST_ASSERT(mereader_tui_remove_tree(tree, &error));
    TEST_ASSERT(!mereader_tui_remove_tree("/", &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    free(tree);
    free(link);
    free(outside_file);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_common_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
        {.name = "checked_strings_and_buffers", .function = test_checked_strings_and_buffers},
        {.name = "files_and_paths", .function = test_files_and_paths},
        {.name = "utf8_width", .function = test_utf8_width},
        {.name = "uri_components_and_traversal", .function = test_uri_components_and_traversal},
        {.name = "xdg_isolation", .function = test_xdg_isolation},
        {.name = "symlink_safe_tree_removal", .function = test_symlink_safe_tree_removal},
    };
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
