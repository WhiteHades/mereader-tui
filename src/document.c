#include "baca/document_backend.h"
#include "baca/graphics.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zip.h>

#define BACA_DATA_URI_MAX (64U * 1024U * 1024U)

static void metadata_free(BacaMetadata *metadata) {
    free(metadata->title);
    free(metadata->creator);
    free(metadata->description);
    free(metadata->publisher);
    free(metadata->date);
    free(metadata->language);
    free(metadata->format);
    free(metadata->identifier);
    free(metadata->source);
    memset(metadata, 0, sizeof(*metadata));
}

static void text_block_free(BacaTextBlock *text) {
    free(text->text);
    for (size_t index = 0; index < text->span_count; index++) {
        free(text->spans[index].link);
    }
    free(text->spans);
    for (size_t index = 0; index < text->anchor_count; index++) {
        free(text->anchors[index].target);
    }
    free(text->anchors);
    memset(text, 0, sizeof(*text));
}

static void block_free(BacaBlock *block) {
    free(block->section_id);
    if (block->kind == BACA_BLOCK_TEXT) {
        text_block_free(&block->value.text);
    } else if (block->kind == BACA_BLOCK_IMAGE) {
        free(block->value.image.uri);
        free(block->value.image.alt);
        free(block->value.image.anchor);
        free(block->value.image.link);
    }
    memset(block, 0, sizeof(*block));
}

static bool retained_add(size_t *total, size_t amount) {
    if (amount > SIZE_MAX - *total) {
        return false;
    }
    *total += amount;
    return true;
}

static bool retained_add_string(size_t *total, const char *value) {
    return value == NULL || (strlen(value) < SIZE_MAX && retained_add(total, strlen(value) + 1U));
}

static bool text_block_retained_size(const BacaTextBlock *text, size_t *size, BacaError *error) {
    if ((text->span_count > 0 && text->spans == NULL) ||
        (text->anchor_count > 0 && text->anchors == NULL)) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "text block metadata arrays are missing");
        return false;
    }
    if (text->span_count > BACA_DOCUMENT_MAX_SPANS_PER_BLOCK ||
        text->anchor_count > BACA_DOCUMENT_MAX_ANCHORS_PER_BLOCK) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "text block metadata exceeds the supported count limit");
        return false;
    }
    if (!retained_add_string(size, text->text) ||
        text->span_count > SIZE_MAX / sizeof(*text->spans) ||
        !retained_add(size, text->span_count * sizeof(*text->spans)) ||
        text->anchor_count > SIZE_MAX / sizeof(*text->anchors) ||
        !retained_add(size, text->anchor_count * sizeof(*text->anchors))) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "text block retained size overflow");
        return false;
    }
    for (size_t index = 0; index < text->span_count; index++) {
        if (!retained_add_string(size, text->spans[index].link)) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "text span retained size overflow");
            return false;
        }
    }
    for (size_t index = 0; index < text->anchor_count; index++) {
        if (!retained_add_string(size, text->anchors[index].target)) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "text anchor retained size overflow");
            return false;
        }
    }
    return true;
}

static bool block_retained_size(const BacaBlock *block, size_t *size, BacaError *error) {
    size_t total = sizeof(*block);
    if (!retained_add_string(&total, block->section_id)) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "block retained size overflow");
        return false;
    }
    if (block->kind == BACA_BLOCK_TEXT) {
        if (!text_block_retained_size(&block->value.text, &total, error)) {
            return false;
        }
    } else if (block->kind == BACA_BLOCK_IMAGE) {
        if (!retained_add_string(&total, block->value.image.uri) ||
            !retained_add_string(&total, block->value.image.alt) ||
            !retained_add_string(&total, block->value.image.anchor) ||
            !retained_add_string(&total, block->value.image.link)) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "image block retained size overflow");
            return false;
        }
    }
    *size = total;
    return true;
}

static bool document_can_retain(const BacaDocument *document, size_t amount, BacaError *error) {
    if (document->retained_bytes > BACA_DOCUMENT_MAX_RETAINED_BYTES ||
        amount > BACA_DOCUMENT_MAX_RETAINED_BYTES - document->retained_bytes) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "document retained data exceeds the supported size limit");
        return false;
    }
    return true;
}

static bool add_block(BacaDocument *document, BacaBlock *block, BacaBlockKind expected, BacaError *error) {
    if (document == NULL || block == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document and block are required");
        return false;
    }
    if (block->kind != expected) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "block kind does not match add helper");
        return false;
    }
    if (document->block_count >= BACA_DOCUMENT_MAX_BLOCKS) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "document contains too many blocks");
        return false;
    }
    size_t retained_size = 0;
    if (!block_retained_size(block, &retained_size, error) ||
        !document_can_retain(document, retained_size, error)) {
        return false;
    }
    BacaError reserve_error = {0};
    BacaBlock *blocks = baca_array_reserve(document->blocks, &document->block_capacity, sizeof(*document->blocks),
                                           document->block_count + 1, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (error != NULL) {
            *error = reserve_error;
        }
        return false;
    }
    document->blocks = blocks;
    document->blocks[document->block_count++] = *block;
    document->retained_bytes += retained_size;
    memset(block, 0, sizeof(*block));
    return true;
}

bool baca_document_add_text_block(BacaDocument *document, BacaBlock *block, BacaError *error) {
    return add_block(document, block, BACA_BLOCK_TEXT, error);
}

bool baca_document_add_image_block(BacaDocument *document, BacaBlock *block, BacaError *error) {
    return add_block(document, block, BACA_BLOCK_IMAGE, error);
}

bool baca_document_add_toc(BacaDocument *document, const char *label, const char *target, unsigned depth,
                           BacaError *error) {
    if (document == NULL || label == NULL || target == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document, TOC label, and TOC target are required");
        return false;
    }
    if (document->toc_count >= BACA_DOCUMENT_MAX_TOC_ENTRIES) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "document contains too many TOC entries");
        return false;
    }
    size_t retained_size = sizeof(*document->toc);
    if (!retained_add_string(&retained_size, label) || !retained_add_string(&retained_size, target)) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "TOC entry retained size overflow");
        return false;
    }
    if (!document_can_retain(document, retained_size, error)) {
        return false;
    }

    char *owned_label = baca_strdup(label, error);
    if (owned_label == NULL) {
        return false;
    }
    char *owned_target = baca_strdup(target, error);
    if (owned_target == NULL) {
        free(owned_label);
        return false;
    }
    BacaError reserve_error = {0};
    BacaTocEntry *toc = baca_array_reserve(document->toc, &document->toc_capacity, sizeof(*document->toc),
                                           document->toc_count + 1, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (error != NULL) {
            *error = reserve_error;
        }
        free(owned_label);
        free(owned_target);
        return false;
    }
    document->toc = toc;

    document->toc[document->toc_count++] = (BacaTocEntry) {
        .label = owned_label,
        .target = owned_target,
        .depth = depth,
        .section_index = SIZE_MAX,
    };
    document->retained_bytes += retained_size;
    return true;
}

bool baca_document_add_section(BacaDocument *document, BacaSection *section, BacaError *error) {
    if (document == NULL || section == NULL || section->id == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document and section id are required");
        return false;
    }
    if (document->section_count >= BACA_DOCUMENT_MAX_SECTIONS) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "document contains too many sections");
        return false;
    }
    size_t retained_size = sizeof(*document->sections);
    if (!retained_add_string(&retained_size, section->id)) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "section retained size overflow");
        return false;
    }
    if (!document_can_retain(document, retained_size, error)) {
        return false;
    }
    BacaError reserve_error = {0};
    BacaSection *sections =
        baca_array_reserve(document->sections, &document->section_capacity, sizeof(*document->sections),
                           document->section_count + 1, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (error != NULL) {
            *error = reserve_error;
        }
        return false;
    }
    document->sections = sections;
    document->sections[document->section_count++] = *section;
    document->retained_bytes += retained_size;
    memset(section, 0, sizeof(*section));
    return true;
}

bool baca_document_account_metadata(BacaDocument *document, BacaError *error) {
    if (document == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document is required");
        return false;
    }
    const char *const values[] = {
        document->metadata.title,       document->metadata.creator,  document->metadata.description,
        document->metadata.publisher,   document->metadata.date,     document->metadata.language,
        document->metadata.format,      document->metadata.identifier,
        document->metadata.source,
    };
    size_t retained_size = 0;
    for (size_t index = 0; index < BACA_ARRAY_LEN(values); index++) {
        if (!retained_add_string(&retained_size, values[index])) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "metadata retained size overflow");
            return false;
        }
    }
    if (!document_can_retain(document, retained_size, error)) {
        return false;
    }
    document->retained_bytes += retained_size;
    return true;
}

void baca_document_rollback_blocks(BacaDocument *document, size_t first_block) {
    if (document == NULL) {
        return;
    }
    while (document->block_count > first_block) {
        document->block_count--;
        size_t retained_size = 0;
        BacaError ignored = {0};
        if (block_retained_size(&document->blocks[document->block_count], &retained_size, &ignored) &&
            retained_size <= document->retained_bytes) {
            document->retained_bytes -= retained_size;
        }
        block_free(&document->blocks[document->block_count]);
    }
}

void baca_document_rollback_toc(BacaDocument *document, size_t first_entry) {
    if (document == NULL) {
        return;
    }
    while (document->toc_count > first_entry) {
        document->toc_count--;
        BacaTocEntry *entry = &document->toc[document->toc_count];
        size_t retained_size = sizeof(*entry);
        bool measured = retained_add_string(&retained_size, entry->label) &&
                        retained_add_string(&retained_size, entry->target);
        if (measured && retained_size <= document->retained_bytes) {
            document->retained_bytes -= retained_size;
        }
        free(entry->label);
        free(entry->target);
        memset(entry, 0, sizeof(*entry));
    }
}

void baca_resource_free(BacaResource *resource) {
    if (resource == NULL) {
        return;
    }
    free(resource->data);
    free(resource->mime_type);
    memset(resource, 0, sizeof(*resource));
}

static int base64_value(unsigned char value) {
    if (value >= 'A' && value <= 'Z') {
        return (int) (value - 'A');
    }
    if (value >= 'a' && value <= 'z') {
        return (int) (value - 'a') + 26;
    }
    if (value >= '0' && value <= '9') {
        return (int) (value - '0') + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

static bool decode_base64(const char *input, size_t input_length, unsigned char **output, size_t *output_length,
                          BacaError *error) {
    if (input_length > BACA_DATA_URI_MAX * 2U) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "data URI exceeds the supported size limit");
        return false;
    }
    size_t capacity = (input_length / 4U + 1U) * 3U;
    if (capacity > BACA_DATA_URI_MAX) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "decoded data URI exceeds the supported size limit");
        return false;
    }
    unsigned char *decoded = malloc(capacity == 0 ? 1 : capacity);
    if (decoded == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate data URI payload");
        return false;
    }

    size_t written = 0;
    int quartet[4] = {0};
    size_t quartet_length = 0;
    bool finished = false;
    for (size_t index = 0; index < input_length; index++) {
        unsigned char value = (unsigned char) input[index];
        if (isspace(value) != 0) {
            continue;
        }
        int digit = value == '=' ? -2 : base64_value(value);
        if (digit < -1 || finished) {
            if (digit == -2 && !finished) {
                quartet[quartet_length++] = digit;
            } else {
                free(decoded);
                baca_error_set(error, BACA_ERROR_CORRUPT, "invalid base64 data URI");
                return false;
            }
        } else if (digit < 0) {
            free(decoded);
            baca_error_set(error, BACA_ERROR_CORRUPT, "invalid base64 data URI");
            return false;
        } else {
            quartet[quartet_length++] = digit;
        }
        if (quartet_length != 4) {
            continue;
        }
        if (quartet[0] < 0 || quartet[1] < 0 || (quartet[2] == -2 && quartet[3] != -2) ||
            quartet[2] < -2 || quartet[3] < -2) {
            free(decoded);
            baca_error_set(error, BACA_ERROR_CORRUPT, "invalid base64 data URI padding");
            return false;
        }
        decoded[written++] = (unsigned char) (((unsigned) quartet[0] << 2U) |
                                               ((unsigned) quartet[1] >> 4U));
        if (quartet[2] >= 0) {
            decoded[written++] = (unsigned char) ((((unsigned) quartet[1] & 0x0fU) << 4U) |
                                                   ((unsigned) quartet[2] >> 2U));
            if (quartet[3] >= 0) {
                decoded[written++] = (unsigned char) ((((unsigned) quartet[2] & 0x03U) << 6U) |
                                                       (unsigned) quartet[3]);
            }
        }
        finished = quartet[2] == -2 || quartet[3] == -2;
        quartet_length = 0;
    }
    if (quartet_length == 1 || (finished && quartet_length != 0)) {
        free(decoded);
        baca_error_set(error, BACA_ERROR_CORRUPT, "truncated base64 data URI");
        return false;
    }
    if (quartet_length == 2 || quartet_length == 3) {
        if (quartet[0] < 0 || quartet[1] < 0 || (quartet_length == 3 && quartet[2] < 0)) {
            free(decoded);
            baca_error_set(error, BACA_ERROR_CORRUPT, "invalid base64 data URI padding");
            return false;
        }
        decoded[written++] = (unsigned char) (((unsigned) quartet[0] << 2U) |
                                               ((unsigned) quartet[1] >> 4U));
        if (quartet_length == 3) {
            decoded[written++] = (unsigned char) ((((unsigned) quartet[1] & 0x0fU) << 4U) |
                                                   ((unsigned) quartet[2] >> 2U));
        }
    }

    *output = decoded;
    *output_length = written;
    return true;
}

static int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

#define BACA_DOCUMENT_URI_MAX (1024U * 1024U)

typedef struct UriParts {
    const char *path;
    size_t path_length;
    const char *query;
    size_t query_length;
    const char *fragment;
    size_t fragment_length;
    bool has_query;
    bool has_fragment;
} UriParts;

typedef enum UriComponent {
    URI_COMPONENT_PATH,
    URI_COMPONENT_QUERY,
    URI_COMPONENT_FRAGMENT,
} UriComponent;

static UriParts split_uri(const char *value) {
    size_t length = strlen(value);
    const char *fragment_marker = memchr(value, '#', length);
    size_t before_fragment = fragment_marker == NULL ? length : (size_t) (fragment_marker - value);
    const char *query_marker = memchr(value, '?', before_fragment);
    size_t path_length = query_marker == NULL ? before_fragment : (size_t) (query_marker - value);
    return (UriParts) {
        .path = value,
        .path_length = path_length,
        .query = query_marker == NULL ? NULL : query_marker + 1,
        .query_length = query_marker == NULL ? 0 : before_fragment - (size_t) (query_marker + 1 - value),
        .fragment = fragment_marker == NULL ? NULL : fragment_marker + 1,
        .fragment_length = fragment_marker == NULL ? 0 : length - (size_t) (fragment_marker + 1 - value),
        .has_query = query_marker != NULL,
        .has_fragment = fragment_marker != NULL,
    };
}

static char *decode_uri_component(const char *value, size_t length, BacaError *error) {
    char *decoded = malloc(length + 1U);
    if (decoded == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate document URI");
        return NULL;
    }
    size_t output = 0;
    for (size_t input = 0; input < length; input++) {
        unsigned char byte = (unsigned char) value[input];
        if (byte == '%') {
            if (input + 2U >= length) {
                free(decoded);
                baca_error_set(error, BACA_ERROR_CORRUPT, "incomplete percent escape in document URI");
                return NULL;
            }
            int high = hex_value(value[input + 1U]);
            int low = hex_value(value[input + 2U]);
            if (high < 0 || low < 0) {
                free(decoded);
                baca_error_set(error, BACA_ERROR_CORRUPT, "invalid percent escape in document URI");
                return NULL;
            }
            byte = (unsigned char) ((unsigned) high * 16U + (unsigned) low);
            input += 2U;
        }
        if (byte == '\0') {
            free(decoded);
            baca_error_set(error, BACA_ERROR_CORRUPT, "document URI contains an encoded NUL byte");
            return NULL;
        }
        decoded[output++] = (char) byte;
    }
    decoded[output] = '\0';
    return decoded;
}

static bool uri_unreserved(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' || value == '~';
}

static bool uri_pchar(unsigned char value) {
    return uri_unreserved(value) || value == '!' || value == '$' || value == '&' || value == '\'' ||
           value == '(' || value == ')' || value == '*' || value == '+' || value == ',' || value == ';' ||
           value == '=' || value == ':' || value == '@';
}

static bool append_canonical_component(BacaString *output, const char *value, size_t length,
                                       UriComponent component, BacaError *error) {
    static const char hexadecimal[] = "0123456789ABCDEF";
    for (size_t index = 0; index < length; index++) {
        unsigned char byte = (unsigned char) value[index];
        bool literal = uri_pchar(byte) ||
                       (component != URI_COMPONENT_PATH && (byte == '/' || byte == '?'));
        if (literal) {
            if (!baca_string_append_char(output, (char) byte, error)) {
                return false;
            }
        } else {
            char encoded[3] = {'%', hexadecimal[byte >> 4U], hexadecimal[byte & 0x0fU]};
            if (!baca_string_append_n(output, encoded, sizeof(encoded), error)) {
                return false;
            }
        }
    }
    return true;
}

static bool normalize_uri_path(const char *path, BacaString *output, BacaError *error) {
    size_t *segments = NULL;
    size_t segment_count = 0;
    size_t segment_capacity = 0;
    size_t length = strlen(path);
    size_t cursor = 0;
    bool success = true;
    while (cursor < length && success) {
        while (cursor < length && path[cursor] == '/') {
            cursor++;
        }
        size_t start = cursor;
        while (cursor < length && path[cursor] != '/') {
            unsigned char byte = (unsigned char) path[cursor];
            if (byte == '\\' || byte < 0x20U || byte == 0x7fU) {
                baca_error_set(error, BACA_ERROR_CORRUPT, "document URI contains an unsafe path byte");
                success = false;
                break;
            }
            cursor++;
        }
        if (!success) {
            break;
        }
        size_t segment_length = cursor - start;
        if (segment_length == 0 || (segment_length == 1 && path[start] == '.')) {
            continue;
        }
        if (segment_length == 2 && path[start] == '.' && path[start + 1U] == '.') {
            if (segment_count == 0) {
                baca_error_set(error, BACA_ERROR_CORRUPT, "relative URI escapes the document root");
                success = false;
                break;
            }
            output->length = segments[--segment_count];
            output->data[output->length] = '\0';
            continue;
        }

        BacaError reserve_error = {0};
        size_t *resized = baca_array_reserve(segments, &segment_capacity, sizeof(*segments),
                                             segment_count + 1U, &reserve_error);
        if (baca_error_is_set(&reserve_error)) {
            if (error != NULL) {
                *error = reserve_error;
            }
            success = false;
            break;
        }
        segments = resized;
        size_t rollback = output->length;
        if ((output->length > 0 && !baca_string_append_char(output, '/', error)) ||
            !append_canonical_component(output, path + start, segment_length, URI_COMPONENT_PATH, error)) {
            success = false;
            break;
        }
        segments[segment_count++] = rollback;
    }
    free(segments);
    return success;
}

static bool append_decoded_component(BacaString *output, char marker, const char *value, size_t length,
                                     UriComponent component, BacaError *error) {
    char *decoded = decode_uri_component(value, length, error);
    if (decoded == NULL) {
        return false;
    }
    bool success = baca_string_append_char(output, marker, error) &&
                   append_canonical_component(output, decoded, strlen(decoded), component, error);
    free(decoded);
    return success;
}

char *baca_document_resolve_uri(const char *base, const char *reference, bool allow_external,
                                BacaError *error) {
    if (base == NULL || reference == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "cannot resolve a null document URI");
        return NULL;
    }
    if (baca_is_external_uri(reference) || strncmp(reference, "//", 2) == 0) {
        if (!allow_external) {
            baca_error_set(error, BACA_ERROR_CORRUPT,
                           "external URI used where a document member was required: %s", reference);
            return NULL;
        }
        return baca_strdup(reference, error);
    }
    if (strlen(base) > BACA_DOCUMENT_URI_MAX || strlen(reference) > BACA_DOCUMENT_URI_MAX) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "document URI exceeds the supported size limit");
        return NULL;
    }

    UriParts base_parts = split_uri(base);
    UriParts reference_parts = split_uri(reference);
    char *base_path = decode_uri_component(base_parts.path, base_parts.path_length, error);
    char *reference_path = decode_uri_component(reference_parts.path, reference_parts.path_length, error);
    if (base_path == NULL || reference_path == NULL) {
        free(base_path);
        free(reference_path);
        return NULL;
    }

    BacaString merged = {0};
    bool success = true;
    if (reference_parts.path_length == 0) {
        success = baca_string_append(&merged, base_path, error);
    } else if (reference_path[0] == '/') {
        success = baca_string_append(&merged, reference_path, error);
    } else {
        const char *slash = strrchr(base_path, '/');
        if (slash != NULL) {
            success = baca_string_append_n(&merged, base_path, (size_t) (slash - base_path + 1), error);
        }
        if (success) {
            success = baca_string_append(&merged, reference_path, error);
        }
    }

    BacaString result = {0};
    if (success) {
        success = normalize_uri_path(merged.data == NULL ? "" : merged.data, &result, error);
    }
    if (success && reference_parts.has_query) {
        success = append_decoded_component(&result, '?', reference_parts.query, reference_parts.query_length,
                                           URI_COMPONENT_QUERY, error);
    } else if (success && reference_parts.path_length == 0 && base_parts.has_query) {
        success = append_decoded_component(&result, '?', base_parts.query, base_parts.query_length,
                                           URI_COMPONENT_QUERY, error);
    }
    if (success && reference_parts.has_fragment) {
        success = append_decoded_component(&result, '#', reference_parts.fragment,
                                           reference_parts.fragment_length, URI_COMPONENT_FRAGMENT, error);
    }

    free(base_path);
    free(reference_path);
    baca_string_free(&merged);
    if (!success) {
        baca_string_free(&result);
        return NULL;
    }
    char *resolved = baca_string_take(&result);
    return resolved == NULL ? baca_strdup("", error) : resolved;
}

char *baca_document_uri_path(const char *uri, BacaError *error) {
    if (uri == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document URI is required");
        return NULL;
    }
    if (baca_is_external_uri(uri) || strncmp(uri, "//", 2) == 0) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "external URI has no document member path");
        return NULL;
    }
    if (strlen(uri) > BACA_DOCUMENT_URI_MAX) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "document URI exceeds the supported size limit");
        return NULL;
    }
    UriParts parts = split_uri(uri);
    return decode_uri_component(parts.path, parts.path_length, error);
}

char *baca_document_fragment_target(const char *base, const char *fragment, BacaError *error) {
    if (base == NULL || fragment == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document target base and fragment are required");
        return NULL;
    }
    if (baca_is_external_uri(base) || strlen(base) > BACA_DOCUMENT_URI_MAX ||
        strlen(fragment) > BACA_DOCUMENT_URI_MAX) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "invalid document target");
        return NULL;
    }
    UriParts parts = split_uri(base);
    BacaString target = {0};
    if (!baca_string_append_n(&target, parts.path, parts.path_length, error) ||
        !baca_string_append_char(&target, '#', error) ||
        !append_canonical_component(&target, fragment, strlen(fragment), URI_COMPONENT_FRAGMENT, error)) {
        baca_string_free(&target);
        return NULL;
    }
    return baca_string_take(&target);
}

static bool ascii_case_equal_n(const char *value, size_t length, const char *expected) {
    size_t expected_length = strlen(expected);
    if (length != expected_length) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char left = (unsigned char) value[index];
        unsigned char right = (unsigned char) expected[index];
        if (left >= 'A' && left <= 'Z') {
            left = (unsigned char) (left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = (unsigned char) (right + ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

static bool decode_percent_data(const char *input, size_t input_length, unsigned char **output,
                                size_t *output_length, BacaError *error) {
    if (input_length > BACA_DATA_URI_MAX) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "data URI exceeds the supported size limit");
        return false;
    }
    unsigned char *decoded = malloc(input_length == 0 ? 1 : input_length);
    if (decoded == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate data URI payload");
        return false;
    }
    size_t written = 0;
    for (size_t index = 0; index < input_length; index++) {
        if (input[index] == '%') {
            if (index + 2 >= input_length) {
                free(decoded);
                baca_error_set(error, BACA_ERROR_CORRUPT, "truncated percent escape in data URI");
                return false;
            }
            int high = hex_value(input[index + 1]);
            int low = hex_value(input[index + 2]);
            if (high < 0 || low < 0) {
                free(decoded);
                baca_error_set(error, BACA_ERROR_CORRUPT, "invalid percent escape in data URI");
                return false;
            }
            decoded[written++] = (unsigned char) ((unsigned) high * 16U + (unsigned) low);
            index += 2;
        } else {
            decoded[written++] = (unsigned char) input[index];
        }
    }
    *output = decoded;
    *output_length = written;
    return true;
}

static bool load_data_uri(const char *uri, BacaResource *resource, BacaError *error) {
    const char *comma = strchr(uri + 5, ',');
    if (comma == NULL) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "data URI has no payload separator");
        return false;
    }
    size_t metadata_length = (size_t) (comma - (uri + 5));
    bool base64 = false;
    size_t mime_length = metadata_length;
    const char *semicolon = memchr(uri + 5, ';', metadata_length);
    if (semicolon != NULL) {
        mime_length = (size_t) (semicolon - (uri + 5));
        const char *option = semicolon + 1;
        while (option < comma) {
            const char *option_end = memchr(option, ';', (size_t) (comma - option));
            if (option_end == NULL) {
                option_end = comma;
            }
            if (ascii_case_equal_n(option, (size_t) (option_end - option), "base64")) {
                base64 = true;
                break;
            }
            option = option_end + 1;
        }
    }

    const char *payload = comma + 1;
    size_t payload_length = strlen(payload);
    unsigned char *data = NULL;
    size_t data_length = 0;
    bool decoded = base64 ? decode_base64(payload, payload_length, &data, &data_length, error)
                          : decode_percent_data(payload, payload_length, &data, &data_length, error);
    if (!decoded) {
        return false;
    }

    char *mime_type = mime_length == 0 ? baca_strdup("text/plain;charset=US-ASCII", error)
                                       : baca_strndup(uri + 5, mime_length, error);
    if (mime_type == NULL) {
        free(data);
        return false;
    }
    resource->data = data;
    resource->length = data_length;
    resource->mime_type = mime_type;
    return true;
}

static bool resource_output_is_empty(const BacaResource *resource) {
    return resource->data == NULL && resource->length == 0 && resource->mime_type == NULL;
}

bool baca_document_load_resource(BacaDocument *document, const char *uri, BacaResource *resource,
                                  BacaError *error) {
    if (error != NULL) {
        baca_error_clear(error);
    }
    if (document == NULL || uri == NULL || resource == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document, URI, and resource output are required");
        return false;
    }
    if (!resource_output_is_empty(resource)) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "resource output must be zero-initialized");
        return false;
    }

    BacaResource loaded = {0};
    bool success;
    if (strlen(uri) >= 5 && ascii_case_equal_n(uri, 5, "data:")) {
        success = load_data_uri(uri, &loaded, error);
    } else if (document->ops != NULL && document->ops->load_resource != NULL) {
        success = document->ops->load_resource(document, uri, &loaded, error);
    } else {
        baca_error_set(error, BACA_ERROR_UNSUPPORTED, "resources are not supported for this document format");
        success = false;
    }
    if (!success) {
        baca_resource_free(&loaded);
        return false;
    }
    *resource = loaded;
    return true;
}

static bool extension_is(const char *extension, const char *expected) {
    return extension != NULL && baca_casecmp(extension, expected) == 0;
}

static bool read_magic(const char *path, unsigned char *buffer, size_t capacity, size_t *length,
                       BacaError *error) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        baca_error_set(error, BACA_ERROR_IO, "could not open %s: %s", path, strerror(errno));
        return false;
    }
    size_t bytes = fread(buffer, 1, capacity, file);
    if (ferror(file) != 0) {
        baca_error_set(error, BACA_ERROR_IO, "could not read %s: %s", path, strerror(errno));
        fclose(file);
        return false;
    }
    fclose(file);
    *length = bytes;
    return true;
}

static bool has_epub_mimetype(const char *path) {
    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_RDONLY, &zip_error);
    if (archive == NULL) {
        return false;
    }
    zip_int64_t index = zip_name_locate(archive, "mimetype", ZIP_FL_ENC_GUESS);
    if (index < 0) {
        zip_close(archive);
        return false;
    }
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive, (zip_uint64_t) index, ZIP_FL_ENC_GUESS, &stat) != 0 || stat.size != 20) {
        zip_close(archive);
        return false;
    }
    zip_file_t *member = zip_fopen_index(archive, (zip_uint64_t) index, ZIP_FL_ENC_GUESS);
    if (member == NULL) {
        zip_close(archive);
        return false;
    }
    char value[20];
    zip_int64_t read_length = zip_fread(member, value, sizeof(value));
    bool matches = read_length == (zip_int64_t) sizeof(value) && memcmp(value, "application/epub+zip", 20) == 0;
    zip_fclose(member);
    zip_close(archive);
    return matches;
}

static BacaDocumentFormat extension_format(const char *extension) {
    if (extension_is(extension, ".epub") || extension_is(extension, ".epub3")) {
        return BACA_FORMAT_EPUB;
    }
    if (extension_is(extension, ".mobi") || extension_is(extension, ".prc")) {
        return BACA_FORMAT_MOBI;
    }
    if (extension_is(extension, ".azw") || extension_is(extension, ".azw3") ||
        extension_is(extension, ".azw4")) {
        return BACA_FORMAT_AZW;
    }
    if (extension_is(extension, ".pdf")) {
        return BACA_FORMAT_PDF;
    }
    if (extension_is(extension, ".cbz") || extension_is(extension, ".cbr") ||
        extension_is(extension, ".cb7")) {
        return BACA_FORMAT_COMIC;
    }
    if (extension_is(extension, ".png") || extension_is(extension, ".jpg") ||
        extension_is(extension, ".jpeg") || extension_is(extension, ".gif") ||
        extension_is(extension, ".webp") || extension_is(extension, ".bmp") ||
        extension_is(extension, ".svg")) {
        return BACA_FORMAT_IMAGE;
    }
    if (extension_is(extension, ".txt") || extension_is(extension, ".md")) {
        return BACA_FORMAT_TEXT;
    }
    if (extension_is(extension, ".fb2")) {
        return BACA_FORMAT_FB2;
    }
    return BACA_FORMAT_UNKNOWN;
}

static BacaDocumentFormat detect_format(const char *path, const char *extension, const unsigned char *magic,
                                         size_t magic_length) {
    if (magic_length >= 68 && (memcmp(magic + 60, "BOOKMOBI", 8) == 0 ||
                               memcmp(magic + 60, "TEXtREAd", 8) == 0)) {
        return extension_is(extension, ".azw") || extension_is(extension, ".azw3") ||
                       extension_is(extension, ".azw4") ?
                   BACA_FORMAT_AZW :
                   BACA_FORMAT_MOBI;
    }
    if (magic_length >= 4 && memcmp(magic, "PK\003\004", 4) == 0 && has_epub_mimetype(path)) {
        return BACA_FORMAT_EPUB;
    }
    if (magic_length >= 5 && memcmp(magic, "%PDF-", 5) == 0) {
        return BACA_FORMAT_PDF;
    }
    if ((magic_length >= 8 && memcmp(magic, "\211PNG\r\n\032\n", 8) == 0) ||
        (magic_length >= 3 && memcmp(magic, "\377\330\377", 3) == 0) ||
        (magic_length >= 6 && (memcmp(magic, "GIF87a", 6) == 0 || memcmp(magic, "GIF89a", 6) == 0))) {
        return BACA_FORMAT_IMAGE;
    }
    return extension_format(extension);
}

static bool metadata_is_empty(const BacaMetadata *metadata) {
    return metadata->title == NULL && metadata->creator == NULL && metadata->description == NULL &&
           metadata->publisher == NULL && metadata->date == NULL && metadata->language == NULL &&
           metadata->format == NULL && metadata->identifier == NULL && metadata->source == NULL;
}

static bool document_output_is_empty(const BacaDocument *document) {
    return document->path == NULL && document->format == BACA_FORMAT_UNKNOWN &&
           metadata_is_empty(&document->metadata) && document->toc == NULL && document->toc_count == 0 &&
           document->toc_capacity == 0 && document->sections == NULL && document->section_count == 0 &&
           document->section_capacity == 0 && document->blocks == NULL && document->block_count == 0 &&
           document->block_capacity == 0 && document->retained_bytes == 0 && document->backend == NULL &&
           document->ops == NULL;
}

typedef struct BacaImageProbe {
    const char *uri;
    int width;
    int height;
    bool broken;
} BacaImageProbe;

static bool data_uri_probe_size(const char *uri, size_t *size) {
    const char *comma = strchr(uri + 5, ',');
    if (comma == NULL) {
        return false;
    }
    size_t metadata_length = (size_t) (comma - (uri + 5));
    bool base64 = false;
    const char *semicolon = memchr(uri + 5, ';', metadata_length);
    if (semicolon != NULL) {
        const char *option = semicolon + 1;
        while (option < comma) {
            const char *option_end = memchr(option, ';', (size_t) (comma - option));
            if (option_end == NULL) {
                option_end = comma;
            }
            if (ascii_case_equal_n(option, (size_t) (option_end - option), "base64")) {
                base64 = true;
                break;
            }
            option = option_end + 1;
        }
    }

    const char *payload = comma + 1;
    size_t payload_length = strlen(payload);
    if (!base64) {
        if (payload_length > BACA_DATA_URI_MAX) {
            return false;
        }
        size_t decoded = 0U;
        for (size_t index = 0U; index < payload_length; ++index) {
            if (payload[index] == '%') {
                if (index + 2U >= payload_length || hex_value(payload[index + 1U]) < 0 ||
                    hex_value(payload[index + 2U]) < 0) {
                    return false;
                }
                index += 2U;
            }
            ++decoded;
        }
        *size = decoded;
        return true;
    }

    if (payload_length > BACA_DATA_URI_MAX * 2U) {
        return false;
    }
    size_t significant = 0U;
    size_t padding = 0U;
    bool padded = false;
    for (size_t index = 0U; index < payload_length; ++index) {
        unsigned char value = (unsigned char) payload[index];
        if (isspace(value) != 0) {
            continue;
        }
        if (value == '=') {
            padded = true;
            ++padding;
        } else if (base64_value(value) < 0 || padded) {
            return false;
        }
        ++significant;
    }
    size_t remainder = significant % 4U;
    if (remainder == 1U || padding > 2U || (padding > 0U && remainder != 0U)) {
        return false;
    }
    size_t decoded = (significant / 4U) * 3U;
    if (remainder > 1U) {
        decoded += remainder - 1U;
    }
    if (padding > decoded) {
        return false;
    }
    *size = decoded - padding;
    return true;
}

static bool archive_probe_size(zip_t *archive, const char *uri, size_t *size) {
    if (archive == NULL) {
        return false;
    }
    BacaError ignored = {0};
    char *member_path = baca_document_uri_path(uri, &ignored);
    if (member_path == NULL) {
        return false;
    }
    zip_int64_t member_index = zip_name_locate(archive, member_path, ZIP_FL_ENC_GUESS);
    free(member_path);
    if (member_index < 0) {
        return false;
    }
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive, (zip_uint64_t) member_index, ZIP_FL_ENC_GUESS, &stat) != 0 ||
        (stat.valid & ZIP_STAT_SIZE) == 0 || stat.size > (zip_uint64_t) SIZE_MAX) {
        return false;
    }
    *size = (size_t) stat.size;
    return true;
}

static bool image_probe_size(zip_t *archive, const char *uri, size_t *size) {
    return strlen(uri) >= 5U && ascii_case_equal_n(uri, 5U, "data:") ?
               data_uri_probe_size(uri, size) :
               archive_probe_size(archive, uri, size);
}

void baca_document_probe_images(BacaDocument *document, bool enabled) {
    if (document == NULL || (document->block_count > 0U && document->blocks == NULL)) {
        return;
    }
    size_t image_count = 0U;
    for (size_t index = 0U; index < document->block_count; ++index) {
        BacaBlock *block = &document->blocks[index];
        if (block->kind != BACA_BLOCK_IMAGE) {
            continue;
        }
        ++image_count;
        BacaImageBlock *image = &block->value.image;
        image->intrinsic_width = 0;
        image->intrinsic_height = 0;
        image->broken = true;
    }
    if (!enabled || image_count == 0U) {
        return;
    }

    size_t probe_capacity = image_count;
    if (probe_capacity > BACA_GRAPHICS_MAX_PROBES) {
        probe_capacity = BACA_GRAPHICS_MAX_PROBES;
    }
    BacaImageProbe *probes = calloc(probe_capacity, sizeof(*probes));
    if (probes == NULL) {
        return;
    }
    zip_t *archive = NULL;
    if (document->path != NULL) {
        int zip_error = 0;
        archive = zip_open(document->path, ZIP_RDONLY, &zip_error);
    }
    size_t probe_count = 0U;
    size_t probe_bytes = 0U;
    for (size_t index = 0U; index < document->block_count; ++index) {
        BacaBlock *block = &document->blocks[index];
        if (block->kind != BACA_BLOCK_IMAGE || block->value.image.uri == NULL) {
            continue;
        }
        BacaImageBlock *image = &block->value.image;
        size_t prior = 0U;
        while (prior < probe_count && strcmp(probes[prior].uri, image->uri) != 0) {
            ++prior;
        }
        if (prior < probe_count) {
            image->intrinsic_width = probes[prior].width;
            image->intrinsic_height = probes[prior].height;
            image->broken = probes[prior].broken;
            continue;
        }
        if (probe_count == probe_capacity) {
            continue;
        }

        BacaError ignored = {0};
        BacaResource resource = {0};
        size_t resource_size = 0U;
        if (image_probe_size(archive, image->uri, &resource_size) &&
            resource_size <= BACA_GRAPHICS_MAX_INPUT_BYTES &&
            resource_size <= BACA_GRAPHICS_MAX_PROBE_BYTES - probe_bytes) {
            probe_bytes += resource_size;
            if (baca_document_load_resource(document, image->uri, &resource, &ignored)) {
                if (resource.length <= resource_size &&
                    baca_graphics_probe_resource(&resource, &image->intrinsic_width,
                                                 &image->intrinsic_height, &ignored)) {
                    image->broken = false;
                } else if (resource.length > resource_size) {
                    /* The backend violated the size established before loading. */
                    probe_bytes = BACA_GRAPHICS_MAX_PROBE_BYTES;
                }
            }
        }
        baca_resource_free(&resource);
        probes[probe_count++] = (BacaImageProbe) {
            .uri = image->uri,
            .width = image->intrinsic_width,
            .height = image->intrinsic_height,
            .broken = image->broken,
        };
    }
    if (archive != NULL) {
        zip_discard(archive);
    }
    free(probes);
}

bool baca_document_open(BacaDocument *document, const char *path, BacaError *error) {
    if (error != NULL) {
        baca_error_clear(error);
    }
    if (document == NULL || path == NULL || path[0] == '\0') {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document output and path are required");
        return false;
    }
    if (!document_output_is_empty(document)) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document output must be zero-initialized");
        return false;
    }

    BacaDocument opened = {0};
    opened.path = baca_realpath(path, error);
    if (opened.path == NULL) {
        return false;
    }

    unsigned char magic[80] = {0};
    size_t magic_length = 0;
    if (!read_magic(opened.path, magic, sizeof(magic), &magic_length, error)) {
        baca_document_close(&opened);
        return false;
    }
    const char *extension = baca_path_extension(opened.path);
    opened.format = detect_format(opened.path, extension, magic, magic_length);

    bool success = false;
    if (opened.format == BACA_FORMAT_EPUB) {
        success = baca_epub_open(&opened, opened.path, NULL, error);
    } else if (opened.format == BACA_FORMAT_MOBI || opened.format == BACA_FORMAT_AZW) {
        success = baca_mobi_open(&opened, opened.path, error);
    } else if (opened.format == BACA_FORMAT_PDF) {
        baca_error_set(error, BACA_ERROR_UNSUPPORTED, "PDF support is not implemented yet");
    } else if (opened.format == BACA_FORMAT_IMAGE) {
        baca_error_set(error, BACA_ERROR_UNSUPPORTED, "image document support is not implemented yet");
    } else if (opened.format == BACA_FORMAT_COMIC) {
        baca_error_set(error, BACA_ERROR_UNSUPPORTED, "comic archive support is not implemented yet");
    } else if (opened.format == BACA_FORMAT_TEXT || opened.format == BACA_FORMAT_FB2) {
        baca_error_set(error, BACA_ERROR_UNSUPPORTED, "%s support is not implemented yet",
                       baca_document_format_name(opened.format));
    } else {
        baca_error_set(error, BACA_ERROR_UNSUPPORTED, "unsupported document format: %s", path);
    }

    if (!success) {
        baca_document_close(&opened);
        return false;
    }
    *document = opened;
    return true;
}

void baca_document_close(BacaDocument *document) {
    if (document == NULL) {
        return;
    }
    if (document->ops != NULL && document->ops->close != NULL) {
        document->ops->close(document);
    }
    for (size_t index = 0; index < document->block_count; index++) {
        block_free(&document->blocks[index]);
    }
    free(document->blocks);
    for (size_t index = 0; index < document->section_count; index++) {
        free(document->sections[index].id);
    }
    free(document->sections);
    for (size_t index = 0; index < document->toc_count; index++) {
        free(document->toc[index].label);
        free(document->toc[index].target);
    }
    free(document->toc);
    metadata_free(&document->metadata);
    free(document->path);
    memset(document, 0, sizeof(*document));
}

const char *baca_document_format_name(BacaDocumentFormat format) {
    switch (format) {
        case BACA_FORMAT_EPUB:
            return "EPUB";
        case BACA_FORMAT_MOBI:
            return "MOBI";
        case BACA_FORMAT_AZW:
            return "AZW";
        case BACA_FORMAT_PDF:
            return "PDF";
        case BACA_FORMAT_IMAGE:
            return "image";
        case BACA_FORMAT_COMIC:
            return "comic archive";
        case BACA_FORMAT_TEXT:
            return "text";
        case BACA_FORMAT_FB2:
            return "FB2";
        case BACA_FORMAT_UNKNOWN:
        default:
            return "unknown";
    }
}

static size_t target_path_length(const char *target) {
    size_t length = strcspn(target, "#?");
    return length;
}

static bool target_path_equal(const char *left, const char *right) {
    size_t left_length = target_path_length(left);
    size_t right_length = target_path_length(right);
    return left_length == right_length && memcmp(left, right, left_length) == 0;
}

typedef struct SectionPathIndex {
    const char *path;
    size_t index;
} SectionPathIndex;

static int compare_section_path(const void *left, const void *right) {
    const SectionPathIndex *left_index = left;
    const SectionPathIndex *right_index = right;
    return strcmp(left_index->path, right_index->path);
}

static int compare_path_span(const char *path, size_t path_length, const char *candidate) {
    size_t candidate_length = strlen(candidate);
    size_t shared = path_length < candidate_length ? path_length : candidate_length;
    int result = memcmp(path, candidate, shared);
    if (result != 0) {
        return result;
    }
    if (path_length == candidate_length) {
        return 0;
    }
    return path_length < candidate_length ? -1 : 1;
}

static size_t indexed_section(const SectionPathIndex *sections, size_t section_count, const char *target) {
    size_t target_length = target_path_length(target);
    size_t first = 0;
    size_t last = section_count;
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        int comparison = compare_path_span(target, target_length, sections[middle].path);
        if (comparison > 0) {
            first = middle + 1U;
        } else {
            last = middle;
        }
    }
    if (first == section_count || compare_path_span(target, target_length, sections[first].path) != 0) {
        return SIZE_MAX;
    }
    size_t result = sections[first].index;
    while (++first < section_count && compare_path_span(target, target_length, sections[first].path) == 0) {
        if (sections[first].index < result) {
            result = sections[first].index;
        }
    }
    return result;
}

bool baca_document_index_toc_sections(BacaDocument *document, BacaError *error) {
    if (document == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document is required");
        return false;
    }
    for (size_t index = 0; index < document->toc_count; index++) {
        document->toc[index].section_index = SIZE_MAX;
    }
    if (document->section_count == 0 || document->toc_count == 0) {
        return true;
    }
    SectionPathIndex *sections = baca_reallocarray(NULL, document->section_count, sizeof(*sections), error);
    if (sections == NULL) {
        return false;
    }
    for (size_t index = 0; index < document->section_count; index++) {
        if (document->sections[index].id == NULL) {
            free(sections);
            baca_error_set(error, BACA_ERROR_CORRUPT, "document section has no id");
            return false;
        }
        sections[index] = (SectionPathIndex) {
            .path = document->sections[index].id,
            .index = index,
        };
    }
    qsort(sections, document->section_count, sizeof(*sections), compare_section_path);
    for (size_t index = 0; index < document->toc_count; index++) {
        document->toc[index].section_index = indexed_section(sections, document->section_count,
                                                             document->toc[index].target);
    }
    free(sections);
    return true;
}

const BacaTocEntry *baca_document_current_toc(const BacaDocument *document, const char *target) {
    if (document == NULL || target == NULL || document->toc_count == 0) {
        return NULL;
    }
    for (size_t index = document->toc_count; index > 0; index--) {
        if (strcmp(document->toc[index - 1].target, target) == 0) {
            return &document->toc[index - 1];
        }
    }

    size_t target_section = document->section_count;
    for (size_t index = 0; index < document->section_count; index++) {
        if (target_path_equal(document->sections[index].id, target)) {
            target_section = index;
            break;
        }
    }
    if (target_section == document->section_count) {
        return NULL;
    }
    const BacaTocEntry *current = NULL;
    size_t current_section = 0;
    for (size_t toc_index = 0; toc_index < document->toc_count; toc_index++) {
        size_t section_index = document->toc[toc_index].section_index;
        if (section_index == SIZE_MAX &&
            target_path_equal(document->toc[toc_index].target, document->sections[target_section].id)) {
            section_index = target_section;
        }
        if (section_index <= target_section && (current == NULL || section_index >= current_section)) {
            current = &document->toc[toc_index];
            current_section = section_index;
        }
    }
    return current;
}
