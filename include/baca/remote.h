#pragma once

#include "baca/common.h"

#define BACA_REMOTE_MAX_BYTES (256U * 1024U * 1024U)

typedef struct BacaRemoteFile {
    char *url;
    char *path;
} BacaRemoteFile;

[[nodiscard]] bool baca_remote_is_url(const char *value);
[[nodiscard]] bool baca_remote_fetch(const char *url, BacaRemoteFile *file, BacaError *error);
void baca_remote_file_free(BacaRemoteFile *file);
