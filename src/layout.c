#include "baca/layout.h"
#include "baca/graphics.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <glib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    BACA_SEARCH_MATCH_LIMIT = 10000,
    BACA_SEARCH_ATTEMPT_LIMIT = 1000000,
    BACA_SEARCH_PCRE_MATCH_LIMIT = 1000000,
    BACA_SEARCH_PCRE_DEPTH_LIMIT = 1000,
};

static void layout_set_error(BacaError *error, BacaErrorCode code, const char *message) {
    if (error != NULL) {
        baca_error_set(error, code, "%s", message);
    }
}

static bool layout_cancelled(const BacaLayout *layout) {
    return layout->cancel != NULL && atomic_load_explicit(layout->cancel, memory_order_relaxed);
}

static bool layout_is_control_byte(char value) {
    unsigned char character = (unsigned char)value;
    return character < 0x20U || character == 0x7fU;
}

static size_t layout_utf8_next(const char *text, size_t length, size_t offset, int *columns) {
    if (offset < length && layout_is_control_byte(text[offset])) {
        *columns = 1;
        return offset + 1U;
    }

    int cell_width = 0;
    size_t next = baca_utf8_next(text, length, offset, &cell_width);

    if (next <= offset || next > length) {
        next = offset + 1U;
        cell_width = 1;
    } else if (cell_width < 0) {
        cell_width = 1;
    }

    *columns = cell_width;
    return next;
}

static bool layout_slice_width(const BacaLayout *layout, const char *text, size_t start, size_t end,
                               size_t *width, BacaError *error) {
    size_t offset = start;
    size_t columns = 0U;

    while (offset < end) {
        if (layout_cancelled(layout)) {
            return false;
        }
        int cell_width = 0;
        size_t next = layout_utf8_next(text, end, offset, &cell_width);
        size_t added = (size_t)cell_width;

        if (added > SIZE_MAX - columns) {
            layout_set_error(error, BACA_ERROR_MEMORY, "text width overflow");
            return false;
        }
        columns += added;
        offset = next;
    }

    *width = columns;
    return true;
}

static bool layout_add_line(BacaLayout *layout, BacaLayoutLine line, BacaError *error) {
    if (layout_cancelled(layout)) {
        return false;
    }
    if (layout->line_count >= BACA_LAYOUT_MAX_LINES) {
        layout_set_error(error, BACA_ERROR_CORRUPT,
                         "layout exceeds the supported line limit of 262144");
        return false;
    }

    size_t needed = layout->line_count + 1U;
    BacaError reserve_error = {0};
    BacaLayoutLine *lines =
        baca_array_reserve(layout->lines, &layout->line_capacity, sizeof(*layout->lines), needed, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (error != NULL) {
            *error = reserve_error;
        }
        return false;
    }
    layout->lines = lines;

    layout->lines[layout->line_count] = line;
    layout->line_count = needed;
    return true;
}

static bool layout_advance(BacaLayout *layout, size_t amount, BacaError *error) {
    if (amount > SIZE_MAX - layout->logical_length) {
        layout_set_error(error, BACA_ERROR_MEMORY, "layout logical length overflow");
        return false;
    }
    layout->logical_length += amount;
    return true;
}

static bool layout_add_blank(BacaLayout *layout, size_t block_index, size_t byte_offset, size_t logical_offset,
                             BacaError *error) {
    BacaLayoutLine line = {
        .kind = BACA_LAYOUT_BLANK,
        .block_index = block_index,
        .byte_start = byte_offset,
        .byte_end = byte_offset,
        .logical_offset = logical_offset,
        .indent = 0,
        .image_row = 0,
        .image_rows = 0,
        .paragraph_end = true,
    };
    return layout_add_line(layout, line, error);
}

static bool layout_add_text_line(BacaLayout *layout, size_t block_index, const char *text, size_t start,
                                 size_t end, size_t logical_base, bool paragraph_end, BacaError *error) {
    if (start > SIZE_MAX - logical_base) {
        layout_set_error(error, BACA_ERROR_MEMORY, "layout logical offset overflow");
        return false;
    }

    size_t columns = 0U;
    if (!layout_slice_width(layout, text, start, end, &columns, error)) {
        return false;
    }

    BacaJustification alignment = layout->justification;
    const BacaBlock *block = &layout->document->blocks[block_index];
    if (block->value.text.heading_level == 1U) {
        alignment = BACA_JUSTIFY_CENTER;
    } else if (block->value.text.heading_level > 1U) {
        alignment = BACA_JUSTIFY_LEFT;
    }

    int indent = 0;
    size_t available = (size_t)layout->width;
    if (columns < available) {
        size_t remaining = available - columns;
        if (alignment == BACA_JUSTIFY_CENTER) {
            indent = (int)(remaining / 2U);
        } else if (alignment == BACA_JUSTIFY_RIGHT) {
            indent = (int)remaining;
        }
    }

    BacaLayoutLine line = {
        .kind = BACA_LAYOUT_TEXT,
        .block_index = block_index,
        .byte_start = start,
        .byte_end = end,
        .logical_offset = logical_base + start,
        .indent = indent,
        .image_row = 0,
        .image_rows = 0,
        .paragraph_end = paragraph_end,
    };
    return layout_add_line(layout, line, error);
}

static bool layout_is_horizontal_space(char value) {
    return value == ' ' || layout_is_control_byte(value);
}

static size_t layout_skip_horizontal_space(const BacaLayout *layout, const char *text, size_t offset,
                                           size_t end) {
    while (offset < end && layout_is_horizontal_space(text[offset])) {
        if (layout_cancelled(layout)) {
            break;
        }
        offset++;
    }
    return offset;
}

static bool layout_wrap_preformatted(BacaLayout *layout, size_t block_index, const char *text, size_t start,
                                     size_t end, size_t logical_base, bool heading, BacaError *error) {
    if (start == end) {
        return layout_add_blank(layout, block_index, start, logical_base + start, error);
    }

    size_t cursor = start;
    size_t target_width = (size_t)layout->width;
    while (cursor < end) {
        if (layout_cancelled(layout)) {
            return false;
        }
        size_t line_start = cursor;
        size_t columns = 0U;

        while (cursor < end) {
            if (layout_cancelled(layout)) {
                return false;
            }
            int cell_width = 0;
            size_t next = layout_utf8_next(text, end, cursor, &cell_width);
            size_t added = (size_t)cell_width;
            bool exceeds = columns > target_width || added > target_width - columns;

            if (exceeds && cursor > line_start) {
                break;
            }
            if (added > SIZE_MAX - columns) {
                layout_set_error(error, BACA_ERROR_MEMORY, "text width overflow");
                return false;
            }
            columns += added;
            cursor = next;
            if (exceeds) {
                break;
            }
        }

        bool paragraph_end = heading || cursor == end;
        if (!layout_add_text_line(layout, block_index, text, line_start, cursor, logical_base, paragraph_end,
                                  error)) {
            return false;
        }
    }
    return true;
}

static bool layout_wrap_normal(BacaLayout *layout, size_t block_index, const char *text, size_t start, size_t end,
                               size_t logical_base, bool heading, BacaError *error) {
    size_t cursor = start;
    size_t target_width = (size_t)layout->width;
    bool emitted = false;

    while (cursor < end) {
        if (layout_cancelled(layout)) {
            return false;
        }
        cursor = layout_skip_horizontal_space(layout, text, cursor, end);
        if (layout_cancelled(layout)) {
            return false;
        }
        if (cursor == end) {
            break;
        }

        size_t line_start = cursor;
        size_t line_end = end;
        size_t next_cursor = end;
        size_t scan = cursor;
        size_t columns = 0U;
        size_t last_break = SIZE_MAX;
        bool in_space = false;
        bool wrapped = false;

        while (scan < end) {
            if (layout_cancelled(layout)) {
                return false;
            }
            int cell_width = 0;
            size_t next = layout_utf8_next(text, end, scan, &cell_width);
            size_t added = (size_t)cell_width;
            bool horizontal_space = next == scan + 1U && layout_is_horizontal_space(text[scan]);

            if (horizontal_space) {
                if (!in_space) {
                    last_break = scan;
                }
                in_space = true;
            } else {
                in_space = false;
            }

            bool exceeds = columns > target_width || added > target_width - columns;
            if (exceeds) {
                if (scan == line_start) {
                    line_end = next;
                    next_cursor = next;
                } else if (last_break != SIZE_MAX && last_break > line_start) {
                    line_end = last_break;
                    next_cursor = layout_skip_horizontal_space(layout, text, last_break, end);
                } else {
                    line_end = scan;
                    next_cursor = scan;
                }
                wrapped = true;
                break;
            }

            if (added > SIZE_MAX - columns) {
                layout_set_error(error, BACA_ERROR_MEMORY, "text width overflow");
                return false;
            }
            columns += added;
            scan = next;
        }

        if (!wrapped) {
            line_end = end;
            next_cursor = end;
        }
        while (line_end > line_start && layout_is_horizontal_space(text[line_end - 1U])) {
            line_end--;
        }

        if (line_end > line_start) {
            bool paragraph_end = heading || next_cursor == end;
            if (!layout_add_text_line(layout, block_index, text, line_start, line_end, logical_base,
                                      paragraph_end, error)) {
                return false;
            }
            emitted = true;
        }

        if (next_cursor <= cursor) {
            int ignored_width = 0;
            next_cursor = layout_utf8_next(text, end, cursor, &ignored_width);
        }
        cursor = next_cursor;
    }

    if (!emitted) {
        return layout_add_blank(layout, block_index, start, logical_base + start, error);
    }
    return true;
}

static bool layout_add_text_block(BacaLayout *layout, size_t block_index, const BacaTextBlock *block,
                                  BacaError *error) {
    const char *text = block->text == NULL ? "" : block->text;
    size_t length = strlen(text);
    size_t logical_base = layout->logical_length;

    if (length == SIZE_MAX || length + 1U > SIZE_MAX - logical_base) {
        layout_set_error(error, BACA_ERROR_MEMORY, "layout logical length overflow");
        return false;
    }

    size_t start = 0U;
    for (;;) {
        if (layout_cancelled(layout)) {
            return false;
        }
        size_t end = start;
        while (end < length && text[end] != '\n' && text[end] != '\r') {
            if (layout_cancelled(layout)) {
                return false;
            }
            end++;
        }

        bool heading = block->heading_level > 0U;
        bool ok = block->preformatted
                      ? layout_wrap_preformatted(layout, block_index, text, start, end, logical_base, heading, error)
                      : layout_wrap_normal(layout, block_index, text, start, end, logical_base, heading, error);
        if (!ok) {
            return false;
        }
        if (end == length) {
            break;
        }

        if (text[end] == '\r' && end + 1U < length && text[end + 1U] == '\n') {
            start = end + 2U;
        } else {
            start = end + 1U;
        }
    }

    layout->logical_length = logical_base + length + 1U;
    return true;
}

static bool layout_add_image_block(BacaLayout *layout, size_t block_index, const BacaImageBlock *image,
                                    BacaError *error) {
    int image_rows = 1;
    int indent = 0;
    bool image_placeholder =
        image->broken || image->intrinsic_width <= 0 || image->intrinsic_height <= 0;
    if (!image_placeholder) {
        const long double rows = ceill((long double)image->intrinsic_height * (long double)layout->width *
                                       (long double)layout->cell_pixel_width /
                                       ((long double)image->intrinsic_width *
                                         (long double)layout->cell_pixel_height));
        if (!isfinite(rows) || rows > (long double)BACA_LAYOUT_MAX_IMAGE_ROWS) {
            if (layout->document->format == BACA_FORMAT_PDF) {
                layout_set_error(error, BACA_ERROR_CORRUPT,
                                 "image aspect ratio exceeds the supported per-image row limit of 1024");
                return false;
            }
            image_placeholder = true;
        }
        if (!image_placeholder && rows > 1.0L) {
            image_rows = (int)rows;
        }
    }
    if (image_placeholder && layout->width > 5) {
        indent = (layout->width - 5) / 2;
    }

    size_t extra_rows = (size_t)(image_rows - 1);
    if (layout->image_extra_rows > BACA_LAYOUT_MAX_IMAGE_EXTRA_ROWS ||
        extra_rows > BACA_LAYOUT_MAX_IMAGE_EXTRA_ROWS - layout->image_extra_rows) {
        layout_set_error(error, BACA_ERROR_CORRUPT,
                         "aggregate image layout exceeds the supported extra-row limit of 65536");
        return false;
    }
    layout->image_extra_rows += extra_rows;

    for (int row = 0; row < image_rows; ++row) {
        if (layout_cancelled(layout)) {
            return false;
        }
        BacaLayoutLine line = {
            .kind = BACA_LAYOUT_IMAGE,
            .block_index = block_index,
            .byte_start = 0U,
            .byte_end = 0U,
            .logical_offset = layout->logical_length,
            .indent = indent,
            .image_row = row,
            .image_rows = image_rows,
            .image_placeholder = image_placeholder,
            .paragraph_end = row + 1 == image_rows,
        };
        if (!layout_add_line(layout, line, error)) {
            return false;
        }
    }
    return layout_advance(layout, 1U, error);
}

static size_t layout_block_end_offset(const BacaBlock *block) {
    if (block->kind == BACA_BLOCK_TEXT && block->value.text.text != NULL) {
        return strlen(block->value.text.text);
    }
    return 0U;
}

void baca_layout_free(BacaLayout *layout) {
    if (layout == NULL) {
        return;
    }

    free(layout->lines);
    free(layout->block_first_line);
    free(layout->section_first_line);
    free(layout->block_section);
    *layout = (BacaLayout){0};
}

static bool layout_block_visible(const BacaBlock *block, BacaPresentation presentation) {
    return block->presentation == BACA_PRESENTATION_DEFAULT || presentation == BACA_PRESENTATION_DEFAULT ||
           block->presentation == presentation;
}

static bool layout_build_section_indexes(BacaLayout *layout, BacaError *error) {
    const BacaDocument *document = layout->document;
    size_t previous_end = 0U;
    for (size_t section_index = 0U; section_index < document->section_count; ++section_index) {
        if (layout_cancelled(layout)) {
            return false;
        }
        const BacaSection *section = &document->sections[section_index];
        if (section->first_block > document->block_count ||
            section->block_count > document->block_count - section->first_block ||
            section->first_block < previous_end) {
            layout_set_error(error, BACA_ERROR_CORRUPT,
                             "document sections are overlapping or out of range");
            return false;
        }
        size_t first_line = layout->line_count;
        if (section->first_block < document->block_count) {
            first_line = layout->block_first_line[section->first_block];
        }
        if (layout->line_count > 0U && first_line >= layout->line_count) {
            first_line = layout->line_count - 1U;
        }
        layout->section_first_line[section_index] = first_line;
        const size_t end = section->first_block + section->block_count;
        for (size_t block_index = section->first_block; block_index < end; ++block_index) {
            if (layout_cancelled(layout)) {
                return false;
            }
            layout->block_section[block_index] = section_index;
        }
        previous_end = end;
    }
    return true;
}

bool baca_layout_build_presentation(BacaLayout *layout, const BacaDocument *document, int width,
                                    BacaJustification justification, BacaPresentation presentation,
                                    int cell_pixel_width, int cell_pixel_height,
                                    const atomic_bool *cancel, BacaError *error) {
    if (layout == NULL || document == NULL) {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "layout and document are required");
        return false;
    }
    if (width <= 0) {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "layout width must be positive");
        return false;
    }
    if (justification > BACA_JUSTIFY_RIGHT) {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "invalid text justification");
        return false;
    }
    if (presentation > BACA_PRESENTATION_REFLOW || cell_pixel_width <= 0 || cell_pixel_height <= 0 ||
        cell_pixel_width > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
        cell_pixel_height > BACA_GRAPHICS_MAX_SOURCE_DIMENSION) {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "invalid layout presentation or cell pixel dimensions");
        return false;
    }
    if (document->block_count > 0U && document->blocks == NULL) {
        layout_set_error(error, BACA_ERROR_CORRUPT, "document block array is missing");
        return false;
    }

    BacaLayout built = {
        .document = document,
        .width = width > BACA_LAYOUT_MAX_WIDTH ? BACA_LAYOUT_MAX_WIDTH : width,
        .cell_pixel_width = cell_pixel_width,
        .cell_pixel_height = cell_pixel_height,
        .justification = justification,
        .presentation = presentation,
        .cancel = cancel,
    };

    if (layout_cancelled(&built)) {
        return false;
    }

    if (document->block_count > 0U) {
        built.block_first_line =
            baca_reallocarray(NULL, document->block_count, sizeof(*built.block_first_line), error);
        built.block_section = baca_reallocarray(NULL, document->block_count,
                                                sizeof(*built.block_section), error);
        if (built.block_first_line == NULL || built.block_section == NULL) {
            baca_layout_free(&built);
            return false;
        }
        for (size_t block_index = 0U; block_index < document->block_count; ++block_index) {
            built.block_section[block_index] = SIZE_MAX;
        }
    }
    if (document->section_count > 0U) {
        if (document->sections == NULL) {
            baca_layout_free(&built);
            layout_set_error(error, BACA_ERROR_CORRUPT, "document section array is missing");
            return false;
        }
        built.section_first_line = baca_reallocarray(
            NULL, document->section_count, sizeof(*built.section_first_line), error);
        if (built.section_first_line == NULL) {
            baca_layout_free(&built);
            return false;
        }
    }

    size_t previous_visible = SIZE_MAX;
    for (size_t block_index = 0U; block_index < document->block_count; block_index++) {
        if (layout_cancelled(&built)) {
            baca_layout_free(&built);
            return false;
        }
        const BacaBlock *block = &document->blocks[block_index];
        built.block_first_line[block_index] = built.line_count;
        if (!layout_block_visible(block, presentation)) {
            continue;
        }

        if (previous_visible != SIZE_MAX && built.line_count > 0U &&
            built.lines[built.line_count - 1U].kind != BACA_LAYOUT_BLANK &&
            block->kind != BACA_BLOCK_PAGE_BREAK) {
            const BacaBlock *previous = &document->blocks[previous_visible];
            const size_t byte_offset = layout_block_end_offset(previous);
            if (!layout_add_blank(&built, previous_visible, byte_offset, built.logical_length, error) ||
                !layout_advance(&built, 1U, error)) {
                baca_layout_free(&built);
                return false;
            }
            built.block_first_line[block_index] = built.line_count;
        }

        bool ok = false;
        switch (block->kind) {
        case BACA_BLOCK_TEXT:
            ok = layout_add_text_block(&built, block_index, &block->value.text, error);
            break;
        case BACA_BLOCK_IMAGE:
            ok = layout_add_image_block(&built, block_index, &block->value.image, error);
            break;
        case BACA_BLOCK_PAGE_BREAK:
            ok = layout_add_blank(&built, block_index, 0U, built.logical_length, error) &&
                 layout_advance(&built, 1U, error);
            break;
        default:
            layout_set_error(error, BACA_ERROR_CORRUPT, "document contains an invalid block kind");
            ok = false;
            break;
        }

        if (!ok) {
            baca_layout_free(&built);
            return false;
        }
        previous_visible = block_index;
    }

    if (!layout_build_section_indexes(&built, error)) {
        baca_layout_free(&built);
        return false;
    }

    built.cancel = NULL;
    baca_layout_free(layout);
    *layout = built;
    return true;
}

bool baca_layout_build(BacaLayout *layout, const BacaDocument *document, int width, BacaJustification justification,
                       BacaError *error) {
    const BacaPresentation presentation =
        document == NULL ? BACA_PRESENTATION_DEFAULT : document->default_presentation;
    return baca_layout_build_presentation(layout, document, width, justification, presentation, 1, 2,
                                          NULL, error);
}

static char *layout_allocate_text(size_t length, BacaError *error) {
    if (length == SIZE_MAX) {
        layout_set_error(error, BACA_ERROR_MEMORY, "line text length overflow");
        return NULL;
    }

    char *text = baca_reallocarray(NULL, length + 1U, sizeof(*text), error);
    if (text != NULL) {
        text[length] = '\0';
    }
    return text;
}

static bool layout_is_interword_gap(const char *text, size_t length, size_t start, size_t end) {
    return start > 0U && end < length && !layout_is_horizontal_space(text[start - 1U]) &&
           !layout_is_horizontal_space(text[end]);
}

static size_t layout_count_gaps(const char *text, size_t length) {
    size_t gaps = 0U;
    size_t offset = 0U;

    while (offset < length) {
        if (!layout_is_horizontal_space(text[offset])) {
            offset++;
            continue;
        }

        size_t start = offset;
        while (offset < length && layout_is_horizontal_space(text[offset])) {
            offset++;
        }
        if (layout_is_interword_gap(text, length, start, offset)) {
            gaps++;
        }
    }
    return gaps;
}

char *baca_layout_line_text(const BacaLayout *layout, size_t line_index, bool justify, BacaError *error) {
    if (layout == NULL || line_index >= layout->line_count || layout->lines == NULL) {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "layout line is out of range");
        return NULL;
    }

    const BacaLayoutLine *line = &layout->lines[line_index];
    if (line->indent < 0) {
        layout_set_error(error, BACA_ERROR_CORRUPT, "layout line has a negative indent");
        return NULL;
    }
    size_t indent = (size_t)line->indent;

    if (line->kind == BACA_LAYOUT_BLANK) {
        return layout_allocate_text(0U, error);
    }
    if (line->kind == BACA_LAYOUT_IMAGE) {
        if (line->image_rows > 1 && line->image_row != line->image_rows / 2) {
            return layout_allocate_text(0U, error);
        }
        static const char placeholder[] = "IMAGE";
        if (indent > SIZE_MAX - (sizeof(placeholder) - 1U)) {
            layout_set_error(error, BACA_ERROR_MEMORY, "line text length overflow");
            return NULL;
        }
        size_t length = indent + sizeof(placeholder) - 1U;
        char *result = layout_allocate_text(length, error);
        if (result == NULL) {
            return NULL;
        }
        memset(result, ' ', indent);
        memcpy(result + indent, placeholder, sizeof(placeholder) - 1U);
        return result;
    }
    if (line->kind != BACA_LAYOUT_TEXT || layout->document == NULL ||
        line->block_index >= layout->document->block_count || layout->document->blocks == NULL) {
        layout_set_error(error, BACA_ERROR_CORRUPT, "layout text line has no source block");
        return NULL;
    }

    const BacaBlock *block = &layout->document->blocks[line->block_index];
    if (block->kind != BACA_BLOCK_TEXT) {
        layout_set_error(error, BACA_ERROR_CORRUPT, "layout text line refers to a non-text block");
        return NULL;
    }

    const char *block_text = block->value.text.text == NULL ? "" : block->value.text.text;
    size_t block_length = strlen(block_text);
    if (line->byte_start > line->byte_end || line->byte_end > block_length) {
        layout_set_error(error, BACA_ERROR_CORRUPT, "layout text line has an invalid byte range");
        return NULL;
    }

    const char *source = block_text + line->byte_start;
    size_t source_length = line->byte_end - line->byte_start;
    size_t extra_spaces = 0U;
    size_t gaps = 0U;

    if (justify && layout->justification == BACA_JUSTIFY_FULL && !line->paragraph_end &&
        !block->value.text.preformatted && block->value.text.heading_level == 0U && layout->width > 0) {
        size_t columns = 0U;
        if (!layout_slice_width(layout, source, 0U, source_length, &columns, error)) {
            return NULL;
        }
        gaps = layout_count_gaps(source, source_length);
        size_t target_width = (size_t)layout->width;
        if (gaps > 0U && columns < target_width) {
            extra_spaces = target_width - columns;
        }
    }

    if (source_length > SIZE_MAX - indent || extra_spaces > SIZE_MAX - indent - source_length) {
        layout_set_error(error, BACA_ERROR_MEMORY, "line text length overflow");
        return NULL;
    }
    size_t result_length = indent + source_length + extra_spaces;
    char *result = layout_allocate_text(result_length, error);
    if (result == NULL) {
        return NULL;
    }

    memset(result, ' ', indent);
    size_t output = indent;
    size_t input = 0U;
    size_t gap_index = 0U;
    size_t per_gap = gaps == 0U ? 0U : extra_spaces / gaps;
    size_t remainder = gaps == 0U ? 0U : extra_spaces % gaps;

    while (input < source_length) {
        if (!layout_is_horizontal_space(source[input])) {
            result[output++] = source[input++];
            continue;
        }

        size_t run_start = input;
        while (input < source_length && layout_is_horizontal_space(source[input])) {
            input++;
        }
        size_t run_length = input - run_start;
        memset(result + output, ' ', run_length);
        output += run_length;

        if (layout_is_interword_gap(source, source_length, run_start, input)) {
            size_t added = per_gap + (gap_index < remainder ? 1U : 0U);
            memset(result + output, ' ', added);
            output += added;
            gap_index++;
        }
    }

    result[output] = '\0';
    return result;
}

static size_t layout_clamp_line(const BacaLayout *layout, size_t line) {
    if (layout->line_count == 0U) {
        return 0U;
    }
    return line < layout->line_count ? line : layout->line_count - 1U;
}

static size_t layout_line_for_block_offset(const BacaLayout *layout, size_t block_index, size_t offset) {
    size_t first = layout_clamp_line(layout, layout->block_first_line[block_index]);
    size_t candidate = first;
    bool saw_text = false;

    for (size_t line_index = first; line_index < layout->line_count; line_index++) {
        const BacaLayoutLine *line = &layout->lines[line_index];
        if (line->block_index != block_index) {
            break;
        }
        if (line->kind != BACA_LAYOUT_TEXT) {
            continue;
        }
        if (!saw_text) {
            candidate = line_index;
            saw_text = true;
        }
        if (line->byte_start > offset) {
            break;
        }
        candidate = line_index;
    }
    return candidate;
}

static bool layout_exact_string(const char *value, const char *target, size_t target_length) {
    return value != NULL && strlen(value) == target_length && memcmp(value, target, target_length) == 0;
}

static bool layout_find_section(const BacaLayout *layout, const char *target, size_t target_length, size_t *line) {
    const BacaDocument *document = layout->document;

    if (document->sections != NULL) {
        for (size_t index = 0U; index < document->section_count; index++) {
            const BacaSection *section = &document->sections[index];
            if (!layout_exact_string(section->id, target, target_length)) {
                continue;
            }
            if (section->first_block >= document->block_count) {
                return false;
            }
            *line = layout_clamp_line(layout, layout->section_first_line[index]);
            return true;
        }
    }

    for (size_t block_index = 0U; block_index < document->block_count; block_index++) {
        if (layout_exact_string(document->blocks[block_index].section_id, target, target_length)) {
            *line = layout_clamp_line(layout, layout->block_first_line[block_index]);
            return true;
        }
    }
    return false;
}

static size_t layout_image_fraction_line(const BacaLayout *layout, size_t first, double fraction) {
    first = layout_clamp_line(layout, first);
    const BacaLayoutLine *line = &layout->lines[first];
    if (line->kind != BACA_LAYOUT_IMAGE || line->image_rows <= 1 || !isfinite(fraction)) {
        return first;
    }
    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }
    const size_t span = (size_t)line->image_rows - 1U;
    const size_t offset = (size_t)llround(fraction * (double)span);
    const size_t bounded = offset > span ? span : offset;
    return layout_clamp_line(layout, bounded > SIZE_MAX - first ? SIZE_MAX : first + bounded);
}

static bool layout_pdf_target_y(const BacaLayout *layout, const char *target, size_t section_length, double *y) {
    if (layout->document->format != BACA_FORMAT_PDF || layout->presentation != BACA_PRESENTATION_FIXED ||
        strncmp(target + section_length, "#y=", 3U) != 0) {
        return false;
    }
    char *end = NULL;
    const double parsed = g_ascii_strtod(target + section_length + 3U, &end);
    if (end == target + section_length + 3U || *end != '\0' || !isfinite(parsed) || parsed < 0.0 || parsed > 1.0) {
        return false;
    }
    *y = parsed;
    return true;
}

static bool layout_pdf_page_target(const BacaLayout *layout, const char *target, size_t *page,
                                   size_t *section_length) {
    static const char prefix[] = "pdf://page/";
    if (layout->document->format != BACA_FORMAT_PDF ||
        strncmp(target, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    size_t value = 0U;
    size_t offset = sizeof(prefix) - 1U;
    const size_t first_digit = offset;
    while (target[offset] >= '0' && target[offset] <= '9') {
        const size_t digit = (size_t)(target[offset] - '0');
        if (value > (SIZE_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        ++offset;
    }
    if (offset == first_digit || (target[offset] != '\0' && target[offset] != '#' && target[offset] != '?')) {
        return false;
    }
    *page = value;
    *section_length = offset;
    return true;
}

size_t baca_layout_target_line(const BacaLayout *layout, const char *target) {
    if (layout == NULL || layout->document == NULL || target == NULL || target[0] == '\0' ||
        layout->line_count == 0U ||
        layout->lines == NULL ||
        (layout->document->block_count > 0U &&
          (layout->document->blocks == NULL || layout->block_first_line == NULL))) {
        return SIZE_MAX;
    }

    const BacaDocument *document = layout->document;
    size_t pdf_page = 0U;
    size_t pdf_section_length = 0U;
    if (layout_pdf_page_target(layout, target, &pdf_page, &pdf_section_length)) {
        if (pdf_page >= document->section_count || layout->section_first_line == NULL) {
            return SIZE_MAX;
        }
        const size_t line = layout_clamp_line(layout, layout->section_first_line[pdf_page]);
        double y = 0.0;
        return layout_pdf_target_y(layout, target, pdf_section_length, &y)
                   ? layout_image_fraction_line(layout, line, y)
                   : line;
    }
    for (size_t block_index = 0U; block_index < document->block_count; block_index++) {
        const BacaBlock *block = &document->blocks[block_index];
        if (block->kind == BACA_BLOCK_TEXT && block->value.text.anchors != NULL) {
            for (size_t anchor_index = 0U; anchor_index < block->value.text.anchor_count; anchor_index++) {
                const BacaAnchor *anchor = &block->value.text.anchors[anchor_index];
                if (anchor->target != NULL && strcmp(anchor->target, target) == 0) {
                    return layout_line_for_block_offset(layout, block_index, anchor->offset);
                }
            }
        } else if (block->kind == BACA_BLOCK_IMAGE && block->value.image.anchor != NULL &&
                   strcmp(block->value.image.anchor, target) == 0) {
            return layout_clamp_line(layout, layout->block_first_line[block_index]);
        }
    }

    size_t line = 0U;
    size_t target_length = strlen(target);
    if (layout_find_section(layout, target, target_length, &line)) {
        return line;
    }

    size_t section_length = strcspn(target, "#?");
    if (section_length < target_length && section_length > 0U &&
        layout_find_section(layout, target, section_length, &line)) {
        double y = 0.0;
        if (layout_pdf_target_y(layout, target, section_length, &y)) {
            return layout_image_fraction_line(layout, line, y);
        }
        return line;
    }
    return SIZE_MAX;
}

size_t baca_layout_section_for_line(const BacaLayout *layout, size_t line, size_t *comparison_count) {
    if (comparison_count != NULL) {
        *comparison_count = 0U;
    }
    if (layout == NULL || layout->document == NULL || layout->document->section_count == 0U ||
        layout->section_first_line == NULL || layout->line_count == 0U ||
        line < layout->section_first_line[0]) {
        return SIZE_MAX;
    }

    size_t low = 0U;
    size_t high = layout->document->section_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2U;
        if (comparison_count != NULL) {
            ++*comparison_count;
        }
        if (layout->section_first_line[middle] <= line) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low == 0U ? SIZE_MAX : low - 1U;
}

double baca_layout_progress(size_t scroll_line, size_t maximum_scroll) {
    if (maximum_scroll == 0U) {
        return 0.0;
    }
    if (scroll_line >= maximum_scroll) {
        return 1.0;
    }
    return (double)scroll_line / (double)maximum_scroll;
}

size_t baca_layout_restore_progress(double progress, size_t maximum_scroll) {
    if (maximum_scroll == 0U || !isfinite(progress) || !(progress > 0.0)) {
        return 0U;
    }
    if (progress >= 1.0) {
        return maximum_scroll;
    }

    long double target = (long double)progress * (long double)maximum_scroll;
    size_t restored = (size_t)(target + 0.5L);
    return restored > maximum_scroll ? maximum_scroll : restored;
}

static void layout_set_pcre_error(BacaError *error, BacaErrorCode code, const char *context, int pcre_error,
                                  size_t offset) {
    if (error == NULL) {
        return;
    }

    PCRE2_UCHAR message[256];
    int length = pcre2_get_error_message(pcre_error, message, sizeof(message));
    if (length >= 0) {
        baca_error_set(error, code, "%s at byte %zu: %s", context, offset, (const char *)message);
    } else {
        baca_error_set(error, code, "%s at byte %zu (PCRE2 error %d)", context, offset, pcre_error);
    }
}

static bool layout_add_match(BacaSearchMatch **matches, size_t *count, size_t *capacity, size_t line,
                             size_t start, size_t end, size_t block_index, size_t block_offset,
                             BacaError *error) {
    if (*count >= BACA_SEARCH_MATCH_LIMIT) {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "search produced too many matches (limit 10000)");
        return false;
    }

    size_t needed = *count + 1U;
    BacaError reserve_error = {0};
    BacaSearchMatch *resized = baca_array_reserve(*matches, capacity, sizeof(**matches), needed, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (error != NULL) {
            *error = reserve_error;
        }
        return false;
    }
    *matches = resized;

    (*matches)[*count] = (BacaSearchMatch){
        .line = line,
        .byte_start = start,
        .byte_end = end,
        .block_index = block_index,
        .block_offset = block_offset,
    };
    *count = needed;
    return true;
}

static bool layout_search_subject(pcre2_code *regex, pcre2_match_data *match_data,
                                  pcre2_match_context *match_context, const char *subject,
                                  size_t subject_length, size_t line, size_t display_prefix,
                                  size_t block_index, size_t block_base, size_t *attempt_count,
                                  BacaSearchMatch **matches, size_t *match_count, size_t *match_capacity,
                                  BacaError *error) {
    PCRE2_SIZE start_offset = 0U;
    uint32_t options = 0U;
    for (;;) {
        if (*attempt_count >= BACA_SEARCH_ATTEMPT_LIMIT) {
            layout_set_error(error, BACA_ERROR_ARGUMENT, "search work limit exceeded");
            return false;
        }
        ++*attempt_count;
        const int result = pcre2_match(regex, (PCRE2_SPTR)subject, (PCRE2_SIZE)subject_length, start_offset,
                                       options, match_data, match_context);
        if (result == PCRE2_ERROR_NOMATCH && options != 0U) {
            options = 0U;
            if ((size_t)start_offset >= subject_length) {
                return true;
            }
            int ignored_width = 0;
            start_offset = (PCRE2_SIZE)layout_utf8_next(subject, subject_length, (size_t)start_offset,
                                                       &ignored_width);
            continue;
        }
        if (result == PCRE2_ERROR_NOMATCH) {
            return true;
        }
        if (result < 0) {
            BacaErrorCode code = BACA_ERROR_CORRUPT;
            if (result == PCRE2_ERROR_NOMEMORY) {
                code = BACA_ERROR_MEMORY;
            } else if (result == PCRE2_ERROR_MATCHLIMIT || result == PCRE2_ERROR_DEPTHLIMIT) {
                code = BACA_ERROR_ARGUMENT;
            }
            layout_set_pcre_error(error, code, "regex matching failed", result, (size_t)start_offset);
            return false;
        }

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        const size_t match_start = (size_t)ovector[0];
        const size_t match_end = (size_t)ovector[1];
        if (!layout_add_match(matches, match_count, match_capacity, line, display_prefix + match_start,
                              display_prefix + match_end, block_index, block_base + match_start, error)) {
            return false;
        }
        start_offset = ovector[1];
        options = ovector[0] == ovector[1] ? PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED : 0U;
    }
}

static size_t layout_section_line_for_block_offset(const BacaLayout *layout, size_t block_index,
                                                    size_t block_offset, size_t block_length) {
    const BacaDocument *document = layout->document;
    if (block_index < document->block_count && layout->block_section != NULL &&
        layout->section_first_line != NULL) {
        const size_t section_index = layout->block_section[block_index];
        if (section_index < document->section_count) {
            const size_t first = layout_clamp_line(layout, layout->section_first_line[section_index]);
            const double fraction = block_length == 0U
                                        ? 0.0
                                        : (double)(block_offset > block_length ? block_length : block_offset) /
                                              (double)block_length;
            return layout_image_fraction_line(layout, first, fraction);
        }
    }
    return layout_clamp_line(layout, layout->block_first_line[block_index]);
}

bool baca_layout_search(const BacaLayout *layout, const char *pattern, BacaSearchMatch **matches,
                        size_t *match_count, BacaError *error) {
    if (matches == NULL || match_count == NULL) {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "search output pointers are required");
        return false;
    }
    *matches = NULL;
    *match_count = 0U;

    if (layout == NULL || pattern == NULL || (layout->line_count > 0U && layout->lines == NULL)) {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "layout and search pattern are required");
        return false;
    }
    if (pattern[0] == '\0') {
        layout_set_error(error, BACA_ERROR_ARGUMENT, "search pattern is empty");
        return false;
    }

    int compile_error = 0;
    PCRE2_SIZE compile_offset = 0U;
    pcre2_code *regex = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                      PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP, &compile_error, &compile_offset,
                                      NULL);
    if (regex == NULL) {
        BacaErrorCode code = compile_error == PCRE2_ERROR_NOMEMORY ? BACA_ERROR_MEMORY : BACA_ERROR_ARGUMENT;
        layout_set_pcre_error(error, code, "invalid search pattern", compile_error, (size_t)compile_offset);
        return false;
    }

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(regex, NULL);
    if (match_data == NULL) {
        pcre2_code_free(regex);
        layout_set_error(error, BACA_ERROR_MEMORY, "could not allocate regex match data");
        return false;
    }
    pcre2_match_context *match_context = pcre2_match_context_create(NULL);
    if (match_context == NULL) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(regex);
        layout_set_error(error, BACA_ERROR_MEMORY, "could not allocate regex match context");
        return false;
    }
    (void)pcre2_set_match_limit(match_context, (uint32_t)BACA_SEARCH_PCRE_MATCH_LIMIT);
    (void)pcre2_set_depth_limit(match_context, (uint32_t)BACA_SEARCH_PCRE_DEPTH_LIMIT);

    BacaSearchMatch *found = NULL;
    size_t found_count = 0U;
    size_t found_capacity = 0U;
    size_t attempt_count = 0U;
    char *line_text = NULL;

    for (size_t line_index = 0U; line_index < layout->line_count; line_index++) {
        const BacaLayoutLine *line = &layout->lines[line_index];
        if (layout->document != NULL && layout->document->format == BACA_FORMAT_PDF &&
            line->kind != BACA_LAYOUT_TEXT) {
            continue;
        }
        line_text = baca_layout_line_text(layout, line_index, false, error);
        if (line_text == NULL) {
            goto fail;
        }

        size_t line_length = strlen(line_text);
        size_t prefix = line->indent > 0 ? (size_t)line->indent : 0U;
        if (prefix > line_length) {
            prefix = line_length;
        }
        const char *subject = line_text + prefix;
        size_t subject_length = line_length - prefix;
        if (!layout_search_subject(regex, match_data, match_context, subject, subject_length, line_index, prefix,
                                   line->block_index, line->byte_start, &attempt_count, &found, &found_count,
                                   &found_capacity, error)) {
            goto fail;
        }

        free(line_text);
        line_text = NULL;
    }

    if (layout->document != NULL && layout->document->format == BACA_FORMAT_PDF &&
        layout->presentation == BACA_PRESENTATION_FIXED) {
        for (size_t block_index = 0U; block_index < layout->document->block_count; ++block_index) {
            const BacaBlock *block = &layout->document->blocks[block_index];
            if (block->kind != BACA_BLOCK_TEXT || layout_block_visible(block, layout->presentation)) {
                continue;
            }
            const char *subject = block->value.text.text == NULL ? "" : block->value.text.text;
            const size_t subject_length = strlen(subject);
            const size_t first_match = found_count;
            const size_t line = layout_section_line_for_block_offset(layout, block_index, 0U, subject_length);
            if (!layout_search_subject(regex, match_data, match_context, subject, subject_length, line, 0U,
                                        block_index, 0U, &attempt_count, &found, &found_count, &found_capacity,
                                        error)) {
                goto fail;
            }
            for (size_t match_index = first_match; match_index < found_count; ++match_index) {
                found[match_index].line = layout_section_line_for_block_offset(
                    layout, block_index, found[match_index].block_offset, subject_length);
            }
        }
    }

    pcre2_match_context_free(match_context);
    pcre2_match_data_free(match_data);
    pcre2_code_free(regex);
    *matches = found;
    *match_count = found_count;
    return true;

fail:
    free(line_text);
    free(found);
    pcre2_match_context_free(match_context);
    pcre2_match_data_free(match_data);
    pcre2_code_free(regex);
    return false;
}
