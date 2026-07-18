#pragma once

#include "baca/common.h"

[[nodiscard]] bool baca_platform_open(const char *target, const char *preferred, BacaError *error);
[[nodiscard]] bool baca_platform_save_svg(const char *path, const char *const *lines, size_t line_count,
                                          uint32_t background, uint32_t foreground, BacaError *error);
[[nodiscard]] const char *baca_platform_find_executable(const char *name);
