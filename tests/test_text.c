#include "test_support.h"

#include "baca/document.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_TEXT_MAX_BYTES (64U * 1024U * 1024U)

static BacaTestResult test_utf8_markdown_and_empty_documents(void) {
    static const unsigned char text[] = {
        0xefU, 0xbbU, 0xbfU, 'F', 'i', 'r', 's', 't',  '\r', '\n', 'S', 'e', 'c', 'o', 'n',
        'd',   '\r',  'T',   'h', 'i', 'r', 'd', '\n', '\n', 'F',  'o', 'u', 'r', 't', 'h',
    };
    TEST_ASSERT(baca_test_write("text/sample.txt", text, sizeof(text)));
    char *path = baca_test_path("text/sample.txt");
    TEST_ASSERT(path != NULL);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_TEXT);
    TEST_ASSERT_STR(document.metadata.title, "sample.txt");
    TEST_ASSERT_STR(document.metadata.format, "Plain Text");
    TEST_ASSERT_SIZE(document.block_count, 1U);
    TEST_ASSERT_SIZE(document.section_count, 1U);
    TEST_ASSERT_STR(document.blocks[0].value.text.text, "First\nSecond\nThird\n\nFourth");
    TEST_ASSERT(!document.blocks[0].value.text.preformatted);
    baca_document_close(&document);
    free(path);

    TEST_ASSERT(baca_test_write_text("text/readme.md", "# Heading\n\n**literal markdown**\n"));
    path = baca_test_path("text/readme.md");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_STR(document.metadata.format, "Markdown");
    TEST_ASSERT_STR(document.blocks[0].value.text.text, "# Heading\n\n**literal markdown**\n");
    baca_document_close(&document);
    free(path);

    TEST_ASSERT(baca_test_write("text/empty.txt", NULL, 0U));
    path = baca_test_path("text/empty.txt");
    TEST_ASSERT(path != NULL);
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.block_count, 1U);
    TEST_ASSERT_STR(document.blocks[0].value.text.text, "");
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_utf16_byte_orders(void) {
    static const unsigned char little_endian[] = {
        0xffU, 0xfeU, 0x48U, 0x00U, 0xe9U, 0x00U, 0x0dU, 0x00U, 0x0aU, 0x00U, 0x16U, 0x4eU, 0x4cU, 0x75U,
    };
    static const unsigned char big_endian[] = {
        0xfeU, 0xffU, 0x00U, 0x48U, 0x00U, 0xe9U, 0x00U, 0x0aU, 0x4eU, 0x16U, 0x75U, 0x4cU,
    };
    static const char expected[] = "H\xc3\xa9\n\xe4\xb8\x96\xe7\x95\x8c";
    const struct {
        const char *name;
        const unsigned char *data;
        size_t length;
    } fixtures[] = {
        {.name = "text/little.txt", .data = little_endian, .length = sizeof(little_endian)},
        {.name = "text/big.txt", .data = big_endian, .length = sizeof(big_endian)},
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(fixtures); ++index) {
        TEST_ASSERT(baca_test_write(fixtures[index].name, fixtures[index].data, fixtures[index].length));
        char *path = baca_test_path(fixtures[index].name);
        TEST_ASSERT(path != NULL);
        BacaDocument document = {0};
        BacaError error = {0};
        TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
        TEST_ASSERT_STR(document.blocks[0].value.text.text, expected);
        baca_document_close(&document);
        free(path);
    }
    return BACA_TEST_PASS;
}

static BacaTestResult test_invalid_binary_and_oversized_text(void) {
    static const unsigned char invalid_utf8[] = {0xc3U, 0x28U};
    static const unsigned char embedded_nul[] = {'a', 0x00U, 'b'};
    static const unsigned char odd_utf16[] = {0xffU, 0xfeU, 0x41U};
    const struct {
        const char *name;
        const unsigned char *data;
        size_t length;
    } fixtures[] = {
        {.name = "text/invalid.txt", .data = invalid_utf8, .length = sizeof(invalid_utf8)},
        {.name = "text/nul.txt", .data = embedded_nul, .length = sizeof(embedded_nul)},
        {.name = "text/odd.txt", .data = odd_utf16, .length = sizeof(odd_utf16)},
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(fixtures); ++index) {
        TEST_ASSERT(baca_test_write(fixtures[index].name, fixtures[index].data, fixtures[index].length));
        char *path = baca_test_path(fixtures[index].name);
        TEST_ASSERT(path != NULL);
        BacaDocument document = {0};
        BacaError error = {0};
        TEST_ASSERT(!baca_document_open(&document, path, &error));
        TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
        free(path);
    }

    char *path = baca_test_path("text/oversized.txt");
    TEST_ASSERT(path != NULL);
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    TEST_ASSERT(descriptor >= 0);
    TEST_ASSERT(ftruncate(descriptor, (off_t)TEST_TEXT_MAX_BYTES + 1) == 0);
    TEST_ASSERT(close(descriptor) == 0);
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    free(path);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_text_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "utf8_markdown_and_empty_documents", .function = test_utf8_markdown_and_empty_documents},
        {.name = "utf16_byte_orders", .function = test_utf16_byte_orders},
        {.name = "invalid_binary_and_oversized_text", .function = test_invalid_binary_and_oversized_text},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
