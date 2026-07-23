#pragma once

#include "mereader-tui/config.h"
#include "mereader-tui/document.h"

#include <stdatomic.h>

/* Layout clamps hostile geometry and limits aggregate aspect-ratio expansion. */
#define MEREADER_TUI_LAYOUT_MAX_WIDTH 1024
#define MEREADER_TUI_LAYOUT_MAX_LINES 262144U
#define MEREADER_TUI_LAYOUT_MAX_IMAGE_ROWS 1024
#define MEREADER_TUI_LAYOUT_MAX_IMAGE_EXTRA_ROWS 65536U

typedef enum MereaderTuiLayoutLineKind : uint8_t {
    MEREADER_TUI_LAYOUT_TEXT = 0,
    MEREADER_TUI_LAYOUT_IMAGE,
    MEREADER_TUI_LAYOUT_BLANK,
} MereaderTuiLayoutLineKind;

typedef struct MereaderTuiLayoutLine {
    MereaderTuiLayoutLineKind kind;
    size_t block_index;
    size_t byte_start;
    size_t byte_end;
    size_t logical_offset;
    int indent;
    int image_row;
    int image_rows;
    bool image_placeholder;
    bool paragraph_end;
} MereaderTuiLayoutLine;

typedef struct MereaderTuiLayout {
    const MereaderTuiDocument *document;
    MereaderTuiLayoutLine *lines;
    size_t line_count;
    size_t line_capacity;
    size_t *block_first_line;
    size_t *section_first_line;
    size_t *block_section;
    size_t logical_length;
    size_t image_extra_rows;
    int width;
    int cell_pixel_width;
    int cell_pixel_height;
    MereaderTuiJustification justification;
    MereaderTuiPresentation presentation;
    const atomic_bool *cancel;
} MereaderTuiLayout;

typedef struct MereaderTuiSearchMatch {
    size_t line;
    size_t byte_start;
    size_t byte_end;
    size_t block_index;
    size_t block_offset;
} MereaderTuiSearchMatch;

void mereader_tui_layout_free(MereaderTuiLayout *layout);
[[nodiscard]] bool mereader_tui_layout_build(MereaderTuiLayout *layout, const MereaderTuiDocument *document, int width,
                                     MereaderTuiJustification justification, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_layout_build_presentation(MereaderTuiLayout *layout, const MereaderTuiDocument *document, int width,
                                                   MereaderTuiJustification justification, MereaderTuiPresentation presentation,
                                                   int cell_pixel_width, int cell_pixel_height,
                                                   const atomic_bool *cancel, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_layout_line_text(const MereaderTuiLayout *layout, size_t line_index, bool justify,
                                           MereaderTuiError *error);
[[nodiscard]] size_t mereader_tui_layout_target_line(const MereaderTuiLayout *layout, const char *target);
[[nodiscard]] size_t mereader_tui_layout_section_for_line(const MereaderTuiLayout *layout, size_t line,
                                                  size_t *comparison_count);
[[nodiscard]] double mereader_tui_layout_progress(size_t scroll_line, size_t maximum_scroll);
[[nodiscard]] size_t mereader_tui_layout_restore_progress(double progress, size_t maximum_scroll);
[[nodiscard]] bool mereader_tui_layout_search(const MereaderTuiLayout *layout, const char *pattern, MereaderTuiSearchMatch **matches,
                                      size_t *match_count, MereaderTuiError *error);
