#include "mereader-tui/document_backend.h"
#include "mereader-tui/graphics.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <glib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    MEREADER_TUI_COMIC_MAX_ENTRIES = 20000,
    MEREADER_TUI_COMIC_MAX_PAGES = 10000,
    MEREADER_TUI_COMIC_READER_BLOCK = 64 * 1024,
};

#define MEREADER_TUI_COMIC_MAX_PATH_BYTES (8U * 1024U * 1024U)
#define MEREADER_TUI_COMIC_MAX_TOTAL_BYTES (1024ULL * 1024ULL * 1024ULL)
#define MEREADER_TUI_COMIC_URI_PREFIX "comic://page/"

typedef struct MereaderTuiComicPage {
    char *member_path;
    char *label;
    size_t size;
    size_t ordinal;
    char extension[6];
} MereaderTuiComicPage;

typedef struct MereaderTuiComicBackend {
    MereaderTuiComicPage *pages;
    size_t page_count;
    size_t page_capacity;
    size_t path_bytes;
    uint64_t total_bytes;
    struct stat identity;
    int descriptor;
    int archive_format;
} MereaderTuiComicBackend;

typedef struct MereaderTuiComicReader {
    struct archive *archive;
    int descriptor;
} MereaderTuiComicReader;

static bool comic_identity_matches(const struct stat *expected, const struct stat *actual, bool include_change_time) {
    return expected->st_dev == actual->st_dev && expected->st_ino == actual->st_ino &&
           expected->st_mode == actual->st_mode && expected->st_size == actual->st_size &&
           expected->st_mtim.tv_sec == actual->st_mtim.tv_sec && expected->st_mtim.tv_nsec == actual->st_mtim.tv_nsec &&
           (!include_change_time || (expected->st_ctim.tv_sec == actual->st_ctim.tv_sec &&
                                     expected->st_ctim.tv_nsec == actual->st_ctim.tv_nsec));
}

static void comic_set_archive_error(MereaderTuiError *error, MereaderTuiErrorCode code, const char *operation,
                                    struct archive *archive) {
    const char *detail = archive == NULL ? NULL : archive_error_string(archive);
    mereader_tui_error_set(error, code, "%s: %s", operation,
                   detail == NULL || detail[0] == '\0' ? "invalid archive data" : detail);
}

static void comic_reader_close(MereaderTuiComicReader *reader) {
    if (reader->archive != NULL) {
        (void)archive_read_close(reader->archive);
        (void)archive_read_free(reader->archive);
    }
    if (reader->descriptor >= 0) {
        (void)close(reader->descriptor);
    }
    *reader = (MereaderTuiComicReader){.descriptor = -1};
}

static bool comic_reader_open(const MereaderTuiComicBackend *backend, MereaderTuiComicReader *reader, MereaderTuiError *error) {
    *reader = (MereaderTuiComicReader){.descriptor = -1};
    const int descriptor = fcntl(backend->descriptor, F_DUPFD_CLOEXEC, 3);
    if (descriptor < 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not duplicate comic archive: %s", strerror(errno));
        return false;
    }
    if (lseek(descriptor, 0, SEEK_SET) < 0) {
        const int saved_errno = errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not rewind comic archive: %s", strerror(saved_errno));
        return false;
    }

    struct archive *archive = archive_read_new();
    if (archive == NULL) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate comic archive reader");
        return false;
    }
    reader->archive = archive;
    reader->descriptor = descriptor;
    if (archive_read_support_filter_all(archive) < ARCHIVE_WARN ||
        archive_read_support_format_all(archive) < ARCHIVE_WARN ||
        archive_read_open_fd(archive, descriptor, MEREADER_TUI_COMIC_READER_BLOCK) != ARCHIVE_OK) {
        comic_set_archive_error(error, MEREADER_TUI_ERROR_CORRUPT, "could not open comic archive", archive);
        comic_reader_close(reader);
        return false;
    }
    return true;
}

static void comic_page_free(MereaderTuiComicPage *page) {
    free(page->member_path);
    free(page->label);
    *page = (MereaderTuiComicPage){0};
}

static void comic_backend_destroy(MereaderTuiComicBackend *backend) {
    if (backend == NULL) {
        return;
    }
    for (size_t index = 0U; index < backend->page_count; ++index) {
        comic_page_free(&backend->pages[index]);
    }
    free(backend->pages);
    if (backend->descriptor >= 0) {
        (void)close(backend->descriptor);
    }
    free(backend);
}

static unsigned char comic_ascii_fold(unsigned char value) {
    return value >= (unsigned char)'A' && value <= (unsigned char)'Z'
               ? (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'))
               : value;
}

static int comic_compare_digit_runs(const unsigned char **left_cursor, const unsigned char **right_cursor) {
    const unsigned char *left = *left_cursor;
    const unsigned char *right = *right_cursor;
    const unsigned char *left_end = left;
    const unsigned char *right_end = right;
    while (*left_end >= (unsigned char)'0' && *left_end <= (unsigned char)'9') {
        ++left_end;
    }
    while (*right_end >= (unsigned char)'0' && *right_end <= (unsigned char)'9') {
        ++right_end;
    }
    const unsigned char *left_digits = left;
    const unsigned char *right_digits = right;
    while (left_digits + 1 < left_end && *left_digits == (unsigned char)'0') {
        ++left_digits;
    }
    while (right_digits + 1 < right_end && *right_digits == (unsigned char)'0') {
        ++right_digits;
    }
    const size_t left_length = (size_t)(left_end - left_digits);
    const size_t right_length = (size_t)(right_end - right_digits);
    int result = 0;
    if (left_length != right_length) {
        result = left_length < right_length ? -1 : 1;
    } else {
        result = memcmp(left_digits, right_digits, left_length);
        if (result == 0) {
            const size_t left_run = (size_t)(left_end - left);
            const size_t right_run = (size_t)(right_end - right);
            if (left_run != right_run) {
                result = left_run < right_run ? -1 : 1;
            }
        }
    }
    *left_cursor = left_end;
    *right_cursor = right_end;
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

static int comic_natural_path_compare(const char *left_value, const char *right_value) {
    const unsigned char *left = (const unsigned char *)left_value;
    const unsigned char *right = (const unsigned char *)right_value;
    while (*left != '\0' && *right != '\0') {
        if (*left >= (unsigned char)'0' && *left <= (unsigned char)'9' && *right >= (unsigned char)'0' &&
            *right <= (unsigned char)'9') {
            const int numeric = comic_compare_digit_runs(&left, &right);
            if (numeric != 0) {
                return numeric;
            }
            continue;
        }
        const unsigned char folded_left = comic_ascii_fold(*left);
        const unsigned char folded_right = comic_ascii_fold(*right);
        if (folded_left != folded_right) {
            return folded_left < folded_right ? -1 : 1;
        }
        ++left;
        ++right;
    }
    if (*left != *right) {
        return *left == '\0' ? -1 : 1;
    }
    return strcmp(left_value, right_value);
}

static int comic_page_compare(const void *left_pointer, const void *right_pointer) {
    const MereaderTuiComicPage *left = left_pointer;
    const MereaderTuiComicPage *right = right_pointer;
    const int path_result = comic_natural_path_compare(left->member_path, right->member_path);
    if (path_result != 0) {
        return path_result;
    }
    return left->ordinal < right->ordinal ? -1 : left->ordinal > right->ordinal ? 1 : 0;
}

static bool comic_path_is_safe(const char *path, size_t length) {
    if (length == 0U || path[0] == '/' || path[0] == '\\' || (length >= 2U && path[1] == ':')) {
        return false;
    }
    size_t component = 0U;
    for (size_t index = 0U; index <= length; ++index) {
        const unsigned char value = index == length ? (unsigned char)'/' : (unsigned char)path[index];
        if (value < 0x20U || value == 0x7fU) {
            return false;
        }
        if (value != (unsigned char)'/' && value != (unsigned char)'\\') {
            continue;
        }
        const size_t component_length = index - component;
        if (component_length == 0U || (component_length == 1U && path[component] == '.') ||
            (component_length == 2U && path[component] == '.' && path[component + 1U] == '.')) {
            return false;
        }
        component = index + 1U;
    }
    return true;
}

static const char *comic_supported_extension(const char *path) {
    const char *extension = mereader_tui_path_extension(path);
    if (extension == NULL) {
        return NULL;
    }
    static const char *const supported[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg",
    };
    for (size_t index = 0U; index < MEREADER_TUI_ARRAY_LEN(supported); ++index) {
        if (mereader_tui_casecmp(extension, supported[index]) == 0) {
            return supported[index];
        }
    }
    return NULL;
}

static const char *comic_mime_type(const char *extension) {
    if (strcmp(extension, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(extension, ".jpg") == 0 || strcmp(extension, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(extension, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(extension, ".webp") == 0) {
        return "image/webp";
    }
    if (strcmp(extension, ".bmp") == 0) {
        return "image/bmp";
    }
    return "image/svg+xml";
}

static char *comic_page_label(const char *path, MereaderTuiError *error) {
    char *basename = mereader_tui_path_basename(path, error);
    if (basename == NULL) {
        return NULL;
    }
    gchar *valid = g_utf8_make_valid(basename, -1);
    free(basename);
    if (valid == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not normalize comic page name");
        return NULL;
    }
    char *label = mereader_tui_strdup(valid[0] == '\0' ? "page" : valid, error);
    g_free(valid);
    return label;
}

static bool comic_add_page(MereaderTuiComicBackend *backend, const char *path, size_t path_length, const char *extension,
                           size_t size, size_t ordinal, MereaderTuiError *error) {
    if (backend->page_count >= MEREADER_TUI_COMIC_MAX_PAGES) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic contains more than %d image pages", MEREADER_TUI_COMIC_MAX_PAGES);
        return false;
    }
    if (path_length + 1U > MEREADER_TUI_COMIC_MAX_PATH_BYTES - backend->path_bytes) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic member names exceed the supported size limit");
        return false;
    }
    if ((uint64_t)size > MEREADER_TUI_COMIC_MAX_TOTAL_BYTES - backend->total_bytes) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic image data exceeds the 1 GiB declared size limit");
        return false;
    }

    MereaderTuiError reserve_error = {0};
    MereaderTuiComicPage *pages = mereader_tui_array_reserve(backend->pages, &backend->page_capacity, sizeof(*backend->pages),
                                              backend->page_count + 1U, &reserve_error);
    if (mereader_tui_error_is_set(&reserve_error)) {
        if (error != NULL) {
            *error = reserve_error;
        }
        return false;
    }
    backend->pages = pages;
    MereaderTuiComicPage page = {
        .member_path = mereader_tui_strndup(path, path_length, error),
        .label = comic_page_label(path, error),
        .size = size,
        .ordinal = ordinal,
    };
    (void)snprintf(page.extension, sizeof(page.extension), "%s", extension);
    if (page.member_path == NULL || page.label == NULL) {
        comic_page_free(&page);
        return false;
    }
    backend->pages[backend->page_count++] = page;
    backend->path_bytes += path_length + 1U;
    backend->total_bytes += (uint64_t)size;
    return true;
}

static bool comic_supported_archive_format(int format) {
    const int base = format & ARCHIVE_FORMAT_BASE_MASK;
    return base == ARCHIVE_FORMAT_ZIP || base == ARCHIVE_FORMAT_RAR || base == ARCHIVE_FORMAT_RAR_V5 ||
           base == ARCHIVE_FORMAT_7ZIP;
}

static bool comic_enumerate(MereaderTuiComicBackend *backend, MereaderTuiError *error) {
    MereaderTuiComicReader reader = {.descriptor = -1};
    if (!comic_reader_open(backend, &reader, error)) {
        return false;
    }
    size_t entry_count = 0U;
    bool success = true;
    for (;;) {
        struct archive_entry *entry = NULL;
        const int status = archive_read_next_header(reader.archive, &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status != ARCHIVE_OK || entry == NULL) {
            comic_set_archive_error(error, MEREADER_TUI_ERROR_CORRUPT, "could not read comic archive header", reader.archive);
            success = false;
            break;
        }
        const size_t ordinal = entry_count++;
        if (entry_count > MEREADER_TUI_COMIC_MAX_ENTRIES) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic contains more than %d archive entries",
                           MEREADER_TUI_COMIC_MAX_ENTRIES);
            success = false;
            break;
        }
        if (archive_entry_is_encrypted(entry) > 0) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED, "encrypted comic archives are not supported");
            success = false;
            break;
        }
        const char *path = archive_entry_pathname(entry);
        if (path == NULL || archive_entry_filetype(entry) != AE_IFREG || archive_entry_symlink(entry) != NULL ||
            archive_entry_hardlink(entry) != NULL) {
            continue;
        }
        const size_t path_length = strnlen(path, MEREADER_TUI_COMIC_MAX_PATH_BYTES + 1U);
        const char *extension = path_length > MEREADER_TUI_COMIC_MAX_PATH_BYTES ? NULL : comic_supported_extension(path);
        if (extension == NULL || !comic_path_is_safe(path, path_length)) {
            continue;
        }
        if (archive_entry_size_is_set(entry) == 0) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic page has no declared size: %s", path);
            success = false;
            break;
        }
        const la_int64_t declared_size = archive_entry_size(entry);
        if (declared_size < 0 || (uint64_t)declared_size > MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic page exceeds the 16 MiB image limit: %s", path);
            success = false;
            break;
        }
        if (!comic_add_page(backend, path, path_length, extension, (size_t)declared_size, ordinal, error)) {
            success = false;
            break;
        }
    }
    if (success && archive_read_has_encrypted_entries(reader.archive) > 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED, "encrypted comic archives are not supported");
        success = false;
    }
    if (success) {
        backend->archive_format = archive_format(reader.archive);
        if (!comic_supported_archive_format(backend->archive_format)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED, "comic file is not a ZIP, RAR, or 7-Zip archive");
            success = false;
        }
    }
    comic_reader_close(&reader);

    struct stat after;
    if (success &&
        (fstat(backend->descriptor, &after) != 0 || !comic_identity_matches(&backend->identity, &after, true))) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic archive changed while opening");
        success = false;
    }
    if (success && backend->page_count == 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic archive contains no supported image pages");
        success = false;
    }
    if (success) {
        qsort(backend->pages, backend->page_count, sizeof(*backend->pages), comic_page_compare);
    }
    return success;
}

static MereaderTuiComicBackend *comic_backend_open(const char *path, const struct stat *expected_identity, MereaderTuiError *error) {
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not open comic archive '%s': %s", path, strerror(errno));
        return NULL;
    }
    struct stat identity;
    if (fstat(descriptor, &identity) != 0) {
        const int saved_errno = errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect comic archive '%s': %s", path, strerror(saved_errno));
        return NULL;
    }
    if (!S_ISREG(identity.st_mode) || identity.st_size < 0 ||
        !comic_identity_matches(expected_identity, &identity, true)) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic archive changed between format detection and open: %s", path);
        return NULL;
    }
    MereaderTuiComicBackend *backend = calloc(1U, sizeof(*backend));
    if (backend == NULL) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate comic archive backend");
        return NULL;
    }
    backend->descriptor = descriptor;
    backend->identity = identity;
    if (!comic_enumerate(backend, error)) {
        comic_backend_destroy(backend);
        return NULL;
    }
    return backend;
}

static bool comic_format_page_uri(size_t page_index, const char *extension, char output[64]) {
    const int length = snprintf(output, 64U, MEREADER_TUI_COMIC_URI_PREFIX "%zu%s", page_index, extension);
    return length > 0 && length < 64;
}

static bool comic_format_page_target(size_t page_index, char output[64]) {
    const int length = snprintf(output, 64U, MEREADER_TUI_COMIC_URI_PREFIX "%zu", page_index);
    return length > 0 && length < 64;
}

static const MereaderTuiComicPage *comic_page_for_uri(const MereaderTuiComicBackend *backend, const char *uri) {
    const size_t prefix_length = sizeof(MEREADER_TUI_COMIC_URI_PREFIX) - 1U;
    if (uri == NULL || strncmp(uri, MEREADER_TUI_COMIC_URI_PREFIX, prefix_length) != 0) {
        return NULL;
    }
    errno = 0;
    char *end = NULL;
    const uintmax_t parsed = strtoumax(uri + prefix_length, &end, 10);
    if (errno == ERANGE || end == uri + prefix_length || parsed > (uintmax_t)SIZE_MAX ||
        (size_t)parsed >= backend->page_count) {
        return NULL;
    }
    const MereaderTuiComicPage *page = &backend->pages[(size_t)parsed];
    char expected[64] = {0};
    return comic_format_page_uri((size_t)parsed, page->extension, expected) && strcmp(uri, expected) == 0 ? page : NULL;
}

static bool comic_resource_size(const MereaderTuiDocument *document, const char *uri, size_t *size) {
    if (document == NULL || size == NULL || document->backend == NULL) {
        return false;
    }
    const MereaderTuiComicPage *page = comic_page_for_uri(document->backend, uri);
    if (page == NULL) {
        return false;
    }
    *size = page->size;
    return true;
}

static bool comic_read_page(MereaderTuiComicBackend *backend, const MereaderTuiComicPage *page, unsigned char **data,
                            MereaderTuiError *error) {
    struct stat before;
    if (fstat(backend->descriptor, &before) != 0 || !comic_identity_matches(&backend->identity, &before, false)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic archive changed before reading a page");
        return false;
    }
    MereaderTuiComicReader reader = {.descriptor = -1};
    if (!comic_reader_open(backend, &reader, error)) {
        return false;
    }
    bool found = false;
    bool success = true;
    for (size_t ordinal = 0U; ordinal <= page->ordinal; ++ordinal) {
        struct archive_entry *entry = NULL;
        const int status = archive_read_next_header(reader.archive, &entry);
        if (status != ARCHIVE_OK || entry == NULL) {
            comic_set_archive_error(error, MEREADER_TUI_ERROR_CORRUPT, "could not locate comic page", reader.archive);
            success = false;
            break;
        }
        if (ordinal != page->ordinal) {
            continue;
        }
        const char *path = archive_entry_pathname(entry);
        if (path == NULL || strcmp(path, page->member_path) != 0 || archive_entry_size(entry) < 0 ||
            (uint64_t)archive_entry_size(entry) != page->size) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic page index changed while reading");
            success = false;
            break;
        }
        if (archive_entry_is_encrypted(entry) > 0) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED, "encrypted comic pages are not supported");
            success = false;
            break;
        }
        unsigned char *buffer = malloc(page->size == 0U ? 1U : page->size);
        if (buffer == NULL) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate comic page data");
            success = false;
            break;
        }
        size_t offset = 0U;
        while (offset < page->size) {
            const la_ssize_t count = archive_read_data(reader.archive, buffer + offset, page->size - offset);
            if (count > 0) {
                offset += (size_t)count;
            } else if (count == 0) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic page data is truncated: %s", page->member_path);
                success = false;
                break;
            } else {
                comic_set_archive_error(error, MEREADER_TUI_ERROR_CORRUPT, "could not decompress comic page", reader.archive);
                success = false;
                break;
            }
        }
        unsigned char extra = 0U;
        if (success && archive_read_data(reader.archive, &extra, 1U) != 0) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic page exceeds its declared size: %s", page->member_path);
            success = false;
        }
        if (success) {
            *data = buffer;
            found = true;
        } else {
            free(buffer);
        }
        break;
    }
    comic_reader_close(&reader);
    struct stat after;
    if (success && found &&
        (fstat(backend->descriptor, &after) != 0 || !comic_identity_matches(&before, &after, false))) {
        free(*data);
        *data = NULL;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "comic archive changed while reading a page");
        success = false;
    }
    if (success && !found) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_NOT_FOUND, "comic page was not found");
        success = false;
    }
    return success && found;
}

static bool comic_load_resource(MereaderTuiDocument *document, const char *uri, MereaderTuiResource *resource, MereaderTuiError *error) {
    MereaderTuiComicBackend *backend = document->backend;
    if (backend == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "comic archive backend is closed");
        return false;
    }
    const MereaderTuiComicPage *page = comic_page_for_uri(backend, uri);
    if (page == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_NOT_FOUND, "comic page resource not found: %s", uri);
        return false;
    }
    unsigned char *data = NULL;
    if (!comic_read_page(backend, page, &data, error)) {
        return false;
    }
    char *mime_type = mereader_tui_strdup(comic_mime_type(page->extension), error);
    if (mime_type == NULL) {
        free(data);
        return false;
    }
    resource->data = data;
    resource->length = page->size;
    resource->mime_type = mime_type;
    return true;
}

static void comic_close(MereaderTuiDocument *document) {
    comic_backend_destroy(document->backend);
    document->backend = NULL;
}

static const MereaderTuiDocumentOps comic_document_ops = {
    .load_resource = comic_load_resource,
    .resource_size = comic_resource_size,
    .close = comic_close,
};

static const char *comic_format_name(int format) {
    const int base = format & ARCHIVE_FORMAT_BASE_MASK;
    if (base == ARCHIVE_FORMAT_ZIP) {
        return "CBZ";
    }
    if (base == ARCHIVE_FORMAT_7ZIP) {
        return "CB7";
    }
    return "CBR";
}

bool mereader_tui_comic_open(MereaderTuiDocument *document, const char *path, const struct stat *expected_identity, MereaderTuiError *error) {
    if (document == NULL || path == NULL || path[0] == '\0' || document->path == NULL ||
        strcmp(path, document->path) != 0 || expected_identity == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                       "document, canonical comic path, and detected identity are required");
        return false;
    }
    MereaderTuiComicBackend *backend = comic_backend_open(path, expected_identity, error);
    if (backend == NULL) {
        return false;
    }
    document->format = MEREADER_TUI_FORMAT_COMIC;
    document->backend = backend;
    document->ops = &comic_document_ops;
    document->metadata.title = mereader_tui_path_basename(path, error);
    document->metadata.format = mereader_tui_strdup(comic_format_name(backend->archive_format), error);
    if (document->metadata.title == NULL || document->metadata.format == NULL ||
        !mereader_tui_document_account_metadata(document, error)) {
        return false;
    }

    for (size_t page_index = 0U; page_index < backend->page_count; ++page_index) {
        const MereaderTuiComicPage *page = &backend->pages[page_index];
        char target_buffer[64] = {0};
        char uri_buffer[64] = {0};
        if (!comic_format_page_target(page_index, target_buffer) ||
            !comic_format_page_uri(page_index, page->extension, uri_buffer)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "could not format comic page URI");
            return false;
        }
        MereaderTuiBlock block = {
            .kind = MEREADER_TUI_BLOCK_IMAGE,
            .section_id = mereader_tui_strdup(target_buffer, error),
            .value.image =
                {
                    .uri = mereader_tui_strdup(uri_buffer, error),
                    .alt = mereader_tui_strdup(page->label, error),
                    .anchor = mereader_tui_strdup(target_buffer, error),
                    .page_index = -1,
                },
        };
        const size_t first_block = document->block_count;
        if (block.section_id == NULL || block.value.image.uri == NULL || block.value.image.alt == NULL ||
            block.value.image.anchor == NULL || !mereader_tui_document_add_image_block(document, &block, error)) {
            mereader_tui_document_block_free(&block);
            return false;
        }
        MereaderTuiSection section = {
            .id = mereader_tui_strdup(target_buffer, error),
            .first_block = first_block,
            .block_count = 1U,
            .source_size = page->size,
            .linear = true,
        };
        if (section.id == NULL || !mereader_tui_document_add_section(document, &section, error)) {
            free(section.id);
            return false;
        }
        if (!mereader_tui_document_add_toc(document, page->label, target_buffer, 0U, error)) {
            return false;
        }
    }
    return mereader_tui_document_index_toc_sections(document, error);
}
