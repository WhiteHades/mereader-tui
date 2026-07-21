#include "baca/remote.h"

#include <curl/curl.h>
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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACA_REMOTE_MAX_URL_BYTES 8192U

typedef struct BacaRemoteWriter {
    int descriptor;
    size_t length;
    int saved_errno;
    bool exceeded;
} BacaRemoteWriter;

static pthread_once_t baca_remote_curl_once = PTHREAD_ONCE_INIT;
static CURLcode baca_remote_curl_status = CURLE_FAILED_INIT;

static const char *const BACA_REMOTE_EXTENSIONS[] = {
    ".epub", ".epub3", ".mobi", ".prc", ".azw",  ".azw3", ".azw4", ".pdf",
    ".cbz",  ".cbr",   ".cb7",  ".png", ".jpg",  ".jpeg", ".gif",  ".webp",
    ".bmp",  ".svg",   ".txt",  ".md",  ".fb2",  ".download",
};

bool baca_remote_is_url(const char *value) {
    return value != nullptr &&
           (g_ascii_strncasecmp(value, "http://", 7U) == 0 || g_ascii_strncasecmp(value, "https://", 8U) == 0);
}

static void baca_remote_curl_initialize(void) {
    baca_remote_curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

void baca_remote_file_free(BacaRemoteFile *file) {
    if (file == nullptr) {
        return;
    }
    free(file->url);
    free(file->path);
    *file = (BacaRemoteFile){0};
}

static bool baca_remote_output_empty(const BacaRemoteFile *file) {
    return file != nullptr && file->url == nullptr && file->path == nullptr;
}

static bool baca_remote_normalize_url(const char *input, char **normalized, char **scheme,
                                      char **path, BacaError *error) {
    if (input == nullptr || input[0] == '\0' || strlen(input) > BACA_REMOTE_MAX_URL_BYTES) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid remote URL");
        return false;
    }

    CURLU *url = curl_url();
    if (url == nullptr) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate URL parser");
        return false;
    }
    CURLUcode status = curl_url_set(url, CURLUPART_URL, input, CURLU_NON_SUPPORT_SCHEME);
    char *curl_scheme = nullptr;
    char *host = nullptr;
    char *user = nullptr;
    char *password = nullptr;
    if (status == CURLUE_OK) {
        status = curl_url_get(url, CURLUPART_SCHEME, &curl_scheme, 0U);
    }
    if (status == CURLUE_OK) {
        status = curl_url_get(url, CURLUPART_HOST, &host, 0U);
    }
    const bool has_user = curl_url_get(url, CURLUPART_USER, &user, 0U) == CURLUE_OK;
    const bool has_password = curl_url_get(url, CURLUPART_PASSWORD, &password, 0U) == CURLUE_OK;
    if (status != CURLUE_OK || curl_scheme == nullptr || host == nullptr || host[0] == '\0') {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid remote URL: %s",
                       status == CURLUE_OK ? "missing host" : curl_url_strerror(status));
        goto failure;
    }
    if (baca_casecmp(curl_scheme, "http") != 0 && baca_casecmp(curl_scheme, "https") != 0) {
        baca_error_set(error, BACA_ERROR_UNSUPPORTED, "only HTTP and HTTPS URLs are supported");
        goto failure;
    }
    if (has_user || has_password) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "URLs containing credentials are not supported");
        goto failure;
    }
    if (curl_url_set(url, CURLUPART_FRAGMENT, nullptr, 0U) != CURLUE_OK) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "could not remove URL fragment");
        goto failure;
    }

    char *curl_normalized = nullptr;
    char *curl_path = nullptr;
    status = curl_url_get(url, CURLUPART_URL, &curl_normalized, CURLU_NO_DEFAULT_PORT);
    if (status == CURLUE_OK) {
        status = curl_url_get(url, CURLUPART_PATH, &curl_path, 0U);
    }
    if (status != CURLUE_OK || curl_normalized == nullptr || curl_path == nullptr) {
        curl_free(curl_normalized);
        curl_free(curl_path);
        baca_error_set(error, BACA_ERROR_ARGUMENT, "could not normalize remote URL: %s", curl_url_strerror(status));
        goto failure;
    }

    *normalized = baca_strdup(curl_normalized, error);
    *scheme = baca_strdup(curl_scheme, error);
    *path = baca_strdup(curl_path, error);
    curl_free(curl_normalized);
    curl_free(curl_path);
    curl_free(curl_scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(url);
    if (*normalized == nullptr || *scheme == nullptr || *path == nullptr) {
        free(*normalized);
        free(*scheme);
        free(*path);
        *normalized = nullptr;
        *scheme = nullptr;
        *path = nullptr;
        return false;
    }
    return true;

failure:
    curl_free(curl_scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(url);
    return false;
}

static const char *baca_remote_supported_extension(const char *path) {
    const char *extension = baca_path_extension(path);
    for (size_t index = 0U; index + 1U < BACA_ARRAY_LEN(BACA_REMOTE_EXTENSIONS); ++index) {
        if (baca_casecmp(extension, BACA_REMOTE_EXTENSIONS[index]) == 0) {
            return BACA_REMOTE_EXTENSIONS[index];
        }
    }
    return nullptr;
}

static char *baca_remote_cache_path(const char *directory, const char *hash, const char *extension,
                                    BacaError *error) {
    char filename[96] = {0};
    const int length = snprintf(filename, sizeof(filename), "%s%s", hash, extension);
    if (length <= 0 || (size_t)length >= sizeof(filename)) {
        baca_error_set(error, BACA_ERROR_INTERNAL, "could not build remote cache filename");
        return nullptr;
    }
    return baca_path_join(directory, filename, error);
}

static bool baca_remote_cached_file(const char *path, char **resolved, bool *found, BacaError *error) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            *found = false;
            return true;
        }
        const int saved_errno = errno;
        baca_error_set(error, BACA_ERROR_IO, "could not inspect remote cache '%s': %s", path,
                       strerror(saved_errno));
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 || (uintmax_t)status.st_size > BACA_REMOTE_MAX_BYTES) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "remote cache entry is invalid: %s", path);
        return false;
    }
    if (chmod(path, 0600) != 0) {
        const int saved_errno = errno;
        baca_error_set(error, BACA_ERROR_IO, "could not secure remote cache '%s': %s", path,
                       strerror(saved_errno));
        return false;
    }
    *resolved = baca_realpath(path, error);
    *found = *resolved != nullptr;
    return *found;
}

static bool baca_remote_find_cached(const char *directory, const char *hash, const char *preferred,
                                    char **path, bool *found, BacaError *error) {
    *found = false;
    if (preferred != nullptr) {
        char *candidate = baca_remote_cache_path(directory, hash, preferred, error);
        if (candidate == nullptr) {
            return false;
        }
        bool candidate_found = false;
        const bool checked = baca_remote_cached_file(candidate, path, &candidate_found, error);
        free(candidate);
        if (!checked || candidate_found) {
            *found = candidate_found;
            return checked;
        }
    }
    for (size_t index = 0U; index < BACA_ARRAY_LEN(BACA_REMOTE_EXTENSIONS); ++index) {
        const char *extension = BACA_REMOTE_EXTENSIONS[index];
        if (preferred != nullptr && strcmp(extension, preferred) == 0) {
            continue;
        }
        char *candidate = baca_remote_cache_path(directory, hash, extension, error);
        if (candidate == nullptr) {
            return false;
        }
        bool candidate_found = false;
        const bool checked = baca_remote_cached_file(candidate, path, &candidate_found, error);
        free(candidate);
        if (!checked || candidate_found) {
            *found = candidate_found;
            return checked;
        }
    }
    return true;
}

static size_t baca_remote_write(char *data, size_t size, size_t count, void *user_data) {
    BacaRemoteWriter *writer = user_data;
    if (size != 0U && count > SIZE_MAX / size) {
        writer->exceeded = true;
        return CURL_WRITEFUNC_ERROR;
    }
    const size_t length = size * count;
    if (length > BACA_REMOTE_MAX_BYTES - writer->length) {
        writer->exceeded = true;
        return CURL_WRITEFUNC_ERROR;
    }
    size_t offset = 0U;
    while (offset < length) {
        const ssize_t written = write(writer->descriptor, data + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            writer->saved_errno = written == 0 ? EIO : errno;
            return CURL_WRITEFUNC_ERROR;
        }
    }
    writer->length += length;
    return length;
}

static const char *baca_remote_content_extension(const char *content_type) {
    if (content_type == nullptr) {
        return nullptr;
    }
    static const struct {
        const char *content_type;
        const char *extension;
    } types[] = {
        {"application/epub+zip", ".epub"},
        {"application/pdf", ".pdf"},
        {"application/x-mobipocket-ebook", ".mobi"},
        {"application/vnd.comicbook+zip", ".cbz"},
        {"application/vnd.comicbook-rar", ".cbr"},
        {"image/png", ".png"},
        {"image/jpeg", ".jpg"},
        {"image/gif", ".gif"},
        {"image/webp", ".webp"},
        {"image/bmp", ".bmp"},
        {"image/svg+xml", ".svg"},
        {"text/markdown", ".md"},
        {"text/plain", ".txt"},
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(types); ++index) {
        const size_t length = strlen(types[index].content_type);
        if (g_ascii_strncasecmp(content_type, types[index].content_type, length) == 0 &&
            (content_type[length] == '\0' || content_type[length] == ';' || content_type[length] == ' ')) {
            return types[index].extension;
        }
    }
    return nullptr;
}

static const char *baca_remote_effective_extension(const char *effective_url) {
    if (effective_url == nullptr) {
        return nullptr;
    }
    CURLU *url = curl_url();
    if (url == nullptr || curl_url_set(url, CURLUPART_URL, effective_url, 0U) != CURLUE_OK) {
        curl_url_cleanup(url);
        return nullptr;
    }
    char *path = nullptr;
    const char *extension = nullptr;
    if (curl_url_get(url, CURLUPART_PATH, &path, 0U) == CURLUE_OK) {
        extension = baca_remote_supported_extension(path);
    }
    static _Thread_local char result[16];
    if (extension != nullptr) {
        (void)snprintf(result, sizeof(result), "%s", extension);
    }
    curl_free(path);
    curl_url_cleanup(url);
    return extension == nullptr ? nullptr : result;
}

static bool baca_remote_download(const char *url, const char *scheme, const char *directory, const char *hash,
                                 const char *preferred_extension, char **path, BacaError *error) {
    char *template = baca_path_join(directory, ".download-XXXXXX", error);
    if (template == nullptr) {
        return false;
    }
    int descriptor = mkstemp(template);
    if (descriptor < 0) {
        const int saved_errno = errno;
        baca_error_set(error, BACA_ERROR_IO, "could not create remote cache file: %s", strerror(saved_errno));
        free(template);
        return false;
    }
    (void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    if (fchmod(descriptor, 0600) != 0) {
        const int saved_errno = errno;
        (void)close(descriptor);
        (void)unlink(template);
        baca_error_set(error, BACA_ERROR_IO, "could not secure remote cache file: %s", strerror(saved_errno));
        free(template);
        return false;
    }

    (void)pthread_once(&baca_remote_curl_once, baca_remote_curl_initialize);
    if (baca_remote_curl_status != CURLE_OK) {
        (void)close(descriptor);
        (void)unlink(template);
        baca_error_set(error, BACA_ERROR_EXTERNAL, "could not initialize libcurl: %s",
                       curl_easy_strerror(baca_remote_curl_status));
        free(template);
        return false;
    }
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        (void)close(descriptor);
        (void)unlink(template);
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate HTTP client");
        free(template);
        return false;
    }

    BacaRemoteWriter writer = {.descriptor = descriptor};
    char curl_error[CURL_ERROR_SIZE] = {0};
    CURLcode status = CURLE_OK;
    const char *redirect_protocols = baca_casecmp(scheme, "https") == 0 ? "https" : "http,https";
    if ((status = curl_easy_setopt(curl, CURLOPT_URL, url)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https")) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, redirect_protocols)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                                   (curl_off_t)BACA_REMOTE_MAX_BYTES)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "")) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_USERAGENT, BACA_NAME "/" BACA_VERSION)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, baca_remote_write)) != CURLE_OK ||
        (status = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer)) != CURLE_OK) {
        baca_error_set(error, BACA_ERROR_EXTERNAL, "could not configure HTTP request: %s",
                       curl_easy_strerror(status));
        goto failure;
    }

    status = curl_easy_perform(curl);
    if (writer.exceeded || status == CURLE_FILESIZE_EXCEEDED) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "remote document exceeds the 256 MiB download limit");
        goto failure;
    }
    if (writer.saved_errno != 0) {
        baca_error_set(error, BACA_ERROR_IO, "could not write remote cache file: %s", strerror(writer.saved_errno));
        goto failure;
    }
    if (status != CURLE_OK) {
        baca_error_set(error, BACA_ERROR_EXTERNAL, "could not download '%s': %s", url,
                       curl_error[0] != '\0' ? curl_error : curl_easy_strerror(status));
        goto failure;
    }

    char *content_type = nullptr;
    char *effective_url = nullptr;
    (void)curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    (void)curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    const char *extension = preferred_extension;
    if (extension == nullptr) {
        extension = baca_remote_effective_extension(effective_url);
    }
    if (extension == nullptr) {
        extension = baca_remote_content_extension(content_type);
    }
    if (extension == nullptr) {
        extension = ".download";
    }
    char *final_path = baca_remote_cache_path(directory, hash, extension, error);
    if (final_path == nullptr) {
        goto failure;
    }
    if (fsync(descriptor) != 0) {
        const int saved_errno = errno;
        free(final_path);
        baca_error_set(error, BACA_ERROR_IO, "could not finish remote cache file: %s", strerror(saved_errno));
        goto failure;
    }
    if (close(descriptor) != 0) {
        const int saved_errno = errno;
        descriptor = -1;
        free(final_path);
        baca_error_set(error, BACA_ERROR_IO, "could not close remote cache file: %s", strerror(saved_errno));
        goto failure;
    }
    descriptor = -1;
    if (rename(template, final_path) != 0) {
        const int saved_errno = errno;
        free(final_path);
        baca_error_set(error, BACA_ERROR_IO, "could not publish remote cache file: %s", strerror(saved_errno));
        goto failure;
    }
    free(template);
    curl_easy_cleanup(curl);
    *path = final_path;
    return true;

failure:
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    (void)unlink(template);
    free(template);
    curl_easy_cleanup(curl);
    return false;
}

bool baca_remote_fetch(const char *url, BacaRemoteFile *file, BacaError *error) {
    baca_error_clear(error);
    if (!baca_remote_output_empty(file)) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "remote file output must be zero-initialized");
        return false;
    }

    BacaRemoteFile result = {0};
    char *scheme = nullptr;
    char *url_path = nullptr;
    if (!baca_remote_normalize_url(url, &result.url, &scheme, &url_path, error)) {
        return false;
    }
    const char *preferred_extension = baca_remote_supported_extension(url_path);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, result.url, -1);
    if (hash == nullptr) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not hash remote URL");
        goto failure;
    }
    char *directory = baca_xdg_cache_path("downloads", error);
    if (directory == nullptr || !baca_mkdirs(directory, error)) {
        free(directory);
        g_free(hash);
        goto failure;
    }
    if (chmod(directory, 0700) != 0) {
        const int saved_errno = errno;
        baca_error_set(error, BACA_ERROR_IO, "could not secure remote cache directory: %s", strerror(saved_errno));
        free(directory);
        g_free(hash);
        goto failure;
    }

    bool found = false;
    bool success = baca_remote_find_cached(directory, hash, preferred_extension, &result.path, &found, error);
    if (success && !found) {
        success = baca_remote_download(result.url, scheme, directory, hash, preferred_extension, &result.path, error);
    }
    free(directory);
    g_free(hash);
    if (!success) {
        goto failure;
    }

    free(scheme);
    free(url_path);
    *file = result;
    return true;

failure:
    free(scheme);
    free(url_path);
    baca_remote_file_free(&result);
    return false;
}
