#include "test_support.h"

#include "baca/layout.h"

#include <stdio.h>
#include <stdlib.h>

static bool append_text_block(BacaDocument *document, const char *section_id, const char *text,
                              unsigned heading_level, bool preformatted, const char *anchor, size_t anchor_offset,
                              BacaError *error) {
    BacaBlock block = {
        .kind = BACA_BLOCK_TEXT,
        .section_id = baca_strdup(section_id, error),
        .value.text = {
            .text = baca_strdup(text, error),
            .heading_level = heading_level,
            .preformatted = preformatted,
        },
    };
    if (anchor != NULL) {
        block.value.text.anchors = baca_reallocarray(NULL, 1U, sizeof(*block.value.text.anchors), error);
        if (block.value.text.anchors != NULL) {
            block.value.text.anchors[0] = (BacaAnchor){
                .target = baca_strdup(anchor, error),
                .offset = anchor_offset,
            };
            block.value.text.anchor_count = 1U;
            block.value.text.anchor_capacity = 1U;
        }
    }
    bool ready = block.section_id != NULL && block.value.text.text != NULL &&
                 (anchor == NULL || (block.value.text.anchors != NULL && block.value.text.anchors[0].target != NULL));
    if (ready && baca_document_add_text_block(document, &block, error)) {
        return true;
    }
    free(block.section_id);
    free(block.value.text.text);
    if (block.value.text.anchors != NULL) {
        free(block.value.text.anchors[0].target);
    }
    free(block.value.text.anchors);
    return false;
}

static bool append_image_block(BacaDocument *document, const char *section_id, const char *anchor,
                                int width, int height, bool broken, BacaError *error) {
    BacaBlock block = {
        .kind = BACA_BLOCK_IMAGE,
        .section_id = baca_strdup(section_id, error),
        .value.image = {
            .uri = baca_strdup("image.png", error),
            .anchor = baca_strdup(anchor, error),
            .page_index = -1,
            .intrinsic_width = width,
            .intrinsic_height = height,
            .broken = broken,
        },
    };
    if (block.section_id != NULL && block.value.image.uri != NULL && block.value.image.anchor != NULL &&
        baca_document_add_image_block(document, &block, error)) {
        return true;
    }
    free(block.section_id);
    free(block.value.image.uri);
    free(block.value.image.anchor);
    return false;
}

static bool append_section(BacaDocument *document, const char *id, size_t first_block, size_t block_count,
                           BacaError *error) {
    BacaSection section = {
        .id = baca_strdup(id, error),
        .first_block = first_block,
        .block_count = block_count,
        .linear = true,
    };
    if (section.id != NULL && baca_document_add_section(document, &section, error)) {
        return true;
    }
    free(section.id);
    return false;
}

static BacaTestResult test_utf8_cjk_combining_tabs_controls_and_pre(void) {
    BacaDocument document = {0};
    BacaError error = {0};
    static const char normal[] = "A界e\xcc\x81 B\tC\x01" "D";
    static const char pre[] = "a\tb\r\n界e\xcc\x81";
    TEST_ASSERT(append_text_block(&document, "normal.xhtml", normal, 0U, false, NULL, 0U, &error));
    TEST_ASSERT(append_text_block(&document, "pre.xhtml", pre, 0U, true, NULL, 0U, &error));
    BacaLayout layout = {0};
    TEST_ASSERT_MSG(baca_layout_build(&layout, &document, 20, BACA_JUSTIFY_LEFT, &error), "%s", error.message);
    TEST_ASSERT_SIZE(layout.block_first_line[0], 0U);
    char *line = baca_layout_line_text(&layout, layout.block_first_line[0], false, &error);
    TEST_ASSERT_STR(line, "A界e\xcc\x81 B C D");
    TEST_ASSERT_SIZE(baca_utf8_width(line, strlen(line)), 10U);
    free(line);

    size_t pre_line = layout.block_first_line[1];
    line = baca_layout_line_text(&layout, pre_line, false, &error);
    TEST_ASSERT_STR(line, "a b");
    free(line);
    line = baca_layout_line_text(&layout, pre_line + 1U, false, &error);
    TEST_ASSERT_STR(line, "界e\xcc\x81");
    TEST_ASSERT_SIZE(baca_utf8_width(line, strlen(line)), 3U);
    free(line);
    TEST_ASSERT(document.blocks[1].value.text.preformatted);
    baca_layout_free(&layout);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_wrapping_justification_and_alignment(void) {
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(append_text_block(&document, "wrap.xhtml", "one two three", 0U, false, NULL, 0U, &error));
    BacaLayout layout = {0};
    TEST_ASSERT(baca_layout_build(&layout, &document, 8, BACA_JUSTIFY_FULL, &error));
    TEST_ASSERT_SIZE(layout.line_count, 2U);
    TEST_ASSERT(!layout.lines[0].paragraph_end);
    TEST_ASSERT(layout.lines[1].paragraph_end);
    char *raw = baca_layout_line_text(&layout, 0U, false, &error);
    char *justified = baca_layout_line_text(&layout, 0U, true, &error);
    TEST_ASSERT_STR(raw, "one two");
    TEST_ASSERT_STR(justified, "one  two");
    TEST_ASSERT_SIZE(baca_utf8_width(justified, strlen(justified)), 8U);
    free(raw);
    free(justified);
    baca_layout_free(&layout);
    baca_document_close(&document);

    TEST_ASSERT(append_text_block(&document, "align.xhtml", "abc", 0U, false, NULL, 0U, &error));
    TEST_ASSERT(baca_layout_build(&layout, &document, 9, BACA_JUSTIFY_CENTER, &error));
    TEST_ASSERT_INT(layout.lines[0].indent, 3);
    baca_layout_free(&layout);
    TEST_ASSERT(baca_layout_build(&layout, &document, 9, BACA_JUSTIFY_RIGHT, &error));
    TEST_ASSERT_INT(layout.lines[0].indent, 6);
    baca_layout_free(&layout);
    baca_document_close(&document);

    TEST_ASSERT(append_text_block(&document, "headings.xhtml", "title", 1U, false, NULL, 0U, &error));
    TEST_ASSERT(append_text_block(&document, "headings.xhtml", "subtitle", 2U, false, NULL, 0U, &error));
    TEST_ASSERT(baca_layout_build(&layout, &document, 12, BACA_JUSTIFY_RIGHT, &error));
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[0]].indent, 3);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[1]].indent, 0);
    baca_layout_free(&layout);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_exact_fallback_and_missing_targets(void) {
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(append_text_block(&document, "chapter1.xhtml", "first chapter", 0U, false,
                                  "chapter1.xhtml#point", 3U, &error));
    TEST_ASSERT(append_text_block(&document, "chapter2.xhtml", "zero one two three four", 0U, false,
                                  "chapter2.xhtml#deep", 14U, &error));
    TEST_ASSERT(append_image_block(&document, "chapter2.xhtml", "chapter2.xhtml#image", 8, 4, false, &error));
    TEST_ASSERT(append_section(&document, "chapter1.xhtml", 0U, 1U, &error));
    TEST_ASSERT(append_section(&document, "chapter2.xhtml", 1U, 2U, &error));
    BacaLayout layout = {0};
    TEST_ASSERT(baca_layout_build(&layout, &document, 7, BACA_JUSTIFY_LEFT, &error));
    size_t chapter2_first = layout.block_first_line[1];
    size_t deep = baca_layout_target_line(&layout, "chapter2.xhtml#deep");
    TEST_ASSERT(deep != SIZE_MAX && deep > chapter2_first);
    TEST_ASSERT_SIZE(baca_layout_target_line(&layout, "chapter1.xhtml"), layout.block_first_line[0]);
    TEST_ASSERT_SIZE(baca_layout_target_line(&layout, "chapter2.xhtml#missing"), chapter2_first);
    TEST_ASSERT_SIZE(baca_layout_target_line(&layout, "chapter2.xhtml?edition=1"), chapter2_first);
    TEST_ASSERT_SIZE(baca_layout_target_line(&layout, "chapter2.xhtml#image"), layout.block_first_line[2]);
    TEST_ASSERT_SIZE(baca_layout_target_line(&layout, "missing.xhtml"), SIZE_MAX);
    baca_layout_free(&layout);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_progress_semantics(void) {
    TEST_ASSERT_DOUBLE(baca_layout_progress(0U, 0U), 0.0, 0.0);
    TEST_ASSERT_DOUBLE(baca_layout_progress(0U, 10U), 0.0, 0.0);
    TEST_ASSERT_DOUBLE(baca_layout_progress(5U, 10U), 0.5, 1e-15);
    TEST_ASSERT_DOUBLE(baca_layout_progress(10U, 10U), 1.0, 0.0);
    TEST_ASSERT_DOUBLE(baca_layout_progress(20U, 10U), 1.0, 0.0);
    TEST_ASSERT_SIZE(baca_layout_restore_progress(0.5, 11U), 6U);
    TEST_ASSERT_SIZE(baca_layout_restore_progress(1.0, 11U), 11U);
    TEST_ASSERT_SIZE(baca_layout_restore_progress(75.0, 11U), 11U);
    TEST_ASSERT_SIZE(baca_layout_restore_progress(-1.0, 11U), 0U);
    TEST_ASSERT_SIZE(baca_layout_restore_progress(NAN, 11U), 0U);
    return BACA_TEST_PASS;
}

static BacaTestResult test_image_aspect_rows_and_broken_placeholder(void) {
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#valid", 8, 4, false, &error));
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#broken", 0, 0, true, &error));
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#unprobed", 0, 0, false, &error));
    BacaLayout layout = {0};
    TEST_ASSERT_MSG(baca_layout_build(&layout, &document, 10, BACA_JUSTIFY_LEFT, &error), "%s", error.message);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[0]].image_rows, 3);
    TEST_ASSERT(!layout.lines[layout.block_first_line[0]].image_placeholder);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[0] + 2U].image_row, 2);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[1]].image_rows, 1);
    TEST_ASSERT(layout.lines[layout.block_first_line[1]].image_placeholder);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[2]].image_rows, 1);
    TEST_ASSERT(layout.lines[layout.block_first_line[2]].image_placeholder);
    char *placeholder = baca_layout_line_text(&layout, layout.block_first_line[1], false, &error);
    TEST_ASSERT_STR(placeholder, "  IMAGE");
    free(placeholder);

    baca_layout_free(&layout);
    TEST_ASSERT(baca_layout_build(&layout, &document, 8, BACA_JUSTIFY_LEFT, &error));
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[0]].image_rows, 2);
    baca_layout_free(&layout);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_image_row_limits_and_tall_placeholder(void) {
    BacaDocument document = {0};
    BacaError error = {0};
    const size_t full_images = BACA_LAYOUT_MAX_IMAGE_EXTRA_ROWS /
                               (BACA_LAYOUT_MAX_IMAGE_ROWS - 1U);
    for (size_t index = 0U; index < full_images; ++index) {
        char anchor[64] = {0};
        const int length = snprintf(anchor, sizeof(anchor), "images.xhtml#boundary-%zu", index);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(anchor));
        TEST_ASSERT(append_image_block(&document, "images.xhtml", anchor, 1,
                                       BACA_LAYOUT_MAX_IMAGE_ROWS * 2, false, &error));
    }
    const size_t remainder = BACA_LAYOUT_MAX_IMAGE_EXTRA_ROWS -
                             full_images * (BACA_LAYOUT_MAX_IMAGE_ROWS - 1U);
    TEST_ASSERT(remainder > 0U && remainder < BACA_LAYOUT_MAX_IMAGE_ROWS);
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#boundary-last", 1,
                                   (int)(remainder + 1U) * 2, false, &error));

    BacaLayout layout = {0};
    TEST_ASSERT_MSG(baca_layout_build(&layout, &document, 1, BACA_JUSTIFY_LEFT, &error), "%s",
                     error.message);
    TEST_ASSERT_INT(layout.lines[0].image_rows, BACA_LAYOUT_MAX_IMAGE_ROWS);
    TEST_ASSERT_SIZE(layout.image_extra_rows, BACA_LAYOUT_MAX_IMAGE_EXTRA_ROWS);
    const size_t boundary_count = full_images + 1U;
    TEST_ASSERT_SIZE(layout.line_count,
                     boundary_count + BACA_LAYOUT_MAX_IMAGE_EXTRA_ROWS + boundary_count - 1U);

    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#over-budget", 1, 4,
                                   false, &error));
    baca_error_clear(&error);
    TEST_ASSERT(!baca_layout_build(&layout, &document, 1, BACA_JUSTIFY_LEFT, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "aggregate image") != NULL);
    TEST_ASSERT_SIZE(layout.image_extra_rows, BACA_LAYOUT_MAX_IMAGE_EXTRA_ROWS);
    baca_layout_free(&layout);
    baca_document_close(&document);

    document.format = BACA_FORMAT_IMAGE;
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#too-tall", 1,
                                   BACA_LAYOUT_MAX_IMAGE_ROWS * 2 + 1, false, &error));
    baca_error_clear(&error);
    TEST_ASSERT_MSG(baca_layout_build(&layout, &document, 1, BACA_JUSTIFY_LEFT, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(layout.line_count, 1U);
    TEST_ASSERT_INT(layout.lines[0].image_rows, 1);
    TEST_ASSERT_INT(layout.lines[0].image_row, 0);
    TEST_ASSERT(layout.lines[0].image_placeholder);
    char *placeholder = baca_layout_line_text(&layout, 0U, false, &error);
    TEST_ASSERT_STR(placeholder, "IMAGE");
    free(placeholder);
    baca_layout_free(&layout);
    baca_document_close(&document);

    document.format = BACA_FORMAT_PDF;
    TEST_ASSERT(append_image_block(&document, "pdf://page/0", "pdf://page/0", 1,
                                   BACA_LAYOUT_MAX_IMAGE_ROWS * 2 + 1, false, &error));
    document.blocks[0].value.image.page_index = 0;
    baca_error_clear(&error);
    TEST_ASSERT(!baca_layout_build_presentation(&layout, &document, 1, BACA_JUSTIFY_LEFT,
                                                BACA_PRESENTATION_FIXED, 1, 2, NULL, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "per-image row limit") != NULL);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_line_limit_and_cancellation(void) {
    char *text = malloc(BACA_LAYOUT_MAX_LINES + 2U);
    TEST_ASSERT(text != NULL);
    memset(text, 'x', BACA_LAYOUT_MAX_LINES + 1U);
    text[BACA_LAYOUT_MAX_LINES + 1U] = '\0';

    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(append_text_block(&document, "limit.xhtml", text, 0U, false, NULL, 0U, &error));
    free(text);
    document.blocks[0].value.text.text[BACA_LAYOUT_MAX_LINES] = '\0';

    BacaLayout layout = {0};
    TEST_ASSERT_MSG(baca_layout_build(&layout, &document, 1, BACA_JUSTIFY_LEFT, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(layout.line_count, BACA_LAYOUT_MAX_LINES);
    document.blocks[0].value.text.text[BACA_LAYOUT_MAX_LINES] = 'x';
    baca_error_clear(&error);
    TEST_ASSERT(!baca_layout_build(&layout, &document, 1, BACA_JUSTIFY_LEFT, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "line limit") != NULL);
    TEST_ASSERT_SIZE(layout.line_count, BACA_LAYOUT_MAX_LINES);
    baca_layout_free(&layout);

    atomic_bool cancel;
    atomic_init(&cancel, true);
    baca_error_clear(&error);
    TEST_ASSERT(!baca_layout_build_presentation(&layout, &document, 1, BACA_JUSTIFY_LEFT,
                                                BACA_PRESENTATION_DEFAULT, 1, 2, &cancel, &error));
    TEST_ASSERT(!baca_error_is_set(&error));
    TEST_ASSERT(layout.lines == NULL && layout.block_first_line == NULL);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_regex_valid_invalid_empty_and_casefold(void) {
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(append_text_block(&document, "search.xhtml", "Alpha beta ALPHA界 alpha", 0U, false, NULL, 0U,
                                  &error));
    BacaLayout layout = {0};
    TEST_ASSERT(baca_layout_build(&layout, &document, 80, BACA_JUSTIFY_LEFT, &error));
    BacaSearchMatch *matches = NULL;
    size_t match_count = 0U;
    TEST_ASSERT_MSG(baca_layout_search(&layout, "alpha", &matches, &match_count, &error), "%s", error.message);
    TEST_ASSERT_SIZE(match_count, 3U);
    TEST_ASSERT_SIZE(matches[0].block_offset, 0U);
    TEST_ASSERT_SIZE(matches[1].block_offset, 11U);
    TEST_ASSERT_SIZE(matches[2].block_offset, 20U);
    free(matches);
    matches = NULL;
    match_count = 0U;
    TEST_ASSERT(baca_layout_search(&layout, "界", &matches, &match_count, &error));
    TEST_ASSERT_SIZE(match_count, 1U);
    free(matches);
    matches = (BacaSearchMatch *)1;
    match_count = 99U;
    TEST_ASSERT(!baca_layout_search(&layout, "(", &matches, &match_count, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    TEST_ASSERT(matches == NULL && match_count == 0U);
    matches = (BacaSearchMatch *)1;
    match_count = 99U;
    TEST_ASSERT(!baca_layout_search(&layout, "", &matches, &match_count, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_ARGUMENT);
    TEST_ASSERT(matches == NULL && match_count == 0U);
    baca_layout_free(&layout);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

static BacaTestResult test_resize_stable_search_identity(void) {
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(append_text_block(&document, "resize.xhtml", "prefix needle suffix words needle end", 0U, false,
                                  NULL, 0U, &error));
    BacaLayout layout = {0};
    TEST_ASSERT(baca_layout_build(&layout, &document, 80, BACA_JUSTIFY_LEFT, &error));
    BacaSearchMatch *wide = NULL;
    size_t wide_count = 0U;
    TEST_ASSERT(baca_layout_search(&layout, "needle", &wide, &wide_count, &error));
    TEST_ASSERT_SIZE(wide_count, 2U);
    TEST_ASSERT_SIZE(wide[0].line, 0U);
    TEST_ASSERT_SIZE(wide[1].line, 0U);

    TEST_ASSERT(baca_layout_build(&layout, &document, 8, BACA_JUSTIFY_LEFT, &error));
    BacaSearchMatch *narrow = NULL;
    size_t narrow_count = 0U;
    TEST_ASSERT(baca_layout_search(&layout, "needle", &narrow, &narrow_count, &error));
    TEST_ASSERT_SIZE(narrow_count, wide_count);
    for (size_t index = 0U; index < wide_count; index++) {
        TEST_ASSERT_SIZE(narrow[index].block_index, wide[index].block_index);
        TEST_ASSERT_SIZE(narrow[index].block_offset, wide[index].block_offset);
    }
    TEST_ASSERT(narrow[1].line > narrow[0].line);
    free(wide);
    free(narrow);
    baca_layout_free(&layout);
    baca_document_close(&document);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_layout_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "utf8_cjk_combining_tabs_controls_and_pre",
         .function = test_utf8_cjk_combining_tabs_controls_and_pre},
        {.name = "wrapping_justification_and_alignment", .function = test_wrapping_justification_and_alignment},
        {.name = "exact_fallback_and_missing_targets", .function = test_exact_fallback_and_missing_targets},
        {.name = "progress_semantics", .function = test_progress_semantics},
        {.name = "image_aspect_rows_and_broken_placeholder",
         .function = test_image_aspect_rows_and_broken_placeholder},
        {.name = "image_row_limits_and_tall_placeholder",
         .function = test_image_row_limits_and_tall_placeholder},
        {.name = "line_limit_and_cancellation", .function = test_line_limit_and_cancellation},
        {.name = "regex_valid_invalid_empty_and_casefold",
         .function = test_regex_valid_invalid_empty_and_casefold},
        {.name = "resize_stable_search_identity", .function = test_resize_stable_search_identity},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
