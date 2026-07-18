#include "test_support.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    if (!baca_test_support_init()) {
        return EXIT_FAILURE;
    }

    size_t common_count = 0U;
    size_t config_count = 0U;
    size_t database_count = 0U;
    size_t document_count = 0U;
    size_t graphics_count = 0U;
    size_t layout_count = 0U;
    const BacaTestCase *common_cases = baca_common_test_cases(&common_count);
    const BacaTestCase *config_cases = baca_config_test_cases(&config_count);
    const BacaTestCase *database_cases = baca_database_test_cases(&database_count);
    const BacaTestCase *document_cases = baca_document_test_cases(&document_count);
    const BacaTestCase *graphics_cases = baca_graphics_test_cases(&graphics_count);
    const BacaTestCase *layout_cases = baca_layout_test_cases(&layout_count);
    const BacaTestSuite suites[] = {
        {.name = "common", .cases = common_cases, .count = common_count},
        {.name = "config", .cases = config_cases, .count = config_count},
        {.name = "database", .cases = database_cases, .count = database_count},
        {.name = "document", .cases = document_cases, .count = document_count},
        {.name = "graphics", .cases = graphics_cases, .count = graphics_count},
        {.name = "layout", .cases = layout_cases, .count = layout_count},
    };
    int result = baca_test_run(suites, BACA_ARRAY_LEN(suites));
    baca_test_support_cleanup();
    return result;
}
