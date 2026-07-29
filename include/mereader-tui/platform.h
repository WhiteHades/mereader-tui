#pragma once

#include "mereader-tui/common.h"

typedef enum MereaderTuiSvgAttributes : uint8_t {
    MEREADER_TUI_SVG_NORMAL = 0U,
    MEREADER_TUI_SVG_BOLD = 1U << 0U,
    MEREADER_TUI_SVG_DIM = 1U << 1U,
    MEREADER_TUI_SVG_UNDERLINE = 1U << 2U,
    MEREADER_TUI_SVG_ITALIC = 1U << 3U,
} MereaderTuiSvgAttributes;

typedef struct MereaderTuiSvgCell {
    char *text;
    size_t column;
    size_t columns;
    uint32_t background;
    uint32_t foreground;
    MereaderTuiSvgAttributes attributes;
} MereaderTuiSvgCell;

typedef struct MereaderTuiSvgLine {
    MereaderTuiSvgCell *cells;
    size_t cell_count;
} MereaderTuiSvgLine;

[[nodiscard]] bool mereader_tui_platform_open(const char *target, const char *preferred, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_platform_save_svg(const char *path, const MereaderTuiSvgLine *lines,
                                          size_t line_count, size_t column_count, uint32_t background,
                                          uint32_t foreground, MereaderTuiError *error);
[[nodiscard]] const char *mereader_tui_platform_find_executable(const char *name);
