#include "mereader-tui/common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static MereaderTuiErrorCode mereader_tui_errno_code(int value) {
    return value == ENOENT || value == ENOTDIR ? MEREADER_TUI_ERROR_NOT_FOUND : MEREADER_TUI_ERROR_IO;
}

bool mereader_tui_error_is_set(const MereaderTuiError *error) {
    return error != nullptr && error->code != MEREADER_TUI_ERROR_NONE;
}

void mereader_tui_error_clear(MereaderTuiError *error) {
    if (error == nullptr) {
        return;
    }
    error->code = MEREADER_TUI_ERROR_NONE;
    error->message[0] = '\0';
}

void mereader_tui_error_set(MereaderTuiError *error, MereaderTuiErrorCode code, const char *format, ...) {
    if (error == nullptr) {
        return;
    }

    error->code = code;
    if (format == nullptr) {
        error->message[0] = '\0';
        return;
    }

    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

void *mereader_tui_reallocarray(void *pointer, size_t count, size_t size, MereaderTuiError *error) {
    if (size != 0U && count > SIZE_MAX / size) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "allocation size overflow (%zu * %zu)", count, size);
        return nullptr;
    }
    if (count == 0U || size == 0U) {
        free(pointer);
        return nullptr;
    }

    void *result = realloc(pointer, count * size);
    if (result == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate %zu bytes", count * size);
    }
    return result;
}

char *mereader_tui_strndup(const char *value, size_t length, MereaderTuiError *error) {
    if (value == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "cannot duplicate a null string");
        return nullptr;
    }
    if (length == SIZE_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "string size overflow");
        return nullptr;
    }

    char *copy = mereader_tui_reallocarray(nullptr, length + 1U, sizeof(*copy), error);
    if (copy == nullptr) {
        return nullptr;
    }
    if (length != 0U) {
        memcpy(copy, value, length);
    }
    copy[length] = '\0';
    return copy;
}

char *mereader_tui_strdup(const char *value, MereaderTuiError *error) {
    if (value == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "cannot duplicate a null string");
        return nullptr;
    }
    return mereader_tui_strndup(value, strlen(value), error);
}

void *mereader_tui_array_reserve(void *items, size_t *capacity, size_t item_size, size_t needed, MereaderTuiError *error) {
    if (capacity == nullptr || item_size == 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid array reservation");
        return nullptr;
    }
    if (needed <= *capacity) {
        if (needed != 0U && items == nullptr) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "array has capacity but no storage");
            return nullptr;
        }
        return items;
    }

    size_t new_capacity = *capacity == 0U ? 8U : *capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2U;
    }
    if (new_capacity > SIZE_MAX / item_size) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "array size overflow (%zu * %zu)", new_capacity, item_size);
        return nullptr;
    }

    void *resized = mereader_tui_reallocarray(items, new_capacity, item_size, error);
    if (resized == nullptr) {
        return nullptr;
    }
    *capacity = new_capacity;
    return resized;
}

void mereader_tui_buffer_free(MereaderTuiBuffer *buffer) {
    if (buffer == nullptr) {
        return;
    }
    free(buffer->data);
    *buffer = (MereaderTuiBuffer){0};
}

bool mereader_tui_buffer_append(MereaderTuiBuffer *buffer, const void *data, size_t length, MereaderTuiError *error) {
    if (buffer == nullptr || (length != 0U && data == nullptr)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid buffer append");
        return false;
    }
    if (buffer->length == SIZE_MAX || length > SIZE_MAX - buffer->length - 1U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "buffer size overflow");
        return false;
    }

    bool aliases = false;
    size_t source_offset = 0U;
    if (length != 0U && buffer->data != nullptr) {
        uintptr_t source_address = (uintptr_t)data;
        uintptr_t buffer_address = (uintptr_t)buffer->data;
        if (source_address >= buffer_address && source_address - buffer_address <= buffer->length) {
            source_offset = (size_t)(source_address - buffer_address);
            aliases = source_offset <= buffer->length && length <= buffer->length - source_offset;
        }
    }

    size_t needed = buffer->length + length + 1U;
    MereaderTuiError reserve_error = {0};
    unsigned char *storage =
        mereader_tui_array_reserve(buffer->data, &buffer->capacity, sizeof(*buffer->data), needed, &reserve_error);
    if (mereader_tui_error_is_set(&reserve_error)) {
        if (error != nullptr) {
            *error = reserve_error;
        }
        return false;
    }
    buffer->data = storage;

    const unsigned char *source = aliases ? buffer->data + source_offset : data;
    if (length != 0U) {
        memmove(buffer->data + buffer->length, source, length);
    }
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

void mereader_tui_string_free(MereaderTuiString *string) {
    if (string == nullptr) {
        return;
    }
    free(string->data);
    *string = (MereaderTuiString){0};
}

bool mereader_tui_string_append_n(MereaderTuiString *string, const char *value, size_t length, MereaderTuiError *error) {
    if (string == nullptr || (length != 0U && value == nullptr)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid string append");
        return false;
    }
    if (string->length == SIZE_MAX || length > SIZE_MAX - string->length - 1U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "string size overflow");
        return false;
    }

    bool aliases = false;
    size_t source_offset = 0U;
    if (length != 0U && string->data != nullptr) {
        uintptr_t source_address = (uintptr_t)value;
        uintptr_t string_address = (uintptr_t)string->data;
        if (source_address >= string_address && source_address - string_address <= string->length) {
            source_offset = (size_t)(source_address - string_address);
            aliases = source_offset <= string->length && length <= string->length - source_offset;
        }
    }

    size_t needed = string->length + length + 1U;
    MereaderTuiError reserve_error = {0};
    char *storage =
        mereader_tui_array_reserve(string->data, &string->capacity, sizeof(*string->data), needed, &reserve_error);
    if (mereader_tui_error_is_set(&reserve_error)) {
        if (error != nullptr) {
            *error = reserve_error;
        }
        return false;
    }
    string->data = storage;

    const char *source = aliases ? string->data + source_offset : value;
    if (length != 0U) {
        memmove(string->data + string->length, source, length);
    }
    string->length += length;
    string->data[string->length] = '\0';
    return true;
}

bool mereader_tui_string_append(MereaderTuiString *string, const char *value, MereaderTuiError *error) {
    if (value == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "cannot append a null string");
        return false;
    }
    return mereader_tui_string_append_n(string, value, strlen(value), error);
}

bool mereader_tui_string_append_char(MereaderTuiString *string, char value, MereaderTuiError *error) {
    return mereader_tui_string_append_n(string, &value, 1U, error);
}

char *mereader_tui_string_take(MereaderTuiString *string) {
    if (string == nullptr) {
        return nullptr;
    }
    char *data = string->data;
    *string = (MereaderTuiString){0};
    return data;
}

bool mereader_tui_read_file(const char *path, MereaderTuiBuffer *output, MereaderTuiError *error) {
    if (path == nullptr || output == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid file read");
        return false;
    }
    if (output->data != nullptr || output->length != 0U || output->capacity != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "file read output is not empty");
        return false;
    }

    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        int saved_errno = errno;
        mereader_tui_error_set(error, mereader_tui_errno_code(saved_errno), "could not open '%s': %s", path, strerror(saved_errno));
        return false;
    }

    MereaderTuiBuffer result = {0};
    unsigned char chunk[16384];
    for (;;) {
        ssize_t count = read(descriptor, chunk, sizeof(chunk));
        if (count > 0) {
            if (!mereader_tui_buffer_append(&result, chunk, (size_t)count, error)) {
                (void)close(descriptor);
                mereader_tui_buffer_free(&result);
                return false;
            }
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }

        int saved_errno = errno;
        (void)close(descriptor);
        mereader_tui_buffer_free(&result);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read '%s': %s", path, strerror(saved_errno));
        return false;
    }

    if (!mereader_tui_buffer_append(&result, nullptr, 0U, error)) {
        (void)close(descriptor);
        mereader_tui_buffer_free(&result);
        return false;
    }
    if (close(descriptor) != 0) {
        int saved_errno = errno;
        mereader_tui_buffer_free(&result);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close '%s': %s", path, strerror(saved_errno));
        return false;
    }

    *output = result;
    return true;
}

bool mereader_tui_write_file(const char *path, const void *data, size_t length, MereaderTuiError *error) {
    if (path == nullptr || (length != 0U && data == nullptr)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid file write");
        return false;
    }

    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (descriptor < 0) {
        int saved_errno = errno;
        mereader_tui_error_set(error, mereader_tui_errno_code(saved_errno), "could not open '%s' for writing: %s", path,
                       strerror(saved_errno));
        return false;
    }

    const unsigned char *cursor = data;
    size_t remaining = length;
    while (remaining != 0U) {
        size_t chunk = remaining > 1048576U ? 1048576U : remaining;
        ssize_t count = write(descriptor, cursor, chunk);
        if (count > 0) {
            cursor += (size_t)count;
            remaining -= (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }

        int saved_errno = count == 0 ? EIO : errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not write '%s': %s", path, strerror(saved_errno));
        return false;
    }

    if (close(descriptor) != 0) {
        int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close '%s': %s", path, strerror(saved_errno));
        return false;
    }
    return true;
}

static bool mereader_tui_ensure_directory(const char *path, MereaderTuiError *error) {
    if (mkdir(path, 0700) == 0) {
        return true;
    }
    if (errno == EEXIST) {
        struct stat status;
        if (stat(path, &status) == 0 && S_ISDIR(status.st_mode)) {
            return true;
        }
        errno = ENOTDIR;
    }

    int saved_errno = errno;
    mereader_tui_error_set(error, mereader_tui_errno_code(saved_errno), "could not create directory '%s': %s", path,
                   strerror(saved_errno));
    return false;
}

bool mereader_tui_mkdirs(const char *path, MereaderTuiError *error) {
    if (path == nullptr || path[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "directory path is empty");
        return false;
    }

    char *copy = mereader_tui_strdup(path, error);
    if (copy == nullptr) {
        return false;
    }

    size_t length = strlen(copy);
    while (length > 1U && copy[length - 1U] == '/') {
        copy[--length] = '\0';
    }
    if (strcmp(copy, "/") == 0) {
        free(copy);
        return true;
    }

    for (char *cursor = copy + (copy[0] == '/' ? 1 : 0);; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') {
            continue;
        }

        char saved = *cursor;
        *cursor = '\0';
        if (copy[0] != '\0' && !mereader_tui_ensure_directory(copy, error)) {
            free(copy);
            return false;
        }
        *cursor = saved;
        if (saved == '\0') {
            break;
        }
        while (cursor[1] == '/') {
            ++cursor;
        }
    }

    free(copy);
    return true;
}

char *mereader_tui_realpath(const char *path, MereaderTuiError *error) {
    if (path == nullptr || path[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "path is empty");
        return nullptr;
    }

    char *resolved = realpath(path, nullptr);
    if (resolved == nullptr) {
        int saved_errno = errno;
        mereader_tui_error_set(error, mereader_tui_errno_code(saved_errno), "could not resolve '%s': %s", path,
                       strerror(saved_errno));
    }
    return resolved;
}

char *mereader_tui_path_join(const char *left, const char *right, MereaderTuiError *error) {
    if (left == nullptr || right == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "cannot join a null path");
        return nullptr;
    }
    if (right[0] == '/') {
        return mereader_tui_strdup(right, error);
    }
    if (left[0] == '\0') {
        return mereader_tui_strdup(right, error);
    }
    if (right[0] == '\0') {
        return mereader_tui_strdup(left, error);
    }

    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool separator = left[left_length - 1U] != '/';
    size_t overhead = separator ? 2U : 1U;
    if (left_length > SIZE_MAX - overhead || right_length > SIZE_MAX - left_length - overhead) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "joined path is too large");
        return nullptr;
    }

    size_t total = left_length + right_length + (separator ? 2U : 1U);
    char *joined = mereader_tui_reallocarray(nullptr, total, sizeof(*joined), error);
    if (joined == nullptr) {
        return nullptr;
    }
    memcpy(joined, left, left_length);
    size_t offset = left_length;
    if (separator) {
        joined[offset++] = '/';
    }
    memcpy(joined + offset, right, right_length + 1U);
    return joined;
}

static size_t mereader_tui_trimmed_path_length(const char *path) {
    size_t length = strlen(path);
    while (length > 1U && path[length - 1U] == '/') {
        --length;
    }
    return length;
}

char *mereader_tui_path_dirname(const char *path, MereaderTuiError *error) {
    if (path == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "path is null");
        return nullptr;
    }
    if (path[0] == '\0') {
        return mereader_tui_strdup(".", error);
    }

    size_t length = mereader_tui_trimmed_path_length(path);
    if (length == 1U && path[0] == '/') {
        return mereader_tui_strdup("/", error);
    }

    size_t slash = length;
    while (slash != 0U && path[slash - 1U] != '/') {
        --slash;
    }
    if (slash == 0U) {
        return mereader_tui_strdup(".", error);
    }
    while (slash > 1U && path[slash - 1U] == '/') {
        --slash;
    }
    return mereader_tui_strndup(path, slash, error);
}

char *mereader_tui_path_basename(const char *path, MereaderTuiError *error) {
    if (path == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "path is null");
        return nullptr;
    }
    if (path[0] == '\0') {
        return mereader_tui_strdup(".", error);
    }

    size_t length = mereader_tui_trimmed_path_length(path);
    if (length == 1U && path[0] == '/') {
        return mereader_tui_strdup("/", error);
    }
    size_t start = length;
    while (start != 0U && path[start - 1U] != '/') {
        --start;
    }
    return mereader_tui_strndup(path + start, length - start, error);
}

char *mereader_tui_path_stem(const char *path, MereaderTuiError *error) {
    char *basename = mereader_tui_path_basename(path, error);
    if (basename == nullptr) {
        return nullptr;
    }

    char *dot = strrchr(basename, '.');
    if (dot != nullptr && dot != basename && dot[1] != '\0') {
        *dot = '\0';
    }
    return basename;
}

const char *mereader_tui_path_extension(const char *path) {
    if (path == nullptr) {
        return nullptr;
    }

    const char *basename = strrchr(path, '/');
    basename = basename == nullptr ? path : basename + 1;
    const char *dot = strrchr(basename, '.');
    return dot != nullptr && dot != basename && dot[1] != '\0' ? dot : path + strlen(path);
}

static int mereader_tui_hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') {
        return (int)(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return (int)(value - 'a') + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return (int)(value - 'A') + 10;
    }
    return -1;
}

char *mereader_tui_uri_decode(const char *value, MereaderTuiError *error) {
    if (value == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "URI is null");
        return nullptr;
    }

    size_t length = strlen(value);
    char *decoded = mereader_tui_reallocarray(nullptr, length + 1U, sizeof(*decoded), error);
    if (decoded == nullptr) {
        return nullptr;
    }

    size_t output = 0U;
    for (size_t input = 0U; input < length; ++input) {
        if (value[input] != '%') {
            decoded[output++] = value[input];
            continue;
        }
        if (input + 2U >= length) {
            free(decoded);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "incomplete percent escape in URI '%s'", value);
            return nullptr;
        }

        int high = mereader_tui_hex_value((unsigned char)value[input + 1U]);
        int low = mereader_tui_hex_value((unsigned char)value[input + 2U]);
        if (high < 0 || low < 0) {
            free(decoded);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid percent escape in URI '%s'", value);
            return nullptr;
        }

        unsigned char byte = (unsigned char)((unsigned)high * 16U + (unsigned)low);
        if (byte == '\0') {
            free(decoded);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "URI contains an encoded null byte");
            return nullptr;
        }
        decoded[output++] = (char)byte;
        input += 2U;
    }
    decoded[output] = '\0';
    return decoded;
}

bool mereader_tui_is_external_uri(const char *value) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    if (value[0] == '/' && value[1] == '/') {
        return true;
    }
    if (!isalpha((unsigned char)value[0])) {
        return false;
    }

    for (size_t index = 1U; value[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (character == ':') {
            return true;
        }
        if (!isalnum(character) && character != '+' && character != '-' && character != '.') {
            return false;
        }
    }
    return false;
}

static size_t mereader_tui_uri_path_length(const char *value) {
    size_t length = 0U;
    while (value[length] != '\0' && value[length] != '?' && value[length] != '#') {
        ++length;
    }
    return length;
}

static void mereader_tui_uri_pop_segment(MereaderTuiString *path) {
    if (path->length == 0U) {
        return;
    }
    size_t length = path->length;
    while (length != 0U && path->data[length - 1U] != '/') {
        --length;
    }
    path->length = length == 0U ? 0U : length - 1U;
    path->data[path->length] = '\0';
}

static bool mereader_tui_uri_normalize_path(const char *path, MereaderTuiString *normalized, MereaderTuiError *error) {
    size_t length = strlen(path);
    size_t cursor = 0U;
    while (cursor < length) {
        while (cursor < length && path[cursor] == '/') {
            ++cursor;
        }
        size_t start = cursor;
        while (cursor < length && path[cursor] != '/') {
            ++cursor;
        }
        size_t segment_length = cursor - start;
        if (segment_length == 0U || (segment_length == 1U && path[start] == '.')) {
            continue;
        }
        if (segment_length == 2U && path[start] == '.' && path[start + 1U] == '.') {
            mereader_tui_uri_pop_segment(normalized);
            continue;
        }
        if (normalized->length != 0U && !mereader_tui_string_append_char(normalized, '/', error)) {
            return false;
        }
        if (!mereader_tui_string_append_n(normalized, path + start, segment_length, error)) {
            return false;
        }
    }

    if (!mereader_tui_string_append_n(normalized, "", 0U, error)) {
        return false;
    }
    return true;
}

static const char *mereader_tui_find_component(const char *value, char component) {
    const char *found = strchr(value, component);
    if (component == '?' && found != nullptr) {
        const char *fragment = strchr(value, '#');
        if (fragment != nullptr && found > fragment) {
            return nullptr;
        }
    }
    return found;
}

static bool mereader_tui_append_decoded_component(MereaderTuiString *result, char marker, const char *start, size_t length,
                                          MereaderTuiError *error) {
    char *encoded = mereader_tui_strndup(start, length, error);
    if (encoded == nullptr) {
        return false;
    }
    char *decoded = mereader_tui_uri_decode(encoded, error);
    free(encoded);
    if (decoded == nullptr) {
        return false;
    }

    bool success = mereader_tui_string_append_char(result, marker, error) && mereader_tui_string_append(result, decoded, error);
    free(decoded);
    return success;
}

char *mereader_tui_uri_resolve(const char *base, const char *reference, MereaderTuiError *error) {
    if (base == nullptr || reference == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "cannot resolve a null URI");
        return nullptr;
    }
    if (mereader_tui_is_external_uri(reference)) {
        return mereader_tui_strdup(reference, error);
    }

    size_t base_path_length = mereader_tui_uri_path_length(base);
    size_t reference_path_length = mereader_tui_uri_path_length(reference);
    char *base_path_encoded = mereader_tui_strndup(base, base_path_length, error);
    char *reference_path_encoded = mereader_tui_strndup(reference, reference_path_length, error);
    if (base_path_encoded == nullptr || reference_path_encoded == nullptr) {
        free(base_path_encoded);
        free(reference_path_encoded);
        return nullptr;
    }

    char *base_path = mereader_tui_uri_decode(base_path_encoded, error);
    char *reference_path = mereader_tui_uri_decode(reference_path_encoded, error);
    free(base_path_encoded);
    free(reference_path_encoded);
    if (base_path == nullptr || reference_path == nullptr) {
        free(base_path);
        free(reference_path);
        return nullptr;
    }

    MereaderTuiString merged = {0};
    if (reference_path_length == 0U) {
        if (!mereader_tui_string_append(&merged, base_path, error)) {
            goto fail;
        }
    } else if (reference_path[0] == '/') {
        if (!mereader_tui_string_append(&merged, reference_path, error)) {
            goto fail;
        }
    } else {
        const char *slash = strrchr(base_path, '/');
        if (slash != nullptr && !mereader_tui_string_append_n(&merged, base_path, (size_t)(slash - base_path + 1), error)) {
            goto fail;
        }
        if (!mereader_tui_string_append(&merged, reference_path, error)) {
            goto fail;
        }
    }

    MereaderTuiString result = {0};
    if (!mereader_tui_uri_normalize_path(merged.data == nullptr ? "" : merged.data, &result, error)) {
        mereader_tui_string_free(&result);
        goto fail;
    }

    const char *reference_query = mereader_tui_find_component(reference, '?');
    const char *reference_fragment = strchr(reference, '#');
    const char *base_query = mereader_tui_find_component(base, '?');
    const char *base_fragment = strchr(base, '#');
    if (reference_query != nullptr) {
        const char *end = reference_fragment == nullptr ? reference + strlen(reference) : reference_fragment;
        if (!mereader_tui_append_decoded_component(&result, '?', reference_query + 1, (size_t)(end - reference_query - 1),
                                           error)) {
            mereader_tui_string_free(&result);
            goto fail;
        }
    } else if (reference_path_length == 0U && base_query != nullptr) {
        const char *end = base_fragment == nullptr ? base + strlen(base) : base_fragment;
        if (!mereader_tui_append_decoded_component(&result, '?', base_query + 1, (size_t)(end - base_query - 1), error)) {
            mereader_tui_string_free(&result);
            goto fail;
        }
    }
    if (reference_fragment != nullptr &&
        !mereader_tui_append_decoded_component(&result, '#', reference_fragment + 1, strlen(reference_fragment + 1), error)) {
        mereader_tui_string_free(&result);
        goto fail;
    }

    free(base_path);
    free(reference_path);
    mereader_tui_string_free(&merged);
    return mereader_tui_string_take(&result);

fail:
    free(base_path);
    free(reference_path);
    mereader_tui_string_free(&merged);
    return nullptr;
}

bool mereader_tui_file_exists(const char *path) {
    if (path == nullptr) {
        return false;
    }
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

bool mereader_tui_directory_exists(const char *path) {
    if (path == nullptr) {
        return false;
    }
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool mereader_tui_remove_tree_at(int parent_descriptor, const char *name, const char *path, MereaderTuiError *error) {
    struct stat status;
    if (fstatat(parent_descriptor, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        int saved_errno = errno;
        mereader_tui_error_set(error, mereader_tui_errno_code(saved_errno), "could not inspect '%s': %s", path,
                       strerror(saved_errno));
        return false;
    }

    if (!S_ISDIR(status.st_mode)) {
        if (unlinkat(parent_descriptor, name, 0) == 0 || errno == ENOENT) {
            return true;
        }
        int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not remove '%s': %s", path, strerror(saved_errno));
        return false;
    }

    int descriptor = openat(parent_descriptor, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return true;
        }
        int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not open directory '%s': %s", path, strerror(saved_errno));
        return false;
    }

    DIR *directory = fdopendir(descriptor);
    if (directory == nullptr) {
        int saved_errno = errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not open directory '%s': %s", path, strerror(saved_errno));
        return false;
    }

    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == nullptr) {
            if (errno != 0) {
                int saved_errno = errno;
                (void)closedir(directory);
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read directory '%s': %s", path,
                               strerror(saved_errno));
                return false;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char *child = mereader_tui_path_join(path, entry->d_name, error);
        if (child == nullptr) {
            (void)closedir(directory);
            return false;
        }
        bool removed = mereader_tui_remove_tree_at(dirfd(directory), entry->d_name, child, error);
        free(child);
        if (!removed) {
            (void)closedir(directory);
            return false;
        }
    }
    if (closedir(directory) != 0) {
        int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close directory '%s': %s", path, strerror(saved_errno));
        return false;
    }
    if (unlinkat(parent_descriptor, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not remove directory '%s': %s", path, strerror(saved_errno));
        return false;
    }
    return true;
}

bool mereader_tui_remove_tree(const char *path, MereaderTuiError *error) {
    if (path == nullptr || path[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "remove path is empty");
        return false;
    }

    char *parent = mereader_tui_path_dirname(path, error);
    char *name = mereader_tui_path_basename(path, error);
    if (parent == nullptr || name == nullptr) {
        free(parent);
        free(name);
        return false;
    }
    if (strcmp(name, "/") == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "refusing to remove unsafe path '%s'", path);
        free(parent);
        free(name);
        return false;
    }

    int parent_descriptor = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_descriptor < 0) {
        int saved_errno = errno;
        if (saved_errno == ENOENT) {
            free(parent);
            free(name);
            return true;
        }
        mereader_tui_error_set(error, mereader_tui_errno_code(saved_errno), "could not open parent directory '%s': %s", parent,
                       strerror(saved_errno));
        free(parent);
        free(name);
        return false;
    }

    bool removed = mereader_tui_remove_tree_at(parent_descriptor, name, path, error);
    if (close(parent_descriptor) != 0 && removed) {
        int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close parent directory '%s': %s", parent,
                       strerror(saved_errno));
        removed = false;
    }
    free(parent);
    free(name);
    return removed;
}

char *mereader_tui_make_temp_directory(const char *prefix, MereaderTuiError *error) {
    const char *name = prefix == nullptr || prefix[0] == '\0' ? MEREADER_TUI_NAME "-" : prefix;
    if (strchr(name, '/') != nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "temporary directory prefix contains '/'");
        return nullptr;
    }

    const char *temporary_root = getenv("TMPDIR");
    if (temporary_root == nullptr || temporary_root[0] != '/') {
        temporary_root = "/tmp";
    }

    MereaderTuiString template = {0};
    bool success = mereader_tui_string_append(&template, temporary_root, error);
    if (success && template.length != 0U && template.data[template.length - 1U] != '/') {
        success = mereader_tui_string_append_char(&template, '/', error);
    }
    success = success && mereader_tui_string_append(&template, name, error) &&
              mereader_tui_string_append(&template, "XXXXXX", error);
    if (!success) {
        mereader_tui_string_free(&template);
        return nullptr;
    }

    char *path = mereader_tui_string_take(&template);
    if (mkdtemp(path) == nullptr) {
        int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not create temporary directory '%s': %s", path,
                       strerror(saved_errno));
        free(path);
        return nullptr;
    }
    return path;
}

static bool mereader_tui_safe_xdg_filename(const char *filename) {
    if (filename == nullptr || filename[0] == '\0' || filename[0] == '/') {
        return false;
    }

    const char *segment = filename;
    for (const char *cursor = filename;; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') {
            continue;
        }
        size_t length = (size_t)(cursor - segment);
        if (length == 0U || (length == 1U && segment[0] == '.') ||
            (length == 2U && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }
        if (*cursor == '\0') {
            return true;
        }
        segment = cursor + 1;
    }
}

static char *mereader_tui_home_directory(MereaderTuiError *error) {
    const char *environment_home = getenv("HOME");
    if (environment_home != nullptr && environment_home[0] == '/') {
        return mereader_tui_strdup(environment_home, error);
    }

    long suggested_size = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t buffer_size = suggested_size > 0 && (unsigned long)suggested_size <= SIZE_MAX
                             ? (size_t)suggested_size
                             : 16384U;
    char *buffer = mereader_tui_reallocarray(nullptr, buffer_size, sizeof(*buffer), error);
    if (buffer == nullptr) {
        return nullptr;
    }

    struct passwd password;
    struct passwd *result = nullptr;
    int status = 0;
    for (;;) {
        status = getpwuid_r(getuid(), &password, buffer, buffer_size, &result);
        if (status != ERANGE) {
            break;
        }
        if (buffer_size > SIZE_MAX / 2U) {
            free(buffer);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "passwd buffer size overflow");
            return nullptr;
        }
        size_t new_size = buffer_size * 2U;
        char *resized = mereader_tui_reallocarray(buffer, new_size, sizeof(*buffer), error);
        if (resized == nullptr) {
            free(buffer);
            return nullptr;
        }
        buffer = resized;
        buffer_size = new_size;
    }
    if (status != 0 || result == nullptr || result->pw_dir == nullptr || result->pw_dir[0] != '/') {
        free(buffer);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not determine the user's home directory: %s",
                       status == 0 ? "no passwd entry" : strerror(status));
        return nullptr;
    }

    char *home = mereader_tui_strdup(result->pw_dir, error);
    free(buffer);
    return home;
}

static char *mereader_tui_xdg_path(const char *environment_name, const char *fallback_directory, const char *filename,
                           MereaderTuiError *error) {
    if (!mereader_tui_safe_xdg_filename(filename)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "XDG filename must be a safe relative path");
        return nullptr;
    }

    const char *environment_root = getenv(environment_name);
    char *root = nullptr;
    if (environment_root != nullptr && environment_root[0] == '/') {
        root = mereader_tui_strdup(environment_root, error);
    } else {
        char *home = mereader_tui_home_directory(error);
        if (home == nullptr) {
            return nullptr;
        }
        root = mereader_tui_path_join(home, fallback_directory, error);
        free(home);
    }
    if (root == nullptr) {
        return nullptr;
    }

    char *application_root = mereader_tui_path_join(root, MEREADER_TUI_NAME, error);
    free(root);
    if (application_root == nullptr) {
        return nullptr;
    }
    if (!mereader_tui_mkdirs(application_root, error)) {
        free(application_root);
        return nullptr;
    }
    int descriptor = open(application_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int saved_errno = descriptor < 0 ? errno : 0;
    if (descriptor >= 0 && fchmod(descriptor, 0700) != 0) {
        saved_errno = errno;
        (void)close(descriptor);
    } else if (descriptor >= 0 && close(descriptor) != 0) {
        saved_errno = errno;
    }
    if (saved_errno != 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not secure state directory '%s': %s",
                               application_root, strerror(saved_errno));
        free(application_root);
        return nullptr;
    }
    char *path = mereader_tui_path_join(application_root, filename, error);
    free(application_root);
    return path;
}

char *mereader_tui_xdg_config_path(const char *filename, MereaderTuiError *error) {
    return mereader_tui_xdg_path("XDG_CONFIG_HOME", ".config", filename, error);
}

char *mereader_tui_xdg_cache_path(const char *filename, MereaderTuiError *error) {
    return mereader_tui_xdg_path("XDG_CACHE_HOME", ".cache", filename, error);
}

char *mereader_tui_now_iso8601(MereaderTuiError *error) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read the system clock");
        return nullptr;
    }

    struct tm local;
    if (localtime_r(&now.tv_sec, &local) == nullptr) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not convert the system clock");
        return nullptr;
    }

    char date[20];
    if (strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local) == 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "could not format the system clock");
        return nullptr;
    }

    char *timestamp = mereader_tui_reallocarray(nullptr, 27U, sizeof(*timestamp), error);
    if (timestamp == nullptr) {
        return nullptr;
    }
    int length = snprintf(timestamp, 27U, "%s.%06ld", date, now.tv_nsec / 1000L);
    if (length != 26) {
        free(timestamp);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "could not format the system clock");
        return nullptr;
    }
    return timestamp;
}

static bool mereader_tui_utf8_continuation(unsigned char value) {
    return (value & 0xc0U) == 0x80U;
}

static size_t mereader_tui_utf8_decode(const char *text, size_t length, size_t offset, uint32_t *codepoint) {
    unsigned char first = (unsigned char)text[offset];
    if (first < 0x80U) {
        *codepoint = first;
        return offset + 1U;
    }

    if (first >= 0xc2U && first <= 0xdfU && offset + 1U < length) {
        unsigned char second = (unsigned char)text[offset + 1U];
        if (mereader_tui_utf8_continuation(second)) {
            *codepoint = ((uint32_t)(first & 0x1fU) << 6U) | (uint32_t)(second & 0x3fU);
            return offset + 2U;
        }
    } else if (first >= 0xe0U && first <= 0xefU && offset + 2U < length) {
        unsigned char second = (unsigned char)text[offset + 1U];
        unsigned char third = (unsigned char)text[offset + 2U];
        bool valid_second = mereader_tui_utf8_continuation(second) &&
                            (first != 0xe0U || second >= 0xa0U) &&
                            (first != 0xedU || second <= 0x9fU);
        if (valid_second && mereader_tui_utf8_continuation(third)) {
            *codepoint = ((uint32_t)(first & 0x0fU) << 12U) | ((uint32_t)(second & 0x3fU) << 6U) |
                         (uint32_t)(third & 0x3fU);
            return offset + 3U;
        }
    } else if (first >= 0xf0U && first <= 0xf4U && offset + 3U < length) {
        unsigned char second = (unsigned char)text[offset + 1U];
        unsigned char third = (unsigned char)text[offset + 2U];
        unsigned char fourth = (unsigned char)text[offset + 3U];
        bool valid_second = mereader_tui_utf8_continuation(second) &&
                            (first != 0xf0U || second >= 0x90U) &&
                            (first != 0xf4U || second <= 0x8fU);
        if (valid_second && mereader_tui_utf8_continuation(third) && mereader_tui_utf8_continuation(fourth)) {
            *codepoint = ((uint32_t)(first & 0x07U) << 18U) | ((uint32_t)(second & 0x3fU) << 12U) |
                         ((uint32_t)(third & 0x3fU) << 6U) | (uint32_t)(fourth & 0x3fU);
            return offset + 4U;
        }
    }

    *codepoint = 0xfffdU;
    return offset + 1U;
}

typedef struct MereaderTuiCodepointRange {
    uint32_t first;
    uint32_t last;
} MereaderTuiCodepointRange;

static bool mereader_tui_codepoint_combining(uint32_t value) {
    static const MereaderTuiCodepointRange ranges[] = {
        {0x0300U, 0x036fU}, {0x0483U, 0x0489U}, {0x0591U, 0x05bdU}, {0x05bfU, 0x05bfU},
        {0x05c1U, 0x05c2U}, {0x05c4U, 0x05c5U}, {0x05c7U, 0x05c7U}, {0x0610U, 0x061aU},
        {0x064bU, 0x065fU}, {0x0670U, 0x0670U}, {0x06d6U, 0x06edU}, {0x0711U, 0x0711U},
        {0x0730U, 0x074aU}, {0x07a6U, 0x07b0U}, {0x07ebU, 0x07f3U}, {0x0816U, 0x082dU},
        {0x0900U, 0x0902U}, {0x093aU, 0x093cU}, {0x0941U, 0x0948U}, {0x094dU, 0x094dU},
        {0x0951U, 0x0957U}, {0x0962U, 0x0963U}, {0x0981U, 0x0981U}, {0x09bcU, 0x09bcU},
        {0x09c1U, 0x09c4U}, {0x09cdU, 0x09cdU}, {0x09e2U, 0x09e3U}, {0x0a01U, 0x0a02U},
        {0x0a3cU, 0x0a3cU}, {0x0a41U, 0x0a42U}, {0x0a47U, 0x0a48U}, {0x0a4bU, 0x0a4dU},
        {0x0a51U, 0x0a51U}, {0x0a70U, 0x0a71U}, {0x0a81U, 0x0a82U}, {0x0abcU, 0x0abcU},
        {0x0ac1U, 0x0ac5U}, {0x0ac7U, 0x0ac8U}, {0x0acdU, 0x0acdU}, {0x0ae2U, 0x0ae3U},
        {0x0b01U, 0x0b01U}, {0x0b3cU, 0x0b3cU}, {0x0b3fU, 0x0b3fU}, {0x0b41U, 0x0b44U},
        {0x0b4dU, 0x0b4dU}, {0x0b56U, 0x0b56U}, {0x0b62U, 0x0b63U}, {0x0b82U, 0x0b82U},
        {0x0bc0U, 0x0bc0U}, {0x0bcdU, 0x0bcdU}, {0x0c00U, 0x0c00U}, {0x0c3eU, 0x0c40U},
        {0x0c46U, 0x0c48U}, {0x0c4aU, 0x0c4dU}, {0x0c55U, 0x0c56U}, {0x0c62U, 0x0c63U},
        {0x0c81U, 0x0c81U}, {0x0cbcU, 0x0cbcU}, {0x0cbfU, 0x0cbfU}, {0x0cc6U, 0x0cc6U},
        {0x0cccU, 0x0ccdU}, {0x0ce2U, 0x0ce3U}, {0x0d00U, 0x0d01U}, {0x0d3bU, 0x0d3cU},
        {0x0d41U, 0x0d44U}, {0x0d4dU, 0x0d4dU}, {0x0d62U, 0x0d63U}, {0x0dcaU, 0x0dcaU},
        {0x0dd2U, 0x0dd4U}, {0x0dd6U, 0x0dd6U}, {0x0e31U, 0x0e31U}, {0x0e34U, 0x0e3aU},
        {0x0e47U, 0x0e4eU}, {0x0eb1U, 0x0eb1U}, {0x0eb4U, 0x0ebcU}, {0x0ec8U, 0x0ecdU},
        {0x0f18U, 0x0f19U}, {0x0f35U, 0x0f35U}, {0x0f37U, 0x0f37U}, {0x0f39U, 0x0f39U},
        {0x0f71U, 0x0f7eU}, {0x0f80U, 0x0f84U}, {0x0f86U, 0x0f87U}, {0x0f8dU, 0x0fbcU},
        {0x0fc6U, 0x0fc6U}, {0x102dU, 0x1030U}, {0x1032U, 0x1037U}, {0x1039U, 0x103aU},
        {0x103dU, 0x103eU}, {0x1058U, 0x1059U}, {0x135dU, 0x135fU}, {0x1712U, 0x1714U},
        {0x1732U, 0x1734U}, {0x1752U, 0x1753U}, {0x1772U, 0x1773U}, {0x17b4U, 0x17b5U},
        {0x17b7U, 0x17bdU}, {0x17c6U, 0x17c6U}, {0x17c9U, 0x17d3U}, {0x17ddU, 0x17ddU},
        {0x180bU, 0x180dU}, {0x18a9U, 0x18a9U}, {0x1ab0U, 0x1affU}, {0x1dc0U, 0x1dffU},
        {0x200bU, 0x200fU}, {0x202aU, 0x202eU}, {0x2060U, 0x2064U}, {0x2066U, 0x206fU},
        {0x20d0U, 0x20ffU}, {0x302aU, 0x302fU}, {0x3099U, 0x309aU}, {0xfe00U, 0xfe0fU},
        {0xfe20U, 0xfe2fU}, {0xe0100U, 0xe01efU},
    };

    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(ranges); ++index) {
        if (value < ranges[index].first) {
            return false;
        }
        if (value <= ranges[index].last) {
            return true;
        }
    }
    return false;
}

static bool mereader_tui_codepoint_wide(uint32_t value) {
    return value >= 0x1100U &&
           (value <= 0x115fU || value == 0x2329U || value == 0x232aU ||
            (value >= 0x2e80U && value <= 0xa4cfU && value != 0x303fU) ||
            (value >= 0xac00U && value <= 0xd7a3U) || (value >= 0xf900U && value <= 0xfaffU) ||
            (value >= 0xfe10U && value <= 0xfe19U) || (value >= 0xfe30U && value <= 0xfe6fU) ||
            (value >= 0xff00U && value <= 0xff60U) || (value >= 0xffe0U && value <= 0xffe6U) ||
            (value >= 0x1f1e6U && value <= 0x1f1ffU) || (value >= 0x1f300U && value <= 0x1faffU) ||
            (value >= 0x20000U && value <= 0x3fffdU));
}

static int mereader_tui_codepoint_width(uint32_t value) {
    if (value == 0U || value < 0x20U || (value >= 0x7fU && value < 0xa0U) ||
        mereader_tui_codepoint_combining(value)) {
        return 0;
    }
    return mereader_tui_codepoint_wide(value) ? 2 : 1;
}

size_t mereader_tui_utf8_next(const char *text, size_t length, size_t offset, int *columns) {
    if (columns != nullptr) {
        *columns = 0;
    }
    if (text == nullptr || offset >= length) {
        return length;
    }

    uint32_t codepoint = 0U;
    size_t next = mereader_tui_utf8_decode(text, length, offset, &codepoint);
    if (columns != nullptr) {
        *columns = mereader_tui_codepoint_width(codepoint);
    }
    return next;
}

size_t mereader_tui_utf8_width(const char *text, size_t length) {
    if (text == nullptr) {
        return 0U;
    }

    size_t width = 0U;
    for (size_t offset = 0U; offset < length;) {
        int columns = 0;
        size_t next = mereader_tui_utf8_next(text, length, offset, &columns);
        if (columns > 0) {
            size_t addition = (size_t)columns;
            if (width > SIZE_MAX - addition) {
                return SIZE_MAX;
            }
            width += addition;
        }
        offset = next;
    }
    return width;
}

static unsigned char mereader_tui_ascii_fold(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

int mereader_tui_casecmp(const char *left, const char *right) {
    if (left == right) {
        return 0;
    }
    if (left == nullptr) {
        return -1;
    }
    if (right == nullptr) {
        return 1;
    }

    while (*left != '\0' && *right != '\0') {
        unsigned char folded_left = mereader_tui_ascii_fold((unsigned char)*left);
        unsigned char folded_right = mereader_tui_ascii_fold((unsigned char)*right);
        if (folded_left != folded_right) {
            return folded_left < folded_right ? -1 : 1;
        }
        ++left;
        ++right;
    }
    if (*left == *right) {
        return 0;
    }
    return *left == '\0' ? -1 : 1;
}

bool mereader_tui_contains_casefold(const char *text, const char *query) {
    if (text == nullptr || query == nullptr) {
        return false;
    }
    if (query[0] == '\0') {
        return true;
    }

    for (const char *start = text; *start != '\0'; ++start) {
        const char *left = start;
        const char *right = query;
        while (*left != '\0' && *right != '\0' &&
               mereader_tui_ascii_fold((unsigned char)*left) == mereader_tui_ascii_fold((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}
