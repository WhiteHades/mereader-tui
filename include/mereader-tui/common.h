#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MEREADER_TUI_NAME "mereader-tui"
#define MEREADER_TUI_VERSION "0.1.1"
#define MEREADER_TUI_ARRAY_LEN(values) (sizeof(values) / sizeof((values)[0]))

typedef enum MereaderTuiErrorCode : uint8_t {
    MEREADER_TUI_ERROR_NONE = 0,
    MEREADER_TUI_ERROR_ARGUMENT,
    MEREADER_TUI_ERROR_NOT_FOUND,
    MEREADER_TUI_ERROR_UNSUPPORTED,
    MEREADER_TUI_ERROR_CORRUPT,
    MEREADER_TUI_ERROR_IO,
    MEREADER_TUI_ERROR_MEMORY,
    MEREADER_TUI_ERROR_DATABASE,
    MEREADER_TUI_ERROR_EXTERNAL,
    MEREADER_TUI_ERROR_INTERNAL,
} MereaderTuiErrorCode;

typedef struct MereaderTuiError {
    MereaderTuiErrorCode code;
    char message[512];
} MereaderTuiError;

typedef struct MereaderTuiBuffer {
    unsigned char *data;
    size_t length;
    size_t capacity;
} MereaderTuiBuffer;

typedef struct MereaderTuiString {
    char *data;
    size_t length;
    size_t capacity;
} MereaderTuiString;

[[nodiscard]] bool mereader_tui_error_is_set(const MereaderTuiError *error);
void mereader_tui_error_clear(MereaderTuiError *error);
void mereader_tui_error_set(MereaderTuiError *error, MereaderTuiErrorCode code, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

[[nodiscard]] void *mereader_tui_reallocarray(void *pointer, size_t count, size_t size, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_strdup(const char *value, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_strndup(const char *value, size_t length, MereaderTuiError *error);
[[nodiscard]] void *mereader_tui_array_reserve(void *items, size_t *capacity, size_t item_size, size_t needed,
                                       MereaderTuiError *error);

void mereader_tui_buffer_free(MereaderTuiBuffer *buffer);
[[nodiscard]] bool mereader_tui_buffer_append(MereaderTuiBuffer *buffer, const void *data, size_t length, MereaderTuiError *error);

void mereader_tui_string_free(MereaderTuiString *string);
[[nodiscard]] bool mereader_tui_string_append(MereaderTuiString *string, const char *value, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_string_append_n(MereaderTuiString *string, const char *value, size_t length, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_string_append_char(MereaderTuiString *string, char value, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_string_take(MereaderTuiString *string);

[[nodiscard]] bool mereader_tui_read_file(const char *path, MereaderTuiBuffer *output, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_write_file(const char *path, const void *data, size_t length, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_mkdirs(const char *path, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_realpath(const char *path, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_path_join(const char *left, const char *right, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_path_dirname(const char *path, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_path_basename(const char *path, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_path_stem(const char *path, MereaderTuiError *error);
[[nodiscard]] const char *mereader_tui_path_extension(const char *path);
[[nodiscard]] char *mereader_tui_uri_decode(const char *value, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_uri_resolve(const char *base, const char *reference, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_is_external_uri(const char *value);
[[nodiscard]] bool mereader_tui_file_exists(const char *path);
[[nodiscard]] bool mereader_tui_directory_exists(const char *path);
[[nodiscard]] bool mereader_tui_remove_tree(const char *path, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_make_temp_directory(const char *prefix, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_xdg_config_path(const char *filename, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_xdg_cache_path(const char *filename, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_now_iso8601(MereaderTuiError *error);
[[nodiscard]] size_t mereader_tui_utf8_width(const char *text, size_t length);
[[nodiscard]] size_t mereader_tui_utf8_next(const char *text, size_t length, size_t offset, int *columns);
[[nodiscard]] int mereader_tui_casecmp(const char *left, const char *right);
[[nodiscard]] bool mereader_tui_contains_casefold(const char *text, const char *query);
