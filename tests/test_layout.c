#include "test_support.h"

#include "mereader-tui/layout.h"

#include <stdio.h>
#include <stdlib.h>

static bool append_text_block(MereaderTuiDocument *document, const char *section_id, const char *text,
                              unsigned heading_level, bool preformatted, const char *anchor, size_t anchor_offset,
                              MereaderTuiError *error) {
    MereaderTuiBlock block = {
        .kind = MEREADER_TUI_BLOCK_TEXT,
        .section_id = mereader_tui_strdup(section_id, error),
        .value.text = {
            .text = mereader_tui_strdup(text, error),
            .heading_level = heading_level,
            .preformatted = preformatted,
        },
    };
    if (anchor != NULL) {
        block.value.text.anchors = mereader_tui_reallocarray(NULL, 1U, sizeof(*block.value.text.anchors), error);
        if (block.value.text.anchors != NULL) {
            block.value.text.anchors[0] = (MereaderTuiAnchor){
                .target = mereader_tui_strdup(anchor, error),
                .offset = anchor_offset,
            };
            block.value.text.anchor_count = 1U;
            block.value.text.anchor_capacity = 1U;
        }
    }
    bool ready = block.section_id != NULL && block.value.text.text != NULL &&
                 (anchor == NULL || (block.value.text.anchors != NULL && block.value.text.anchors[0].target != NULL));
    if (ready && mereader_tui_document_add_text_block(document, &block, error)) {
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

static bool append_image_block(MereaderTuiDocument *document, const char *section_id, const char *anchor,
                                int width, int height, bool broken, MereaderTuiError *error) {
    MereaderTuiBlock block = {
        .kind = MEREADER_TUI_BLOCK_IMAGE,
        .section_id = mereader_tui_strdup(section_id, error),
        .value.image = {
            .uri = mereader_tui_strdup("image.png", error),
            .anchor = mereader_tui_strdup(anchor, error),
            .page_index = -1,
            .intrinsic_width = width,
            .intrinsic_height = height,
            .broken = broken,
        },
    };
    if (block.section_id != NULL && block.value.image.uri != NULL && block.value.image.anchor != NULL &&
        mereader_tui_document_add_image_block(document, &block, error)) {
        return true;
    }
    free(block.section_id);
    free(block.value.image.uri);
    free(block.value.image.anchor);
    return false;
}

static bool append_section(MereaderTuiDocument *document, const char *id, size_t first_block, size_t block_count,
                           MereaderTuiError *error) {
    MereaderTuiSection section = {
        .id = mereader_tui_strdup(id, error),
        .first_block = first_block,
        .block_count = block_count,
        .linear = true,
    };
    if (section.id != NULL && mereader_tui_document_add_section(document, &section, error)) {
        return true;
    }
    free(section.id);
    return false;
}

static MereaderTuiTestResult test_utf8_cjk_combining_tabs_controls_and_pre(void) {
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    static const char normal[] = "A界e\xcc\x81 B\tC\x01" "D";
    static const char pre[] = "a\tb\r\n界e\xcc\x81";
    TEST_ASSERT(append_text_block(&document, "normal.xhtml", normal, 0U, false, NULL, 0U, &error));
    TEST_ASSERT(append_text_block(&document, "pre.xhtml", pre, 0U, true, NULL, 0U, &error));
    MereaderTuiLayout layout = {0};
    TEST_ASSERT_MSG(mereader_tui_layout_build(&layout, &document, 20, MEREADER_TUI_JUSTIFY_LEFT, &error), "%s", error.message);
    TEST_ASSERT_SIZE(layout.block_first_line[0], 0U);
    char *line = mereader_tui_layout_line_text(&layout, layout.block_first_line[0], false, &error);
    TEST_ASSERT_STR(line, "A界e\xcc\x81 B C D");
    TEST_ASSERT_SIZE(mereader_tui_utf8_width(line, strlen(line)), 10U);
    free(line);

    size_t pre_line = layout.block_first_line[1];
    line = mereader_tui_layout_line_text(&layout, pre_line, false, &error);
    TEST_ASSERT_STR(line, "a b");
    free(line);
    line = mereader_tui_layout_line_text(&layout, pre_line + 1U, false, &error);
    TEST_ASSERT_STR(line, "界e\xcc\x81");
    TEST_ASSERT_SIZE(mereader_tui_utf8_width(line, strlen(line)), 3U);
    free(line);
    TEST_ASSERT(document.blocks[1].value.text.preformatted);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_wrapping_justification_and_alignment(void) {
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(append_text_block(&document, "wrap.xhtml", "one two three", 0U, false, NULL, 0U, &error));
    MereaderTuiLayout layout = {0};
    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 8, MEREADER_TUI_JUSTIFY_FULL, &error));
    TEST_ASSERT_SIZE(layout.line_count, 2U);
    TEST_ASSERT(!layout.lines[0].paragraph_end);
    TEST_ASSERT(layout.lines[1].paragraph_end);
    char *raw = mereader_tui_layout_line_text(&layout, 0U, false, &error);
    char *justified = mereader_tui_layout_line_text(&layout, 0U, true, &error);
    TEST_ASSERT_STR(raw, "one two");
    TEST_ASSERT_STR(justified, "one  two");
    TEST_ASSERT_SIZE(mereader_tui_utf8_width(justified, strlen(justified)), 8U);
    free(raw);
    free(justified);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);

    TEST_ASSERT(append_text_block(&document, "align.xhtml", "abc", 0U, false, NULL, 0U, &error));
    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 9, MEREADER_TUI_JUSTIFY_CENTER, &error));
    TEST_ASSERT_INT(layout.lines[0].indent, 3);
    mereader_tui_layout_free(&layout);
    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 9, MEREADER_TUI_JUSTIFY_RIGHT, &error));
    TEST_ASSERT_INT(layout.lines[0].indent, 6);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);

    TEST_ASSERT(append_text_block(&document, "headings.xhtml", "title", 1U, false, NULL, 0U, &error));
    TEST_ASSERT(append_text_block(&document, "headings.xhtml", "subtitle", 2U, false, NULL, 0U, &error));
    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 12, MEREADER_TUI_JUSTIFY_RIGHT, &error));
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[0]].indent, 3);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[1]].indent, 0);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_exact_fallback_and_missing_targets(void) {
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(append_text_block(&document, "chapter1.xhtml", "first chapter", 0U, false,
                                  "chapter1.xhtml#point", 3U, &error));
    TEST_ASSERT(append_text_block(&document, "chapter2.xhtml", "zero one two three four", 0U, false,
                                  "chapter2.xhtml#deep", 14U, &error));
    TEST_ASSERT(append_image_block(&document, "chapter2.xhtml", "chapter2.xhtml#image", 8, 4, false, &error));
    TEST_ASSERT(append_section(&document, "chapter1.xhtml", 0U, 1U, &error));
    TEST_ASSERT(append_section(&document, "chapter2.xhtml", 1U, 2U, &error));
    MereaderTuiLayout layout = {0};
    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 7, MEREADER_TUI_JUSTIFY_LEFT, &error));
    size_t chapter2_first = layout.block_first_line[1];
    size_t deep = mereader_tui_layout_target_line(&layout, "chapter2.xhtml#deep");
    TEST_ASSERT(deep != SIZE_MAX && deep > chapter2_first);
    TEST_ASSERT_SIZE(mereader_tui_layout_target_line(&layout, "chapter1.xhtml"), layout.block_first_line[0]);
    TEST_ASSERT_SIZE(mereader_tui_layout_target_line(&layout, "chapter2.xhtml#missing"), chapter2_first);
    TEST_ASSERT_SIZE(mereader_tui_layout_target_line(&layout, "chapter2.xhtml?edition=1"), chapter2_first);
    TEST_ASSERT_SIZE(mereader_tui_layout_target_line(&layout, "chapter2.xhtml#image"), layout.block_first_line[2]);
    TEST_ASSERT_SIZE(mereader_tui_layout_target_line(&layout, "missing.xhtml"), SIZE_MAX);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_progress_semantics(void) {
    TEST_ASSERT_DOUBLE(mereader_tui_layout_progress(0U, 0U), 0.0, 0.0);
    TEST_ASSERT_DOUBLE(mereader_tui_layout_progress(0U, 10U), 0.0, 0.0);
    TEST_ASSERT_DOUBLE(mereader_tui_layout_progress(5U, 10U), 0.5, 1e-15);
    TEST_ASSERT_DOUBLE(mereader_tui_layout_progress(10U, 10U), 1.0, 0.0);
    TEST_ASSERT_DOUBLE(mereader_tui_layout_progress(20U, 10U), 1.0, 0.0);
    TEST_ASSERT_SIZE(mereader_tui_layout_restore_progress(0.5, 11U), 6U);
    TEST_ASSERT_SIZE(mereader_tui_layout_restore_progress(1.0, 11U), 11U);
    TEST_ASSERT_SIZE(mereader_tui_layout_restore_progress(75.0, 11U), 11U);
    TEST_ASSERT_SIZE(mereader_tui_layout_restore_progress(-1.0, 11U), 0U);
    TEST_ASSERT_SIZE(mereader_tui_layout_restore_progress(NAN, 11U), 0U);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_image_aspect_rows_and_broken_placeholder(void) {
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#valid", 8, 4, false, &error));
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#broken", 0, 0, true, &error));
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#unprobed", 0, 0, false, &error));
    MereaderTuiLayout layout = {0};
    TEST_ASSERT_MSG(mereader_tui_layout_build(&layout, &document, 10, MEREADER_TUI_JUSTIFY_LEFT, &error), "%s", error.message);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[0]].image_rows, 3);
    TEST_ASSERT(!layout.lines[layout.block_first_line[0]].image_placeholder);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[0] + 2U].image_row, 2);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[1]].image_rows, 1);
    TEST_ASSERT(layout.lines[layout.block_first_line[1]].image_placeholder);
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[2]].image_rows, 1);
    TEST_ASSERT(layout.lines[layout.block_first_line[2]].image_placeholder);
    char *placeholder = mereader_tui_layout_line_text(&layout, layout.block_first_line[1], false, &error);
    TEST_ASSERT_STR(placeholder, "  IMAGE");
    free(placeholder);

    mereader_tui_layout_free(&layout);
    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 8, MEREADER_TUI_JUSTIFY_LEFT, &error));
    TEST_ASSERT_INT(layout.lines[layout.block_first_line[0]].image_rows, 2);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_image_row_limits_and_tall_placeholder(void) {
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    const size_t full_images = MEREADER_TUI_LAYOUT_MAX_IMAGE_EXTRA_ROWS /
                               (MEREADER_TUI_LAYOUT_MAX_IMAGE_ROWS - 1U);
    for (size_t index = 0U; index < full_images; ++index) {
        char anchor[64] = {0};
        const int length = snprintf(anchor, sizeof(anchor), "images.xhtml#boundary-%zu", index);
        TEST_ASSERT(length > 0 && (size_t)length < sizeof(anchor));
        TEST_ASSERT(append_image_block(&document, "images.xhtml", anchor, 1,
                                       MEREADER_TUI_LAYOUT_MAX_IMAGE_ROWS * 2, false, &error));
    }
    const size_t remainder = MEREADER_TUI_LAYOUT_MAX_IMAGE_EXTRA_ROWS -
                             full_images * (MEREADER_TUI_LAYOUT_MAX_IMAGE_ROWS - 1U);
    TEST_ASSERT(remainder > 0U && remainder < MEREADER_TUI_LAYOUT_MAX_IMAGE_ROWS);
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#boundary-last", 1,
                                   (int)(remainder + 1U) * 2, false, &error));

    MereaderTuiLayout layout = {0};
    TEST_ASSERT_MSG(mereader_tui_layout_build(&layout, &document, 1, MEREADER_TUI_JUSTIFY_LEFT, &error), "%s",
                     error.message);
    TEST_ASSERT_INT(layout.lines[0].image_rows, MEREADER_TUI_LAYOUT_MAX_IMAGE_ROWS);
    TEST_ASSERT_SIZE(layout.image_extra_rows, MEREADER_TUI_LAYOUT_MAX_IMAGE_EXTRA_ROWS);
    const size_t boundary_count = full_images + 1U;
    TEST_ASSERT_SIZE(layout.line_count,
                     boundary_count + MEREADER_TUI_LAYOUT_MAX_IMAGE_EXTRA_ROWS + boundary_count - 1U);

    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#over-budget", 1, 4,
                                   false, &error));
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_layout_build(&layout, &document, 1, MEREADER_TUI_JUSTIFY_LEFT, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "aggregate image") != NULL);
    TEST_ASSERT_SIZE(layout.image_extra_rows, MEREADER_TUI_LAYOUT_MAX_IMAGE_EXTRA_ROWS);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);

    document.format = MEREADER_TUI_FORMAT_IMAGE;
    TEST_ASSERT(append_image_block(&document, "images.xhtml", "images.xhtml#too-tall", 1,
                                   MEREADER_TUI_LAYOUT_MAX_IMAGE_ROWS * 2 + 1, false, &error));
    mereader_tui_error_clear(&error);
    TEST_ASSERT_MSG(mereader_tui_layout_build(&layout, &document, 1, MEREADER_TUI_JUSTIFY_LEFT, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(layout.line_count, 1U);
    TEST_ASSERT_INT(layout.lines[0].image_rows, 1);
    TEST_ASSERT_INT(layout.lines[0].image_row, 0);
    TEST_ASSERT(layout.lines[0].image_placeholder);
    char *placeholder = mereader_tui_layout_line_text(&layout, 0U, false, &error);
    TEST_ASSERT_STR(placeholder, "IMAGE");
    free(placeholder);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);

    document.format = MEREADER_TUI_FORMAT_PDF;
    TEST_ASSERT(append_image_block(&document, "pdf://page/0", "pdf://page/0", 1,
                                   MEREADER_TUI_LAYOUT_MAX_IMAGE_ROWS * 2 + 1, false, &error));
    document.blocks[0].value.image.page_index = 0;
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_layout_build_presentation(&layout, &document, 1, MEREADER_TUI_JUSTIFY_LEFT,
                                                MEREADER_TUI_PRESENTATION_FIXED, 1, 2, NULL, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "per-image row limit") != NULL);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_line_limit_and_cancellation(void) {
    char *text = malloc(MEREADER_TUI_LAYOUT_MAX_LINES + 2U);
    TEST_ASSERT(text != NULL);
    memset(text, 'x', MEREADER_TUI_LAYOUT_MAX_LINES + 1U);
    text[MEREADER_TUI_LAYOUT_MAX_LINES + 1U] = '\0';

    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(append_text_block(&document, "limit.xhtml", text, 0U, false, NULL, 0U, &error));
    free(text);
    document.blocks[0].value.text.text[MEREADER_TUI_LAYOUT_MAX_LINES] = '\0';

    MereaderTuiLayout layout = {0};
    TEST_ASSERT_MSG(mereader_tui_layout_build(&layout, &document, 1, MEREADER_TUI_JUSTIFY_LEFT, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(layout.line_count, MEREADER_TUI_LAYOUT_MAX_LINES);
    document.blocks[0].value.text.text[MEREADER_TUI_LAYOUT_MAX_LINES] = 'x';
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_layout_build(&layout, &document, 1, MEREADER_TUI_JUSTIFY_LEFT, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "line limit") != NULL);
    TEST_ASSERT_SIZE(layout.line_count, MEREADER_TUI_LAYOUT_MAX_LINES);
    mereader_tui_layout_free(&layout);

    atomic_bool cancel;
    atomic_init(&cancel, true);
    mereader_tui_error_clear(&error);
    TEST_ASSERT(!mereader_tui_layout_build_presentation(&layout, &document, 1, MEREADER_TUI_JUSTIFY_LEFT,
                                                MEREADER_TUI_PRESENTATION_DEFAULT, 1, 2, &cancel, &error));
    TEST_ASSERT(!mereader_tui_error_is_set(&error));
    TEST_ASSERT(layout.lines == NULL && layout.block_first_line == NULL);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_regex_valid_invalid_empty_and_casefold(void) {
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(append_text_block(&document, "search.xhtml", "Alpha beta ALPHA界 alpha", 0U, false, NULL, 0U,
                                  &error));
    MereaderTuiLayout layout = {0};
    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 80, MEREADER_TUI_JUSTIFY_LEFT, &error));
    MereaderTuiSearchMatch *matches = NULL;
    size_t match_count = 0U;
    TEST_ASSERT_MSG(mereader_tui_layout_search(&layout, "alpha", &matches, &match_count, &error), "%s", error.message);
    TEST_ASSERT_SIZE(match_count, 3U);
    TEST_ASSERT_SIZE(matches[0].block_offset, 0U);
    TEST_ASSERT_SIZE(matches[1].block_offset, 11U);
    TEST_ASSERT_SIZE(matches[2].block_offset, 20U);
    free(matches);
    matches = NULL;
    match_count = 0U;
    TEST_ASSERT(mereader_tui_layout_search(&layout, "界", &matches, &match_count, &error));
    TEST_ASSERT_SIZE(match_count, 1U);
    free(matches);
    matches = (MereaderTuiSearchMatch *)1;
    match_count = 99U;
    TEST_ASSERT(!mereader_tui_layout_search(&layout, "(", &matches, &match_count, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    TEST_ASSERT(matches == NULL && match_count == 0U);
    matches = (MereaderTuiSearchMatch *)1;
    match_count = 99U;
    TEST_ASSERT(!mereader_tui_layout_search(&layout, "", &matches, &match_count, &error));
    TEST_ASSERT_ERROR(error, MEREADER_TUI_ERROR_ARGUMENT);
    TEST_ASSERT(matches == NULL && match_count == 0U);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

static MereaderTuiTestResult test_resize_stable_search_identity(void) {
    MereaderTuiDocument document = {0};
    MereaderTuiError error = {0};
    TEST_ASSERT(append_text_block(&document, "resize.xhtml", "prefix needle suffix words needle end", 0U, false,
                                  NULL, 0U, &error));
    MereaderTuiLayout layout = {0};
    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 80, MEREADER_TUI_JUSTIFY_LEFT, &error));
    MereaderTuiSearchMatch *wide = NULL;
    size_t wide_count = 0U;
    TEST_ASSERT(mereader_tui_layout_search(&layout, "needle", &wide, &wide_count, &error));
    TEST_ASSERT_SIZE(wide_count, 2U);
    TEST_ASSERT_SIZE(wide[0].line, 0U);
    TEST_ASSERT_SIZE(wide[1].line, 0U);

    TEST_ASSERT(mereader_tui_layout_build(&layout, &document, 8, MEREADER_TUI_JUSTIFY_LEFT, &error));
    MereaderTuiSearchMatch *narrow = NULL;
    size_t narrow_count = 0U;
    TEST_ASSERT(mereader_tui_layout_search(&layout, "needle", &narrow, &narrow_count, &error));
    TEST_ASSERT_SIZE(narrow_count, wide_count);
    for (size_t index = 0U; index < wide_count; index++) {
        TEST_ASSERT_SIZE(narrow[index].block_index, wide[index].block_index);
        TEST_ASSERT_SIZE(narrow[index].block_offset, wide[index].block_offset);
    }
    TEST_ASSERT(narrow[1].line > narrow[0].line);
    free(wide);
    free(narrow);
    mereader_tui_layout_free(&layout);
    mereader_tui_document_close(&document);
    return MEREADER_TUI_TEST_PASS;
}

const MereaderTuiTestCase *mereader_tui_layout_test_cases(size_t *count) {
    static const MereaderTuiTestCase cases[] = {
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
    *count = MEREADER_TUI_ARRAY_LEN(cases);
    return cases;
}
