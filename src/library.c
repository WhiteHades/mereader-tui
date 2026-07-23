#include "mereader-tui/library.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <glib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool unsafe_codepoint(uint32_t codepoint) {
    const GUnicodeType type = g_unichar_type(codepoint);
    return type == G_UNICODE_CONTROL || type == G_UNICODE_FORMAT || type == G_UNICODE_LINE_SEPARATOR ||
           type == G_UNICODE_PARAGRAPH_SEPARATOR;
}

char *mereader_tui_library_sanitize_text(const char *text, size_t max_columns, MereaderTuiError *error) {
    MereaderTuiString result = {0};
    gchar *valid = g_utf8_make_valid(text == NULL ? "" : text, -1);
    if (valid == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not sanitize library text");
        return NULL;
    }
    size_t columns = 0U;
    for (const char *cursor = valid; *cursor != '\0';) {
        const char *next = g_utf8_next_char(cursor);
        const uint32_t codepoint = g_utf8_get_char(cursor);
        if (unsafe_codepoint(codepoint)) {
            cursor = next;
            continue;
        }
        const size_t length = (size_t)(next - cursor);
        const size_t width = mereader_tui_utf8_width(cursor, length);
        if (width > max_columns || columns > max_columns - width) {
            break;
        }
        if (!mereader_tui_string_append_n(&result, cursor, length, error)) {
            g_free(valid);
            mereader_tui_string_free(&result);
            return NULL;
        }
        columns += width;
        cursor = next;
    }
    g_free(valid);
    if (result.data == NULL) {
        return mereader_tui_strdup("", error);
    }
    return mereader_tui_string_take(&result);
}

static bool timestamp_digits(const char **cursor, size_t count, int *value) {
    int parsed = 0;
    for (size_t index = 0U; index < count; ++index) {
        const unsigned char character = (unsigned char)**cursor;
        if (character < (unsigned char)'0' || character > (unsigned char)'9') {
            return false;
        }
        parsed = parsed * 10 + (int)(character - (unsigned char)'0');
        ++*cursor;
    }
    *value = parsed;
    return true;
}

static bool timestamp_leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool mereader_tui_library_parse_timestamp(const char *value, struct timespec *timestamp) {
    if (value == NULL || timestamp == NULL) {
        return false;
    }
    const char *cursor = value;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!timestamp_digits(&cursor, 4U, &year) || *cursor++ != '-' ||
        !timestamp_digits(&cursor, 2U, &month) || *cursor++ != '-' ||
        !timestamp_digits(&cursor, 2U, &day) || *cursor++ != ' ' ||
        !timestamp_digits(&cursor, 2U, &hour) || *cursor++ != ':' ||
        !timestamp_digits(&cursor, 2U, &minute) || *cursor++ != ':' ||
        !timestamp_digits(&cursor, 2U, &second)) {
        return false;
    }

    long nanoseconds = 0L;
    size_t fraction_digits = 0U;
    if (*cursor == '.') {
        ++cursor;
        while (*cursor >= '0' && *cursor <= '9') {
            if (fraction_digits == 6U) {
                return false;
            }
            nanoseconds = nanoseconds * 10L + (long)(*cursor - '0');
            ++fraction_digits;
            ++cursor;
        }
        if (fraction_digits == 0U) {
            return false;
        }
        while (fraction_digits < 9U) {
            nanoseconds *= 10L;
            ++fraction_digits;
        }
    }

    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (*cursor != '\0' || year < 1 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    int maximum_day = month_days[month - 1];
    if (month == 2 && timestamp_leap_year(year)) {
        ++maximum_day;
    }
    if (day > maximum_day) {
        return false;
    }

    struct tm parsed = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
        .tm_isdst = -1,
    };
    errno = 0;
    const time_t seconds = mktime(&parsed);
    if (seconds == (time_t)-1 && errno == EOVERFLOW) {
        return false;
    }
    *timestamp = (struct timespec){.tv_sec = seconds, .tv_nsec = nanoseconds};
    return true;
}

static bool text_has_content(const char *text) {
    if (text == NULL) {
        return false;
    }
    for (const char *cursor = text; *cursor != '\0'; cursor = g_utf8_next_char(cursor)) {
        if (!g_unichar_isspace(g_utf8_get_char(cursor))) {
            return true;
        }
    }
    return false;
}

static char *normalized_key(const char *text, MereaderTuiError *error) {
    gchar *folded = g_utf8_casefold(text == NULL ? "" : text, -1);
    gchar *normalized = folded == NULL ? NULL : g_utf8_normalize(folded, -1, G_NORMALIZE_ALL_COMPOSE);
    char *key = normalized == NULL ? NULL : mereader_tui_strdup(normalized, error);
    g_free(normalized);
    g_free(folded);
    if (key == NULL && !mereader_tui_error_is_set(error)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not normalize library text");
    }
    return key;
}

static void library_row_free(MereaderTuiLibraryRow *row) {
    free(row->title);
    free(row->author);
    free(row->path);
    free(row->title_key);
    free(row->author_key);
    free(row->path_key);
    *row = (MereaderTuiLibraryRow){0};
}

void mereader_tui_library_view_free(MereaderTuiLibraryView *view) {
    if (view == NULL) {
        return;
    }
    for (size_t index = 0U; index < view->length; ++index) {
        library_row_free(&view->rows[index]);
    }
    free(view->rows);
    *view = (MereaderTuiLibraryView){0};
}

static bool library_row_build(MereaderTuiLibraryRow *row, const MereaderTuiHistoryEntry *entry, MereaderTuiError *error) {
    row->entry = entry;
    row->path = mereader_tui_library_sanitize_text(entry->filepath, SIZE_MAX, error);
    row->title = mereader_tui_library_sanitize_text(entry->title, SIZE_MAX, error);
    row->author = mereader_tui_library_sanitize_text(entry->author, SIZE_MAX, error);
    if (row->path == NULL || row->title == NULL || row->author == NULL) {
        library_row_free(row);
        return false;
    }

    if (!text_has_content(row->title)) {
        char *basename = entry->filepath == NULL ? NULL : mereader_tui_path_basename(entry->filepath, error);
        if (entry->filepath != NULL && basename == NULL) {
            library_row_free(row);
            return false;
        }
        char *fallback = mereader_tui_library_sanitize_text(basename == NULL ? "untitled" : basename, SIZE_MAX, error);
        free(basename);
        if (fallback == NULL) {
            library_row_free(row);
            return false;
        }
        free(row->title);
        row->title = fallback;
        if (!text_has_content(row->title)) {
            free(row->title);
            row->title = mereader_tui_strdup("untitled", error);
            if (row->title == NULL) {
                library_row_free(row);
                return false;
            }
        }
    }

    if (!text_has_content(row->author)) {
        free(row->author);
        row->author = mereader_tui_strdup("", error);
        if (row->author == NULL) {
            library_row_free(row);
            return false;
        }
    }

    row->title_key = normalized_key(row->title, error);
    row->author_key = normalized_key(row->author, error);
    row->path_key = normalized_key(row->path, error);
    if (row->title_key == NULL || row->author_key == NULL || row->path_key == NULL) {
        library_row_free(row);
        return false;
    }
    return true;
}

static bool row_matches(const MereaderTuiLibraryRow *row, const char *filter_key) {
    return filter_key[0] == '\0' || strstr(row->title_key, filter_key) != NULL ||
           strstr(row->author_key, filter_key) != NULL || strstr(row->path_key, filter_key) != NULL;
}

static const char *entry_filepath(const MereaderTuiLibraryRow *row) {
    return row->entry->filepath == NULL ? "" : row->entry->filepath;
}

static int compare_filepath(const MereaderTuiLibraryRow *left, const MereaderTuiLibraryRow *right) {
    return strcmp(entry_filepath(left), entry_filepath(right));
}

static int compare_recent(const void *left_pointer, const void *right_pointer) {
    const MereaderTuiLibraryRow *left = left_pointer;
    const MereaderTuiLibraryRow *right = right_pointer;
    struct timespec left_read = {0};
    struct timespec right_read = {0};
    const bool left_valid = mereader_tui_library_parse_timestamp(left->entry->last_read, &left_read);
    const bool right_valid = mereader_tui_library_parse_timestamp(right->entry->last_read, &right_read);
    if (left_valid != right_valid) {
        return left_valid ? -1 : 1;
    }
    if (left_valid && left_read.tv_sec != right_read.tv_sec) {
        return left_read.tv_sec > right_read.tv_sec ? -1 : 1;
    }
    if (left_valid && left_read.tv_nsec != right_read.tv_nsec) {
        return left_read.tv_nsec > right_read.tv_nsec ? -1 : 1;
    }
    return compare_filepath(left, right);
}

static int compare_title(const void *left_pointer, const void *right_pointer) {
    const MereaderTuiLibraryRow *left = left_pointer;
    const MereaderTuiLibraryRow *right = right_pointer;
    const int order = strcmp(left->title_key, right->title_key);
    return order == 0 ? compare_filepath(left, right) : order;
}

static int compare_author(const void *left_pointer, const void *right_pointer) {
    const MereaderTuiLibraryRow *left = left_pointer;
    const MereaderTuiLibraryRow *right = right_pointer;
    int order = 0;
    if (left->author_key[0] == '\0' && right->author_key[0] != '\0') {
        order = 1;
    } else if (left->author_key[0] != '\0' && right->author_key[0] == '\0') {
        order = -1;
    } else {
        order = strcmp(left->author_key, right->author_key);
    }
    return order == 0 ? compare_filepath(left, right) : order;
}

bool mereader_tui_library_view_build(MereaderTuiLibraryView *view, const MereaderTuiHistory *history, const char *filter,
                             MereaderTuiLibrarySort sort, MereaderTuiError *error) {
    if (view == NULL || view->rows != NULL || view->length != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "library view output is not empty");
        return false;
    }
    if (sort > MEREADER_TUI_LIBRARY_SORT_RELEVANCE) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid library sort order");
        return false;
    }
    if (history != NULL && history->length > 0U && history->items == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "library history items are missing");
        return false;
    }

    char *safe_filter = mereader_tui_library_sanitize_text(filter, SIZE_MAX, error);
    char *filter_key = safe_filter == NULL ? NULL : normalized_key(safe_filter, error);
    free(safe_filter);
    if (filter_key == NULL) {
        return false;
    }

    const size_t history_length = history == NULL ? 0U : history->length;
    MereaderTuiLibraryRow *rows = NULL;
    if (history_length > 0U) {
        rows = mereader_tui_reallocarray(NULL, history_length, sizeof(*rows), error);
        if (rows == NULL) {
            free(filter_key);
            return false;
        }
        memset(rows, 0, history_length * sizeof(*rows));
    }

    size_t length = 0U;
    for (size_t index = 0U; index < history_length; ++index) {
        MereaderTuiLibraryRow row = {0};
        if (!library_row_build(&row, &history->items[index], error)) {
            for (size_t retained = 0U; retained < length; ++retained) {
                library_row_free(&rows[retained]);
            }
            free(rows);
            free(filter_key);
            return false;
        }
        if (row_matches(&row, filter_key)) {
            rows[length++] = row;
        } else {
            library_row_free(&row);
        }
    }
    free(filter_key);

    if (length > 1U) {
        if (sort == MEREADER_TUI_LIBRARY_SORT_RECENT) {
            qsort(rows, length, sizeof(*rows), compare_recent);
        } else if (sort == MEREADER_TUI_LIBRARY_SORT_TITLE) {
            qsort(rows, length, sizeof(*rows), compare_title);
        } else if (sort == MEREADER_TUI_LIBRARY_SORT_AUTHOR) {
            qsort(rows, length, sizeof(*rows), compare_author);
        }
    }
    *view = (MereaderTuiLibraryView){.rows = rows, .length = length, .sort = sort};
    return true;
}

size_t mereader_tui_library_preserve_selection(const MereaderTuiLibraryView *view, const char *filepath, size_t fallback_index) {
    if (view == NULL || view->length == 0U) {
        return 0U;
    }
    if (filepath != NULL) {
        for (size_t index = 0U; index < view->length; ++index) {
            const char *candidate = view->rows[index].entry->filepath;
            if (candidate != NULL && strcmp(candidate, filepath) == 0) {
                return index;
            }
        }
    }
    return fallback_index < view->length ? fallback_index : view->length - 1U;
}

MereaderTuiLibrarySort mereader_tui_library_sort_next(MereaderTuiLibrarySort sort) {
    if (sort == MEREADER_TUI_LIBRARY_SORT_RECENT) {
        return MEREADER_TUI_LIBRARY_SORT_TITLE;
    }
    if (sort == MEREADER_TUI_LIBRARY_SORT_TITLE) {
        return MEREADER_TUI_LIBRARY_SORT_AUTHOR;
    }
    return MEREADER_TUI_LIBRARY_SORT_RECENT;
}

const char *mereader_tui_library_sort_name(MereaderTuiLibrarySort sort) {
    if (sort == MEREADER_TUI_LIBRARY_SORT_RELEVANCE) {
        return "relevance";
    }
    if (sort == MEREADER_TUI_LIBRARY_SORT_TITLE) {
        return "title";
    }
    if (sort == MEREADER_TUI_LIBRARY_SORT_AUTHOR) {
        return "author";
    }
    return "recent";
}
