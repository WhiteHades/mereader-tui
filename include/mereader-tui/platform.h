#pragma once

#include "mereader-tui/common.h"

[[nodiscard]] bool mereader_tui_platform_open(const char *target, const char *preferred, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_platform_save_svg(const char *path, const char *const *lines, size_t line_count,
                                          uint32_t background, uint32_t foreground, MereaderTuiError *error);
[[nodiscard]] const char *mereader_tui_platform_find_executable(const char *name);
