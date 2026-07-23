#include "test_support.h"

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    if (getenv("MEREADER_TUI_IMAGE_PTY_CHILD") != NULL) {
        const int result = mereader_tui_image_pty_child();
        cairo_debug_reset_static_data();
        return result;
    }
    if (getenv("MEREADER_TUI_PDF_PTY_CHILD") != NULL) {
        const int result = mereader_tui_pdf_pty_child();
        cairo_debug_reset_static_data();
        return result;
    }
    if (!mereader_tui_test_support_init()) {
        return EXIT_FAILURE;
    }

    size_t common_count = 0U;
    size_t catalog_count = 0U;
    size_t comic_count = 0U;
    size_t config_count = 0U;
    size_t database_count = 0U;
    size_t document_count = 0U;
    size_t fb2_count = 0U;
    size_t graphics_count = 0U;
    size_t library_count = 0U;
    size_t library_shelf_count = 0U;
    size_t layout_count = 0U;
    size_t pdf_count = 0U;
    size_t remote_count = 0U;
    size_t search_count = 0U;
    size_t text_count = 0U;
    const MereaderTuiTestCase *common_cases = mereader_tui_common_test_cases(&common_count);
    const MereaderTuiTestCase *catalog_cases = mereader_tui_catalog_test_cases(&catalog_count);
    const MereaderTuiTestCase *comic_cases = mereader_tui_comic_test_cases(&comic_count);
    const MereaderTuiTestCase *config_cases = mereader_tui_config_test_cases(&config_count);
    const MereaderTuiTestCase *database_cases = mereader_tui_database_test_cases(&database_count);
    const MereaderTuiTestCase *document_cases = mereader_tui_document_test_cases(&document_count);
    const MereaderTuiTestCase *fb2_cases = mereader_tui_fb2_test_cases(&fb2_count);
    const MereaderTuiTestCase *graphics_cases = mereader_tui_graphics_test_cases(&graphics_count);
    const MereaderTuiTestCase *library_cases = mereader_tui_library_test_cases(&library_count);
    const MereaderTuiTestCase *library_shelf_cases = mereader_tui_library_shelf_test_cases(&library_shelf_count);
    const MereaderTuiTestCase *layout_cases = mereader_tui_layout_test_cases(&layout_count);
    const MereaderTuiTestCase *pdf_cases = mereader_tui_pdf_test_cases(&pdf_count);
    const MereaderTuiTestCase *remote_cases = mereader_tui_remote_test_cases(&remote_count);
    const MereaderTuiTestCase *search_cases = mereader_tui_search_test_cases(&search_count);
    const MereaderTuiTestCase *text_cases = mereader_tui_text_test_cases(&text_count);
    const MereaderTuiTestSuite suites[] = {
        {.name = "common", .cases = common_cases, .count = common_count},
        {.name = "catalog", .cases = catalog_cases, .count = catalog_count},
        {.name = "comic", .cases = comic_cases, .count = comic_count},
        {.name = "config", .cases = config_cases, .count = config_count},
        {.name = "database", .cases = database_cases, .count = database_count},
        {.name = "document", .cases = document_cases, .count = document_count},
        {.name = "fb2", .cases = fb2_cases, .count = fb2_count},
        {.name = "graphics", .cases = graphics_cases, .count = graphics_count},
        {.name = "library", .cases = library_cases, .count = library_count},
        {.name = "library_shelf", .cases = library_shelf_cases, .count = library_shelf_count},
        {.name = "layout", .cases = layout_cases, .count = layout_count},
        {.name = "pdf", .cases = pdf_cases, .count = pdf_count},
        {.name = "remote", .cases = remote_cases, .count = remote_count},
        {.name = "search", .cases = search_cases, .count = search_count},
        {.name = "text", .cases = text_cases, .count = text_count},
    };
    int result = mereader_tui_test_run(suites, MEREADER_TUI_ARRAY_LEN(suites));
    mereader_tui_test_support_cleanup();
    cairo_debug_reset_static_data();
    FcFini();
    return result;
}
