#include "mereader-tui/document_backend.h"
#include "mereader-tui/graphics.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEREADER_TUI_IMAGE_MAX_EXPORT_BYTES MEREADER_TUI_DOCUMENT_MAX_RETAINED_BYTES
#define MEREADER_TUI_IMAGE_EXPORT_CHUNK_BYTES (64U * 1024U)

typedef struct MereaderTuiImageBackend {
    struct stat identity;
    unsigned char *snapshot;
    size_t snapshot_length;
    int descriptor;
} MereaderTuiImageBackend;

static MereaderTuiErrorCode image_errno_code(int value) {
    return value == ENOENT || value == ENOTDIR ? MEREADER_TUI_ERROR_NOT_FOUND : MEREADER_TUI_ERROR_IO;
}

static void image_backend_destroy(MereaderTuiImageBackend *backend) {
    if (backend == NULL) {
        return;
    }
    if (backend->descriptor >= 0) {
        (void)close(backend->descriptor);
    }
    free(backend->snapshot);
    free(backend);
}

static bool image_stat_unchanged(const struct stat *before, const struct stat *after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
           before->st_mode == after->st_mode && before->st_size == after->st_size &&
           before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static bool image_object_unchanged(const struct stat *expected, const struct stat *actual) {
    return expected->st_dev == actual->st_dev && expected->st_ino == actual->st_ino &&
           expected->st_mode == actual->st_mode && expected->st_size == actual->st_size;
}

static MereaderTuiImageBackend *image_backend_open(const char *path, const struct stat *expected_identity,
                                            MereaderTuiError *error) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, image_errno_code(saved_errno), "could not open image '%s': %s", path,
                       strerror(saved_errno));
        return NULL;
    }

    struct stat status;
    if (fstat(descriptor, &status) != 0) {
        const int saved_errno = errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect image '%s': %s", path,
                       strerror(saved_errno));
        return NULL;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "image is not a regular file: %s", path);
        return NULL;
    }
    if (!image_stat_unchanged(expected_identity, &status)) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                       "image changed between format detection and open: %s", path);
        return NULL;
    }

    MereaderTuiImageBackend *backend = calloc(1U, sizeof(*backend));
    if (backend == NULL) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate image backend");
        return NULL;
    }
    backend->descriptor = descriptor;
    backend->identity = status;

    if ((uintmax_t)status.st_size > MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES) {
        return backend;
    }

    const size_t size = (size_t)status.st_size;
    backend->snapshot = malloc(size == 0U ? 1U : size);
    if (backend->snapshot == NULL) {
        image_backend_destroy(backend);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate image snapshot: %s", path);
        return NULL;
    }
    size_t offset = 0U;
    while (offset < size) {
        const ssize_t count = pread(descriptor, backend->snapshot + offset, size - offset, (off_t)offset);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count == 0) {
            image_backend_destroy(backend);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "image was truncated while opening: %s", path);
            return NULL;
        }
        const int saved_errno = errno;
        image_backend_destroy(backend);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read image '%s': %s", path,
                       strerror(saved_errno));
        return NULL;
    }
    struct stat after;
    if (fstat(descriptor, &after) != 0) {
        const int saved_errno = errno;
        image_backend_destroy(backend);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not re-inspect image '%s': %s", path,
                       strerror(saved_errno));
        return NULL;
    }
    if (!image_stat_unchanged(&status, &after)) {
        image_backend_destroy(backend);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "image changed while opening: %s", path);
        return NULL;
    }
    backend->snapshot_length = size;
    return backend;
}

static bool image_load_resource(MereaderTuiDocument *document, const char *uri, MereaderTuiResource *resource,
                                MereaderTuiError *error) {
    MereaderTuiImageBackend *backend = document->backend;
    if (document->path == NULL || backend == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "image document is closed");
        return false;
    }
    if (strcmp(uri, document->path) != 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_NOT_FOUND, "image resource not found: %s", uri);
        return false;
    }

    if ((uintmax_t)backend->identity.st_size > MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "image exceeds the supported size limit: %s",
                       document->path);
        return false;
    }
    if (backend->snapshot == NULL || backend->snapshot_length != (size_t)backend->identity.st_size) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "image snapshot is unavailable: %s", document->path);
        return false;
    }

    const size_t size = backend->snapshot_length;
    unsigned char *data = malloc(size == 0U ? 1U : size);
    if (data == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate image resource: %s", document->path);
        return false;
    }
    if (size > 0U) {
        memcpy(data, backend->snapshot, size);
    }
    resource->data = data;
    resource->length = size;
    return true;
}

static bool image_resource_size(const MereaderTuiDocument *document, const char *uri, size_t *size) {
    const MereaderTuiImageBackend *backend = document->backend;
    if (backend == NULL || document->path == NULL || uri == NULL || size == NULL ||
        strcmp(uri, document->path) != 0 || backend->identity.st_size < 0 ||
        (uintmax_t)backend->identity.st_size > SIZE_MAX) {
        return false;
    }
    *size = (size_t)backend->identity.st_size;
    return true;
}

static void image_close(MereaderTuiDocument *document) {
    image_backend_destroy(document->backend);
    document->backend = NULL;
}

static const MereaderTuiDocumentOps image_document_ops = {
    .load_resource = image_load_resource,
    .resource_size = image_resource_size,
    .close = image_close,
};

static bool image_write_all(int descriptor, const void *data, size_t length, const char *destination,
                            MereaderTuiError *error) {
    const unsigned char *cursor = data;
    while (length > 0U) {
        const ssize_t written = write(descriptor, cursor, length);
        if (written > 0) {
            cursor += (size_t)written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        const int saved_errno = written == 0 ? EIO : errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not write image export '%s': %s", destination,
                       strerror(saved_errno));
        return false;
    }
    return true;
}

bool mereader_tui_image_export_original(const MereaderTuiDocument *document, const char *destination,
                                MereaderTuiError *error) {
    if (document == NULL || document->format != MEREADER_TUI_FORMAT_IMAGE ||
        document->ops != &image_document_ops || document->backend == NULL || destination == NULL ||
        destination[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "standalone image document is required");
        return false;
    }
    const MereaderTuiImageBackend *backend = document->backend;
    if (backend->identity.st_size < 0 ||
        (uintmax_t)backend->identity.st_size > MEREADER_TUI_IMAGE_MAX_EXPORT_BYTES) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                       "image exceeds the 256 MiB export size limit: %s", document->path);
        return false;
    }

    struct stat before = {0};
    const bool snapshot_export = (uintmax_t)backend->identity.st_size <= MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES;
    if (snapshot_export) {
        if (backend->snapshot == NULL || backend->snapshot_length != (size_t)backend->identity.st_size) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL, "image snapshot is unavailable: %s", document->path);
            return false;
        }
    } else if (backend->descriptor < 0 || fstat(backend->descriptor, &before) != 0 ||
               !image_object_unchanged(&backend->identity, &before)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                       "image changed before exporting the held original: %s", document->path);
        return false;
    }

    int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not create image export '%s': %s", destination,
                       strerror(saved_errno));
        return false;
    }

    bool success = true;
    struct stat output_status;
    if (fstat(output, &output_status) != 0) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect image export '%s': %s", destination,
                       strerror(saved_errno));
        success = false;
    } else if (!S_ISREG(output_status.st_mode)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "image export is not a regular file: %s", destination);
        success = false;
    } else if (snapshot_export) {
        success = image_write_all(output, backend->snapshot, backend->snapshot_length, destination, error);
    } else {
        unsigned char buffer[MEREADER_TUI_IMAGE_EXPORT_CHUNK_BYTES];
        size_t offset = 0U;
        const size_t size = (size_t)backend->identity.st_size;
        while (success && offset < size) {
            size_t wanted = size - offset;
            if (wanted > sizeof(buffer)) {
                wanted = sizeof(buffer);
            }
            const ssize_t count = pread(backend->descriptor, buffer, wanted, (off_t)offset);
            if (count > 0) {
                success = image_write_all(output, buffer, (size_t)count, destination, error);
                offset += (size_t)count;
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else if (count == 0) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                               "image was truncated while exporting: %s", document->path);
                success = false;
            } else {
                const int saved_errno = errno;
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read held image '%s': %s",
                               document->path, strerror(saved_errno));
                success = false;
            }
        }
        struct stat after;
        if (success && (fstat(backend->descriptor, &after) != 0 ||
                        !image_stat_unchanged(&before, &after))) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                           "image changed while exporting the held original: %s", document->path);
            success = false;
        }
    }

    if (success && (fstat(output, &output_status) != 0 || !S_ISREG(output_status.st_mode) ||
                    output_status.st_size != backend->identity.st_size)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "image export has an unexpected size: %s", destination);
        success = false;
    }
    if (close(output) != 0 && success) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close image export '%s': %s", destination,
                       strerror(saved_errno));
        success = false;
    }
    if (!success && unlink(destination) != 0 && errno != ENOENT) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not remove incomplete image export '%s': %s",
                       destination, strerror(saved_errno));
    }
    return success;
}

bool mereader_tui_image_open(MereaderTuiDocument *document, const char *path, const struct stat *expected_identity,
                     MereaderTuiError *error) {
    if (document == NULL || path == NULL || path[0] == '\0' || document->path == NULL ||
        strcmp(path, document->path) != 0 || expected_identity == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                       "document, canonical image path, and detected identity are required");
        return false;
    }

    MereaderTuiImageBackend *backend = image_backend_open(path, expected_identity, error);
    if (backend == NULL) {
        return false;
    }
    document->format = MEREADER_TUI_FORMAT_IMAGE;
    document->backend = backend;
    document->ops = &image_document_ops;

    document->metadata.title = mereader_tui_path_basename(document->path, error);
    document->metadata.format = mereader_tui_strdup("Image", error);
    if (document->metadata.title == NULL || document->metadata.format == NULL ||
        !mereader_tui_document_account_metadata(document, error)) {
        return false;
    }

    MereaderTuiBlock block = {
        .kind = MEREADER_TUI_BLOCK_IMAGE,
        .section_id = mereader_tui_strdup(document->path, error),
        .value.image = {
            .uri = mereader_tui_strdup(document->path, error),
            .alt = mereader_tui_strdup(document->metadata.title, error),
            .page_index = -1,
        },
    };
    if (block.section_id == NULL || block.value.image.uri == NULL || block.value.image.alt == NULL ||
        !mereader_tui_document_add_image_block(document, &block, error)) {
        mereader_tui_document_block_free(&block);
        return false;
    }

    MereaderTuiSection section = {
        .id = mereader_tui_strdup(document->path, error),
        .first_block = 0U,
        .block_count = 1U,
        .linear = true,
    };
    if (section.id == NULL || !mereader_tui_document_add_section(document, &section, error)) {
        free(section.id);
        return false;
    }

    return true;
}
