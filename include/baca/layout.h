#pragma once

#include "baca/config.h"
#include "baca/document.h"

#include <stdatomic.h>

/* Layout clamps hostile geometry and limits aggregate aspect-ratio expansion. */
#define BACA_LAYOUT_MAX_WIDTH 1024
#define BACA_LAYOUT_MAX_LINES 262144U
#define BACA_LAYOUT_MAX_IMAGE_ROWS 1024
#define BACA_LAYOUT_MAX_IMAGE_EXTRA_ROWS 65536U

typedef enum BacaLayoutLineKind : uint8_t {
    BACA_LAYOUT_TEXT = 0,
    BACA_LAYOUT_IMAGE,
    BACA_LAYOUT_BLANK,
} BacaLayoutLineKind;

typedef struct BacaLayoutLine {
    BacaLayoutLineKind kind;
    size_t block_index;
    size_t byte_start;
    size_t byte_end;
    size_t logical_offset;
    int indent;
    int image_row;
    int image_rows;
    bool paragraph_end;
} BacaLayoutLine;

typedef struct BacaLayout {
    const BacaDocument *document;
    BacaLayoutLine *lines;
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
    BacaJustification justification;
    BacaPresentation presentation;
    const atomic_bool *cancel;
} BacaLayout;

typedef struct BacaSearchMatch {
    size_t line;
    size_t byte_start;
    size_t byte_end;
    size_t block_index;
    size_t block_offset;
} BacaSearchMatch;

void baca_layout_free(BacaLayout *layout);
[[nodiscard]] bool baca_layout_build(BacaLayout *layout, const BacaDocument *document, int width,
                                     BacaJustification justification, BacaError *error);
[[nodiscard]] bool baca_layout_build_presentation(BacaLayout *layout, const BacaDocument *document, int width,
                                                   BacaJustification justification, BacaPresentation presentation,
                                                   int cell_pixel_width, int cell_pixel_height,
                                                   const atomic_bool *cancel, BacaError *error);
[[nodiscard]] char *baca_layout_line_text(const BacaLayout *layout, size_t line_index, bool justify,
                                           BacaError *error);
[[nodiscard]] size_t baca_layout_target_line(const BacaLayout *layout, const char *target);
[[nodiscard]] size_t baca_layout_section_for_line(const BacaLayout *layout, size_t line,
                                                  size_t *comparison_count);
[[nodiscard]] double baca_layout_progress(size_t scroll_line, size_t maximum_scroll);
[[nodiscard]] size_t baca_layout_restore_progress(double progress, size_t maximum_scroll);
[[nodiscard]] bool baca_layout_search(const BacaLayout *layout, const char *pattern, BacaSearchMatch **matches,
                                      size_t *match_count, BacaError *error);
