#include "mereader-tui/catalog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEREADER_TUI_CATALOG_PAGE_SIZE 4096U
#define MEREADER_TUI_CATALOG_MAX_DEPTH 256U

typedef struct CatalogMapSlot {
    const char *key;
    size_t value;
} CatalogMapSlot;

typedef struct CatalogMap {
    CatalogMapSlot *slots;
    size_t capacity;
    size_t length;
} CatalogMap;

static uint64_t catalog_hash(const char *value) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool catalog_map_init(CatalogMap *map, size_t item_count, MereaderTuiError *error) {
    size_t capacity = 16U;
    const size_t needed = item_count > SIZE_MAX / 2U ? SIZE_MAX : item_count * 2U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "library map is too large");
            return false;
        }
        capacity *= 2U;
    }
    map->slots = mereader_tui_reallocarray(NULL, capacity, sizeof(*map->slots), error);
    if (map->slots == NULL) {
        return false;
    }
    memset(map->slots, 0, capacity * sizeof(*map->slots));
    map->capacity = capacity;
    return true;
}

static bool catalog_map_lookup(const CatalogMap *map, const char *key, size_t *value) {
    if (map->capacity == 0U) {
        return false;
    }
    size_t slot = (size_t)(catalog_hash(key) & (uint64_t)(map->capacity - 1U));
    while (map->slots[slot].key != NULL) {
        if (strcmp(map->slots[slot].key, key) == 0) {
            if (value != NULL) {
                *value = map->slots[slot].value;
            }
            return true;
        }
        slot = (slot + 1U) & (map->capacity - 1U);
    }
    return false;
}

static bool catalog_map_grow(CatalogMap *map, MereaderTuiError *error) {
    if (map->capacity > SIZE_MAX / 2U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "library map is too large");
        return false;
    }
    const size_t capacity = map->capacity * 2U;
    CatalogMapSlot *slots = mereader_tui_reallocarray(NULL, capacity, sizeof(*slots), error);
    if (slots == NULL) {
        return false;
    }
    memset(slots, 0, capacity * sizeof(*slots));
    for (size_t index = 0U; index < map->capacity; ++index) {
        if (map->slots[index].key == NULL) {
            continue;
        }
        size_t slot = (size_t)(catalog_hash(map->slots[index].key) & (uint64_t)(capacity - 1U));
        while (slots[slot].key != NULL) {
            slot = (slot + 1U) & (capacity - 1U);
        }
        slots[slot] = map->slots[index];
    }
    free(map->slots);
    map->slots = slots;
    map->capacity = capacity;
    return true;
}

static bool catalog_map_insert(CatalogMap *map, const char *key, size_t value, MereaderTuiError *error) {
    if (map->capacity == 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "library map is not initialized");
        return false;
    }
    if (map->length >= map->capacity / 2U && !catalog_map_grow(map, error)) {
        return false;
    }
    size_t slot = (size_t)(catalog_hash(key) & (uint64_t)(map->capacity - 1U));
    size_t inspected = 0U;
    while (map->slots[slot].key != NULL) {
        if (strcmp(map->slots[slot].key, key) == 0) {
            map->slots[slot].value = value;
            return true;
        }
        slot = (slot + 1U) & (map->capacity - 1U);
        if (++inspected == map->capacity) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "library map is full");
            return false;
        }
    }
    map->slots[slot] = (CatalogMapSlot){.key = key, .value = value};
    ++map->length;
    return true;
}

static void catalog_map_free(CatalogMap *map) {
    free(map->slots);
    *map = (CatalogMap){0};
}

static const char *catalog_supported_extension(const char *path) {
    static const char *extensions[] = {
        ".epub", ".epub3", ".pdf", ".mobi", ".prc", ".azw", ".azw3", ".azw4",
        ".fb2",  ".txt",   ".md",  ".cbz",  ".cbr", ".cb7",
    };
    const char *extension = mereader_tui_path_extension(path);
    for (size_t index = 0U; extension != NULL && index < MEREADER_TUI_ARRAY_LEN(extensions); ++index) {
        if (mereader_tui_casecmp(extension, extensions[index]) == 0) {
            return extensions[index];
        }
    }
    return NULL;
}

static size_t catalog_format_rank(const char *name) {
    if (mereader_tui_casecmp(name, "epub") == 0 || mereader_tui_casecmp(name, "epub3") == 0) {
        return 0U;
    }
    if (mereader_tui_casecmp(name, "pdf") == 0) {
        return 1U;
    }
    if (mereader_tui_casecmp(name, "mobi") == 0 || mereader_tui_casecmp(name, "azw") == 0 ||
        mereader_tui_casecmp(name, "azw3") == 0 || mereader_tui_casecmp(name, "azw4") == 0 ||
        mereader_tui_casecmp(name, "prc") == 0) {
        return 2U;
    }
    return 3U;
}

static char *catalog_format_name(const char *extension, MereaderTuiError *error) {
    const char *start = extension[0] == '.' ? extension + 1 : extension;
    char *name = mereader_tui_strdup(start, error);
    if (name == NULL) {
        return NULL;
    }
    for (char *cursor = name; *cursor != '\0'; ++cursor) {
        *cursor = (char)toupper((unsigned char)*cursor);
    }
    return name;
}

static char *catalog_stem(const char *path, MereaderTuiError *error) {
    char *basename = mereader_tui_path_basename(path, error);
    if (basename == NULL) {
        return NULL;
    }
    char *extension = strrchr(basename, '.');
    if (extension != NULL) {
        *extension = '\0';
    }
    return basename;
}

static char *catalog_calibre_title(const char *directory, MereaderTuiError *error) {
    char *title = mereader_tui_path_basename(directory, error);
    if (title == NULL) {
        return NULL;
    }
    char *suffix = strrchr(title, '(');
    if (suffix == NULL || suffix == title || suffix[-1] != ' ') {
        return title;
    }
    char *cursor = suffix + 1;
    if (!isdigit((unsigned char)*cursor)) {
        return title;
    }
    while (isdigit((unsigned char)*cursor)) {
        ++cursor;
    }
    if (cursor[0] == ')' && cursor[1] == '\0') {
        suffix[-1] = '\0';
    }
    return title;
}

static char *catalog_calibre_author(const char *directory, MereaderTuiError *error) {
    char *parent = mereader_tui_path_dirname(directory, error);
    if (parent == NULL) {
        return NULL;
    }
    char *author = strcmp(parent, ".") == 0 ? mereader_tui_strdup("", error) : mereader_tui_path_basename(parent, error);
    free(parent);
    return author;
}

static bool catalog_detect_calibre(const char *root, MereaderTuiError *error) {
    char *metadata = mereader_tui_path_join(root, "metadata.db", error);
    if (metadata == NULL) {
        return false;
    }
    struct stat status;
    const bool calibre = stat(metadata, &status) == 0 && S_ISREG(status.st_mode);
    free(metadata);
    return calibre;
}

static void catalog_format_free(MereaderTuiCatalogFormat *format) {
    free(format->path);
    free(format->relative_path);
    free(format->name);
    free(format->last_read);
    *format = (MereaderTuiCatalogFormat){0};
}

static void catalog_book_free(MereaderTuiCatalogBook *book) {
    free(book->title);
    free(book->author);
    free(book->directory);
    free(book->group_key);
    for (size_t index = 0U; index < book->format_count; ++index) {
        catalog_format_free(&book->formats[index]);
    }
    free(book->formats);
    *book = (MereaderTuiCatalogBook){0};
}

static void catalog_books_free(MereaderTuiCatalog *catalog) {
    for (size_t index = 0U; index < catalog->length; ++index) {
        catalog_book_free(&catalog->books[index]);
    }
    free(catalog->books);
    catalog->books = NULL;
    catalog->length = 0U;
    catalog->capacity = 0U;
}

void mereader_tui_catalog_close(MereaderTuiCatalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    catalog_books_free(catalog);
    mereader_tui_search_index_close(&catalog->search);
    *catalog = (MereaderTuiCatalog){0};
}

bool mereader_tui_catalog_is_open(const MereaderTuiCatalog *catalog) {
    return catalog != NULL && catalog->search.handle != NULL;
}

static bool catalog_add_book(MereaderTuiCatalog *catalog, const char *key, const char *relative_path, CatalogMap *groups,
                             size_t *book_index, MereaderTuiError *error) {
    if (catalog_map_lookup(groups, key, book_index)) {
        return true;
    }
    MereaderTuiCatalogBook *books = mereader_tui_array_reserve(catalog->books, &catalog->capacity, sizeof(*catalog->books),
                                                catalog->length + 1U, error);
    if (books == NULL) {
        return false;
    }
    catalog->books = books;

    char *directory = mereader_tui_path_dirname(relative_path, error);
    char *title = NULL;
    char *author = NULL;
    char *group_key = mereader_tui_strdup(key, error);
    if (directory != NULL && catalog->calibre) {
        title = catalog_calibre_title(directory, error);
        author = title == NULL ? NULL : catalog_calibre_author(directory, error);
    } else if (directory != NULL) {
        title = catalog_stem(relative_path, error);
        author = title == NULL ? NULL : mereader_tui_strdup("", error);
    }
    if (directory == NULL || title == NULL || author == NULL || group_key == NULL) {
        free(directory);
        free(title);
        free(author);
        free(group_key);
        return false;
    }

    *book_index = catalog->length;
    catalog->books[catalog->length++] = (MereaderTuiCatalogBook){
        .title = title,
        .author = author,
        .directory = directory,
        .group_key = group_key,
    };
    return catalog_map_insert(groups, catalog->books[*book_index].group_key, *book_index, error);
}

static bool catalog_add_format(MereaderTuiCatalog *catalog, const char *relative_path, const char *extension,
                               CatalogMap *groups, MereaderTuiError *error) {
    char *directory = mereader_tui_path_dirname(relative_path, error);
    char *stem = directory == NULL ? NULL : catalog_stem(relative_path, error);
    char *key = NULL;
    if (directory != NULL && stem != NULL) {
        key = catalog->calibre ? mereader_tui_strdup(directory, error) : mereader_tui_path_join(directory, stem, error);
    }
    free(directory);
    free(stem);
    if (key == NULL) {
        return false;
    }

    size_t book_index = 0U;
    const bool book_ready = catalog_add_book(catalog, key, relative_path, groups, &book_index, error);
    free(key);
    if (!book_ready) {
        return false;
    }
    MereaderTuiCatalogBook *book = &catalog->books[book_index];
    MereaderTuiCatalogFormat *formats = mereader_tui_array_reserve(book->formats, &book->format_capacity, sizeof(*book->formats),
                                                    book->format_count + 1U, error);
    if (formats == NULL) {
        return false;
    }
    book->formats = formats;

    char *path = mereader_tui_path_join(catalog->search.root, relative_path, error);
    char *stored_relative = path == NULL ? NULL : mereader_tui_strdup(relative_path, error);
    char *name = stored_relative == NULL ? NULL : catalog_format_name(extension, error);
    if (path == NULL || stored_relative == NULL || name == NULL) {
        free(path);
        free(stored_relative);
        free(name);
        return false;
    }
    const size_t new_rank = catalog_format_rank(name);
    if (book->format_count == 0U || new_rank < catalog_format_rank(book->formats[book->preferred_format].name)) {
        book->preferred_format = book->format_count;
    }
    book->formats[book->format_count++] = (MereaderTuiCatalogFormat){
        .path = path,
        .relative_path = stored_relative,
        .name = name,
    };
    return true;
}

static int catalog_compare_books(const void *left_pointer, const void *right_pointer) {
    const MereaderTuiCatalogBook *left = left_pointer;
    const MereaderTuiCatalogBook *right = right_pointer;
    const int author = mereader_tui_casecmp(left->author, right->author);
    if (author != 0) {
        return author;
    }
    const int title = mereader_tui_casecmp(left->title, right->title);
    return title == 0 ? strcmp(left->directory, right->directory) : title;
}

static int catalog_compare_formats(const void *left_pointer, const void *right_pointer) {
    const MereaderTuiCatalogFormat *left = left_pointer;
    const MereaderTuiCatalogFormat *right = right_pointer;
    const size_t left_rank = catalog_format_rank(left->name);
    const size_t right_rank = catalog_format_rank(right->name);
    if (left_rank != right_rank) {
        return left_rank < right_rank ? -1 : 1;
    }
    const int name = mereader_tui_casecmp(left->name, right->name);
    return name == 0 ? strcmp(left->relative_path, right->relative_path) : name;
}

static bool catalog_walk_directory(MereaderTuiCatalog *catalog, int descriptor, const char *relative_directory, size_t depth,
                                   CatalogMap *groups, MereaderTuiError *error) {
    if (depth > MEREADER_TUI_CATALOG_MAX_DEPTH) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "library nesting exceeds %u directories",
                       (unsigned)MEREADER_TUI_CATALOG_MAX_DEPTH);
        return false;
    }
    DIR *stream = fdopendir(descriptor);
    if (stream == NULL) {
        const int status = errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not scan library directory '%s': %s", relative_directory,
                       strerror(status));
        return false;
    }

    bool success = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (entry == NULL) {
            if (errno != 0) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not finish scanning library directory '%s': %s",
                               relative_directory, strerror(errno));
                success = false;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *relative_path = relative_directory[0] == '\0' ? mereader_tui_strdup(entry->d_name, error)
                                                             : mereader_tui_path_join(relative_directory, entry->d_name, error);
        if (relative_path == NULL) {
            free(relative_path);
            success = false;
            break;
        }
        struct stat status;
        if (fstatat(dirfd(stream), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
            const int stat_error = errno;
            free(relative_path);
            if (stat_error == EACCES || stat_error == ENOENT) {
                continue;
            }
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect library entry '%s': %s", entry->d_name,
                           strerror(stat_error));
            success = false;
            break;
        }
        if (S_ISDIR(status.st_mode)) {
            const int child = openat(dirfd(stream), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child >= 0) {
                success = catalog_walk_directory(catalog, child, relative_path, depth + 1U, groups, error);
            } else if (errno != EACCES && errno != ENOENT && errno != ELOOP) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not open library directory '%s': %s", relative_path,
                               strerror(errno));
                success = false;
            }
        } else if (S_ISREG(status.st_mode)) {
            const char *extension = catalog_supported_extension(relative_path);
            success = extension == NULL || catalog_add_format(catalog, relative_path, extension, groups, error);
        }
        free(relative_path);
        if (!success) {
            break;
        }
    }
    if (closedir(stream) != 0 && success) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close library directory '%s': %s", relative_directory,
                       strerror(errno));
        success = false;
    }
    return success;
}

static bool catalog_collect_files(MereaderTuiCatalog *catalog, MereaderTuiError *error) {
    CatalogMap groups = {0};
    const int root = open(catalog->search.root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not open library root '%s': %s", catalog->search.root,
                       strerror(errno));
        return false;
    }
    if (!catalog_map_init(&groups, 0U, error)) {
        (void)close(root);
        return false;
    }
    if (!catalog_walk_directory(catalog, root, "", 0U, &groups, error)) {
        catalog_map_free(&groups);
        return false;
    }
    catalog_map_free(&groups);
    for (size_t index = 0U; index < catalog->length; ++index) {
        MereaderTuiCatalogBook *book = &catalog->books[index];
        if (book->format_count > 1U) {
            qsort(book->formats, book->format_count, sizeof(*book->formats), catalog_compare_formats);
        }
        book->preferred_format = 0U;
    }
    if (catalog->length > 1U) {
        qsort(catalog->books, catalog->length, sizeof(*catalog->books), catalog_compare_books);
    }
    return true;
}

static bool catalog_apply_history(MereaderTuiCatalog *catalog, const MereaderTuiHistory *history, MereaderTuiError *error) {
    size_t format_count = 0U;
    for (size_t index = 0U; index < catalog->length; ++index) {
        if (format_count > SIZE_MAX - catalog->books[index].format_count) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "library format count overflow");
            return false;
        }
        format_count += catalog->books[index].format_count;
    }
    CatalogMap paths = {0};
    MereaderTuiCatalogFormat **formats = format_count == 0U ? NULL : mereader_tui_reallocarray(NULL, format_count, sizeof(*formats),
                                                                                error);
    if (!catalog_map_init(&paths, format_count, error) || (format_count > 0U && formats == NULL)) {
        free(formats);
        catalog_map_free(&paths);
        return false;
    }
    size_t flat_index = 0U;
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        MereaderTuiCatalogBook *book = &catalog->books[book_index];
        for (size_t format_index = 0U; format_index < book->format_count; ++format_index) {
            formats[flat_index] = &book->formats[format_index];
            if (!catalog_map_insert(&paths, book->formats[format_index].path, flat_index++, error)) {
                free(formats);
                catalog_map_free(&paths);
                return false;
            }
        }
    }
    for (size_t history_index = 0U; history != NULL && history_index < history->length; ++history_index) {
        const MereaderTuiHistoryEntry *entry = &history->items[history_index];
        size_t target = 0U;
        if (entry->filepath == NULL || !catalog_map_lookup(&paths, entry->filepath, &target)) {
            continue;
        }
        MereaderTuiCatalogFormat *format = formats[target];
        format->reading_progress = entry->reading_progress;
        free(format->last_read);
        format->last_read = entry->last_read == NULL ? NULL : mereader_tui_strdup(entry->last_read, error);
        if (entry->last_read != NULL && format->last_read == NULL) {
            free(formats);
            catalog_map_free(&paths);
            return false;
        }
    }
    free(formats);
    catalog_map_free(&paths);
    return true;
}

bool mereader_tui_catalog_open(MereaderTuiCatalog *catalog, const char *root, const MereaderTuiHistory *history, bool watch,
                       MereaderTuiError *error) {
    if (catalog == NULL || root == NULL || root[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid library catalog root");
        return false;
    }
    if (catalog->search.handle != NULL || catalog->books != NULL || catalog->length != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "library catalog output is not empty");
        return false;
    }
    if (!mereader_tui_search_index_open(&catalog->search, root, watch, error)) {
        return false;
    }
    catalog->calibre = catalog_detect_calibre(catalog->search.root, error);
    if (mereader_tui_error_is_set(error) || !catalog_collect_files(catalog, error) ||
        !catalog_apply_history(catalog, history, error)) {
        mereader_tui_catalog_close(catalog);
        return false;
    }
    return true;
}

bool mereader_tui_catalog_refresh(MereaderTuiCatalog *catalog, const MereaderTuiHistory *history, MereaderTuiError *error) {
    if (catalog == NULL || catalog->search.handle == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "library catalog is not open");
        return false;
    }
    if (!mereader_tui_search_index_refresh(&catalog->search, error)) {
        return false;
    }
    catalog_books_free(catalog);
    catalog->calibre = catalog_detect_calibre(catalog->search.root, error);
    if (mereader_tui_error_is_set(error) || !catalog_collect_files(catalog, error) ||
        !catalog_apply_history(catalog, history, error)) {
        catalog_books_free(catalog);
        return false;
    }
    return true;
}

bool mereader_tui_catalog_update_progress(MereaderTuiCatalog *catalog, const MereaderTuiHistory *history, MereaderTuiError *error) {
    if (catalog == NULL || catalog->search.handle == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "library catalog is not open");
        return false;
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        MereaderTuiCatalogBook *book = &catalog->books[book_index];
        for (size_t format_index = 0U; format_index < book->format_count; ++format_index) {
            book->formats[format_index].reading_progress = 0.0;
            free(book->formats[format_index].last_read);
            book->formats[format_index].last_read = NULL;
        }
    }
    return catalog_apply_history(catalog, history, error);
}

const MereaderTuiCatalogFormat *mereader_tui_catalog_preferred_format(const MereaderTuiCatalogBook *book) {
    if (book == NULL || book->format_count == 0U || book->preferred_format >= book->format_count) {
        return NULL;
    }
    return &book->formats[book->preferred_format];
}

bool mereader_tui_catalog_apply_format_preferences(MereaderTuiCatalog *catalog, const MereaderTuiFormatPreferences *preferences,
                                           MereaderTuiError *error) {
    if (catalog == NULL || preferences == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid catalog format preferences");
        return false;
    }
    CatalogMap books = {0};
    if (!catalog_map_init(&books, catalog->length, error)) {
        return false;
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        if (!catalog_map_insert(&books, catalog->books[book_index].group_key, book_index, error)) {
            catalog_map_free(&books);
            return false;
        }
    }
    for (size_t preference_index = 0U; preference_index < preferences->length; ++preference_index) {
        const MereaderTuiFormatPreference *preference = &preferences->items[preference_index];
        size_t book_index = 0U;
        if (!catalog_map_lookup(&books, preference->book_key, &book_index)) {
            continue;
        }
        MereaderTuiCatalogBook *book = &catalog->books[book_index];
        for (size_t format_index = 0U; format_index < book->format_count; ++format_index) {
            if (strcmp(book->formats[format_index].relative_path, preference->relative_path) == 0) {
                book->preferred_format = format_index;
                break;
            }
        }
    }
    catalog_map_free(&books);
    return true;
}

bool mereader_tui_catalog_prefer_path(MereaderTuiCatalog *catalog, const char *path, const char **book_key,
                              const char **relative_path) {
    if (catalog == NULL || path == NULL) {
        return false;
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        MereaderTuiCatalogBook *book = &catalog->books[book_index];
        for (size_t format_index = 0U; format_index < book->format_count; ++format_index) {
            if (strcmp(book->formats[format_index].path, path) != 0) {
                continue;
            }
            book->preferred_format = format_index;
            if (book_key != NULL) {
                *book_key = book->group_key;
            }
            if (relative_path != NULL) {
                *relative_path = book->formats[format_index].relative_path;
            }
            return true;
        }
    }
    return false;
}

void mereader_tui_catalog_matches_free(MereaderTuiCatalogMatches *matches) {
    if (matches == NULL) {
        return;
    }
    free(matches->book_indices);
    *matches = (MereaderTuiCatalogMatches){0};
}

static bool catalog_text_matches(const char *text, const char *token, size_t token_length) {
    if (text == NULL || token_length == 0U) {
        return false;
    }
    const size_t text_length = strlen(text);
    if (token_length > text_length) {
        return false;
    }
    for (size_t offset = 0U; offset <= text_length - token_length; ++offset) {
        size_t index = 0U;
        while (index < token_length &&
               tolower((unsigned char)text[offset + index]) == tolower((unsigned char)token[index])) {
            ++index;
        }
        if (index == token_length) {
            return true;
        }
    }
    size_t token_index = 0U;
    for (size_t text_index = 0U; text_index < text_length && token_index < token_length; ++text_index) {
        if (tolower((unsigned char)text[text_index]) == tolower((unsigned char)token[token_index])) {
            ++token_index;
        }
    }
    return token_index == token_length;
}

static bool catalog_book_matches(const MereaderTuiCatalogBook *book, const char *query) {
    const char *cursor = query;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const char *token = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const size_t token_length = (size_t)(cursor - token);
        if (token_length == 0U) {
            continue;
        }
        bool found = catalog_text_matches(book->title, token, token_length) ||
                     catalog_text_matches(book->author, token, token_length) ||
                     catalog_text_matches(book->directory, token, token_length);
        for (size_t index = 0U; !found && index < book->format_count; ++index) {
            found = catalog_text_matches(book->formats[index].relative_path, token, token_length) ||
                    catalog_text_matches(book->formats[index].name, token, token_length);
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool mereader_tui_catalog_search(MereaderTuiCatalog *catalog, const char *query, MereaderTuiCatalogMatches *matches, MereaderTuiError *error) {
    if (catalog == NULL || catalog->search.handle == NULL || query == NULL || matches == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid catalog search request");
        return false;
    }
    if (matches->book_indices != NULL || matches->length != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "catalog search output is not empty");
        return false;
    }
    size_t *indices = catalog->length == 0U ? NULL : mereader_tui_reallocarray(NULL, catalog->length, sizeof(*indices), error);
    if (catalog->length > 0U && indices == NULL) {
        return false;
    }
    if (query[0] == '\0') {
        for (size_t index = 0U; index < catalog->length; ++index) {
            indices[index] = index;
        }
        *matches = (MereaderTuiCatalogMatches){.book_indices = indices, .length = catalog->length};
        return true;
    }

    size_t format_count = 0U;
    for (size_t index = 0U; index < catalog->length; ++index) {
        if (format_count > SIZE_MAX - catalog->books[index].format_count) {
            free(indices);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "library format count overflow");
            return false;
        }
        format_count += catalog->books[index].format_count;
    }
    CatalogMap paths = {0};
    bool *seen = catalog->length == 0U ? NULL : calloc(catalog->length, sizeof(*seen));
    if (!catalog_map_init(&paths, format_count, error) || (catalog->length > 0U && seen == NULL)) {
        catalog_map_free(&paths);
        free(seen);
        free(indices);
        if (!mereader_tui_error_is_set(error)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate catalog search state");
        }
        return false;
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        for (size_t format_index = 0U; format_index < catalog->books[book_index].format_count; ++format_index) {
            if (!catalog_map_insert(&paths, catalog->books[book_index].formats[format_index].relative_path,
                                    book_index, error)) {
                catalog_map_free(&paths);
                free(seen);
                free(indices);
                return false;
            }
        }
    }

    size_t length = 0U;
    size_t offset = 0U;
    bool complete = false;
    while (!complete) {
        MereaderTuiSearchFiles files = {0};
        if (!mereader_tui_search_files(&catalog->search, query, offset, MEREADER_TUI_CATALOG_PAGE_SIZE, &files, error)) {
            catalog_map_free(&paths);
            free(seen);
            free(indices);
            return false;
        }
        for (size_t index = 0U; index < files.length; ++index) {
            size_t book_index = 0U;
            if (catalog_map_lookup(&paths, files.items[index].relative_path, &book_index) && !seen[book_index]) {
                seen[book_index] = true;
                indices[length++] = book_index;
            }
        }
        if (files.length == 0U || offset + files.length >= files.total) {
            complete = true;
        } else {
            offset += files.length;
        }
        mereader_tui_search_files_free(&files);
    }
    for (size_t book_index = 0U; book_index < catalog->length; ++book_index) {
        if (!seen[book_index] && catalog_book_matches(&catalog->books[book_index], query)) {
            seen[book_index] = true;
            indices[length++] = book_index;
        }
    }
    catalog_map_free(&paths);
    free(seen);
    *matches = (MereaderTuiCatalogMatches){.book_indices = indices, .length = length};
    return true;
}
