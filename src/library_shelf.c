#include "mereader-tui/library_shelf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mereader_tui_library_shelf_free(MereaderTuiLibraryShelf *shelf) {
    if (shelf == NULL) {
        return;
    }
    mereader_tui_history_free(&shelf->entries);
    free(shelf->references);
    *shelf = (MereaderTuiLibraryShelf){0};
}

static bool shelf_append(MereaderTuiLibraryShelf *shelf, const char *path, const char *title, const char *author,
                         double progress, const char *last_read, MereaderTuiLibraryShelfReference reference,
                         MereaderTuiError *error) {
    const size_t needed = shelf->entries.length + 1U;
    MereaderTuiHistoryEntry *entries = mereader_tui_array_reserve(shelf->entries.items, &shelf->entries.capacity,
                                                   sizeof(*shelf->entries.items), needed, error);
    if (entries == NULL) {
        return false;
    }
    shelf->entries.items = entries;
    MereaderTuiLibraryShelfReference *references = mereader_tui_array_reserve(
        shelf->references, &shelf->reference_capacity, sizeof(*shelf->references), needed, error);
    if (references == NULL) {
        return false;
    }
    shelf->references = references;

    MereaderTuiHistoryEntry entry = {
        .filepath = mereader_tui_strdup(path == NULL ? "" : path, error),
        .title = mereader_tui_strdup(title == NULL ? "" : title, error),
        .author = mereader_tui_strdup(author == NULL ? "" : author, error),
        .reading_progress = progress,
        .last_read = mereader_tui_strdup(last_read == NULL ? "" : last_read, error),
    };
    if (entry.filepath == NULL || entry.title == NULL || entry.author == NULL || entry.last_read == NULL) {
        free(entry.filepath);
        free(entry.title);
        free(entry.author);
        free(entry.last_read);
        return false;
    }
    shelf->entries.items[shelf->entries.length] = entry;
    shelf->references[shelf->entries.length] = reference;
    ++shelf->entries.length;
    return true;
}

static char *shelf_book_title(const MereaderTuiCatalogBook *book, MereaderTuiError *error) {
    MereaderTuiString title = {0};
    const MereaderTuiCatalogFormat *preferred = mereader_tui_catalog_preferred_format(book);
    if (!mereader_tui_string_append(&title, book->title, error)) {
        return NULL;
    }
    if (preferred != NULL && book->format_count > 1U) {
        char suffix[64] = {0};
        (void)snprintf(suffix, sizeof(suffix), " [%s +%zu]", preferred->name, book->format_count - 1U);
        if (!mereader_tui_string_append(&title, suffix, error)) {
            mereader_tui_string_free(&title);
            return NULL;
        }
    }
    return mereader_tui_string_take(&title);
}

static bool shelf_add_book(MereaderTuiLibraryShelf *shelf, const MereaderTuiCatalog *catalog, size_t book_index,
                           MereaderTuiError *error) {
    const MereaderTuiCatalogBook *book = &catalog->books[book_index];
    const MereaderTuiCatalogFormat *preferred = mereader_tui_catalog_preferred_format(book);
    if (preferred == NULL) {
        return true;
    }
    char *title = shelf_book_title(book, error);
    if (title == NULL) {
        return false;
    }
    const bool added = shelf_append(
        shelf, preferred->path, title, book->author, preferred->reading_progress, preferred->last_read,
        (MereaderTuiLibraryShelfReference){.book_index = book_index, .format_index = SIZE_MAX}, error);
    free(title);
    return added;
}

static bool shelf_add_formats(MereaderTuiLibraryShelf *shelf, const MereaderTuiCatalog *catalog, size_t book_index,
                              MereaderTuiError *error) {
    if (book_index >= catalog->length) {
        return true;
    }
    const MereaderTuiCatalogBook *book = &catalog->books[book_index];
    for (size_t index = 0U; index < book->format_count; ++index) {
        const MereaderTuiCatalogFormat *format = &book->formats[index];
        char title[512] = {0};
        (void)snprintf(title, sizeof(title), "%s [%s]", book->title, format->name);
        if (!shelf_append(shelf, format->path, title, book->author, format->reading_progress, format->last_read,
                          (MereaderTuiLibraryShelfReference){.book_index = book_index, .format_index = index}, error)) {
            return false;
        }
    }
    return true;
}

static bool shelf_add_authors(MereaderTuiLibraryShelf *shelf, const MereaderTuiCatalog *catalog,
                              const MereaderTuiCatalogMatches *matches, MereaderTuiError *error) {
    bool *matched = catalog->length == 0U ? NULL : calloc(catalog->length, sizeof(*matched));
    if (catalog->length > 0U && matched == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate author filter");
        return false;
    }
    for (size_t index = 0U; index < matches->length; ++index) {
        matched[matches->book_indices[index]] = true;
    }
    bool added = true;
    for (size_t start = 0U; added && start < catalog->length;) {
        size_t end = start + 1U;
        while (end < catalog->length && strcmp(catalog->books[start].author, catalog->books[end].author) == 0) {
            ++end;
        }
        size_t selected_book = SIZE_MAX;
        double progress = 0.0;
        const char *last_read = NULL;
        for (size_t index = start; index < end; ++index) {
            if (!matched[index]) {
                continue;
            }
            if (selected_book == SIZE_MAX) {
                selected_book = index;
            }
            const MereaderTuiCatalogFormat *format = mereader_tui_catalog_preferred_format(&catalog->books[index]);
            if (format != NULL && format->reading_progress > progress) {
                progress = format->reading_progress;
            }
            if (format != NULL && format->last_read != NULL &&
                (last_read == NULL || strcmp(format->last_read, last_read) > 0)) {
                last_read = format->last_read;
            }
        }
        if (selected_book != SIZE_MAX) {
            const MereaderTuiCatalogBook *book = &catalog->books[selected_book];
            char count[64] = {0};
            (void)snprintf(count, sizeof(count), "%zu %s", end - start, end - start == 1U ? "book" : "books");
            const MereaderTuiCatalogFormat *format = mereader_tui_catalog_preferred_format(book);
            added = format != NULL &&
                    shelf_append(shelf, format->path, book->author[0] == '\0' ? "unknown author" : book->author,
                                 count, progress, last_read,
                                 (MereaderTuiLibraryShelfReference){.book_index = selected_book, .format_index = SIZE_MAX},
                                 error);
        }
        start = end;
    }
    free(matched);
    return added;
}

bool mereader_tui_library_shelf_build(MereaderTuiLibraryShelf *shelf, MereaderTuiCatalog *catalog, MereaderTuiLibraryShelfKind kind,
                              const char *author, size_t format_book, const char *query, MereaderTuiError *error) {
    if (shelf == NULL || catalog == NULL || query == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "invalid library shelf request");
        return false;
    }
    if (shelf->entries.items != NULL || shelf->entries.length != 0U || shelf->references != NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "library shelf output is not empty");
        return false;
    }
    MereaderTuiCatalogMatches matches = {0};
    if (!mereader_tui_catalog_search(catalog, query, &matches, error)) {
        return false;
    }

    bool built = true;
    if (kind == MEREADER_TUI_LIBRARY_SHELF_FORMATS) {
        bool matched = query[0] == '\0';
        for (size_t index = 0U; !matched && index < matches.length; ++index) {
            matched = matches.book_indices[index] == format_book;
        }
        built = !matched || shelf_add_formats(shelf, catalog, format_book, error);
    } else if (kind == MEREADER_TUI_LIBRARY_SHELF_AUTHORS) {
        built = shelf_add_authors(shelf, catalog, &matches, error);
    } else {
        for (size_t index = 0U; built && index < matches.length; ++index) {
            const size_t book_index = matches.book_indices[index];
            const MereaderTuiCatalogBook *book = &catalog->books[book_index];
            if (kind == MEREADER_TUI_LIBRARY_SHELF_BOOKS && author != NULL && strcmp(book->author, author) != 0) {
                continue;
            }
            built = shelf_add_book(shelf, catalog, book_index, error);
        }
    }
    mereader_tui_catalog_matches_free(&matches);
    if (!built) {
        mereader_tui_library_shelf_free(shelf);
    }
    return built;
}

const MereaderTuiLibraryShelfReference *mereader_tui_library_shelf_reference(const MereaderTuiLibraryShelf *shelf,
                                                               const MereaderTuiHistoryEntry *entry) {
    if (shelf == NULL || entry == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < shelf->entries.length; ++index) {
        if (entry == &shelf->entries.items[index]) {
            return &shelf->references[index];
        }
    }
    return NULL;
}
