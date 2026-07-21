#include "baca/document_backend.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <glib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <stdlib.h>
#include <string.h>

#define BACA_TEXT_MAX_BYTES (64U * 1024U * 1024U)
#define BACA_TEXT_SECTION_ID "text://document"

static char *text_convert_utf16(const unsigned char *data, size_t length, const char *encoding, size_t *output_length,
                                BacaError *error) {
    if ((length % 2U) != 0U) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "UTF-16 text has an incomplete code unit");
        return NULL;
    }
    GError *conversion_error = NULL;
    gsize bytes_read = 0U;
    gsize bytes_written = 0U;
    gchar *converted = g_convert((const gchar *)data, (gssize)length, "UTF-8", encoding, &bytes_read, &bytes_written,
                                 &conversion_error);
    if (converted == NULL || bytes_read != length || bytes_written > BACA_TEXT_MAX_BYTES) {
        const char *detail = conversion_error == NULL ? "invalid UTF-16 data" : conversion_error->message;
        baca_error_set(error, BACA_ERROR_CORRUPT, "could not decode text: %s", detail);
        g_clear_error(&conversion_error);
        g_free(converted);
        return NULL;
    }
    char *result = baca_strndup(converted, bytes_written, error);
    g_free(converted);
    if (result != NULL) {
        *output_length = bytes_written;
    }
    return result;
}

static char *text_decode(const BacaBuffer *snapshot, size_t *length, BacaError *error) {
    const unsigned char *data = snapshot->data;
    size_t input_length = snapshot->length;
    char *decoded = NULL;
    if (input_length >= 2U && data[0] == 0xffU && data[1] == 0xfeU) {
        decoded = text_convert_utf16(data + 2U, input_length - 2U, "UTF-16LE", length, error);
    } else if (input_length >= 2U && data[0] == 0xfeU && data[1] == 0xffU) {
        decoded = text_convert_utf16(data + 2U, input_length - 2U, "UTF-16BE", length, error);
    } else {
        size_t offset = input_length >= 3U && data[0] == 0xefU && data[1] == 0xbbU && data[2] == 0xbfU ? 3U : 0U;
        const size_t utf8_length = input_length - offset;
        if (memchr(data + offset, '\0', utf8_length) != NULL ||
            !g_utf8_validate((const gchar *)data + offset, (gssize)utf8_length, NULL)) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "plain text is not valid UTF-8 or BOM-marked UTF-16");
            return NULL;
        }
        decoded = baca_strndup((const char *)data + offset, utf8_length, error);
        if (decoded != NULL) {
            *length = utf8_length;
        }
    }
    if (decoded == NULL) {
        return NULL;
    }
    if (memchr(decoded, '\0', *length) != NULL) {
        free(decoded);
        baca_error_set(error, BACA_ERROR_CORRUPT, "plain text contains an embedded null character");
        return NULL;
    }
    return decoded;
}

static void text_normalize_newlines(char *text, size_t *length) {
    size_t read_offset = 0U;
    size_t write_offset = 0U;
    while (read_offset < *length) {
        if (text[read_offset] == '\r') {
            text[write_offset++] = '\n';
            ++read_offset;
            if (read_offset < *length && text[read_offset] == '\n') {
                ++read_offset;
            }
        } else {
            text[write_offset++] = text[read_offset++];
        }
    }
    text[write_offset] = '\0';
    *length = write_offset;
}

bool baca_text_open(BacaDocument *document, const char *path, const struct stat *expected_identity, BacaError *error) {
    if (document == NULL || path == NULL || path[0] == '\0' || document->path == NULL ||
        strcmp(path, document->path) != 0 || expected_identity == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document, canonical text path, and detected identity are required");
        return false;
    }
    BacaBuffer snapshot = {0};
    if (!baca_document_read_snapshot(path, expected_identity, BACA_TEXT_MAX_BYTES, &snapshot, error)) {
        return false;
    }
    size_t text_length = 0U;
    char *text = text_decode(&snapshot, &text_length, error);
    baca_buffer_free(&snapshot);
    if (text == NULL) {
        return false;
    }
    text_normalize_newlines(text, &text_length);

    document->format = BACA_FORMAT_TEXT;
    document->metadata.title = baca_path_basename(path, error);
    const char *extension = baca_path_extension(path);
    document->metadata.format =
        baca_strdup(extension != NULL && baca_casecmp(extension, ".md") == 0 ? "Markdown" : "Plain Text", error);
    if (document->metadata.title == NULL || document->metadata.format == NULL ||
        !baca_document_account_metadata(document, error)) {
        free(text);
        return false;
    }
    BacaBlock block = {
        .kind = BACA_BLOCK_TEXT,
        .section_id = baca_strdup(BACA_TEXT_SECTION_ID, error),
        .value.text =
            {
                .text = text,
            },
    };
    if (block.section_id == NULL || !baca_document_add_text_block(document, &block, error)) {
        baca_document_block_free(&block);
        return false;
    }
    BacaSection section = {
        .id = baca_strdup(BACA_TEXT_SECTION_ID, error),
        .first_block = 0U,
        .block_count = 1U,
        .source_size = text_length,
        .linear = true,
    };
    if (section.id == NULL || !baca_document_add_section(document, &section, error)) {
        free(section.id);
        return false;
    }
    return true;
}
