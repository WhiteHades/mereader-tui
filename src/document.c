#include "mereader-tui/document_backend.h"
#include "mereader-tui/graphics.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zip.h>

#define MEREADER_TUI_DATA_URI_MAX (64U * 1024U * 1024U)

static atomic_uint_least64_t document_instance_serial;

static uint64_t next_document_instance_id(void) {
    uint_least64_t instance_id = 0U;
    while (instance_id == 0U) {
        instance_id = atomic_fetch_add_explicit(&document_instance_serial, 1U, memory_order_relaxed) + 1U;
    }
    return (uint64_t) instance_id;
}

static void metadata_free(MereaderTuiMetadata *metadata) {
    free(metadata->title);
    free(metadata->author);
    free(metadata->creator);
    free(metadata->description);
    free(metadata->publisher);
    free(metadata->producer);
    free(metadata->date);
    free(metadata->creation_date);
    free(metadata->modification_date);
    free(metadata->language);
    free(metadata->format);
    free(metadata->identifier);
    free(metadata->source);
    memset(metadata, 0, sizeof(*metadata));
}

static void text_block_free(MereaderTuiTextBlock *text) {
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

void mereader_tui_document_block_free(MereaderTuiBlock *block) {
    free(block->section_id);
    if (block->kind == MEREADER_TUI_BLOCK_TEXT) {
        text_block_free(&block->value.text);
    } else if (block->kind == MEREADER_TUI_BLOCK_IMAGE) {
        free(block->value.image.uri);
        free(block->value.image.alt);
        free(block->value.image.anchor);
        free(block->value.image.link);
        for (size_t index = 0; index < block->value.image.link_count; index++) {
            free(block->value.image.links[index].target);
        }
        free(block->value.image.links);
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

static bool text_block_retained_size(const MereaderTuiTextBlock *text, size_t *size, MereaderTuiError *error) {
    if ((text->span_count > 0 && text->spans == NULL) ||
        (text->anchor_count > 0 && text->anchors == NULL)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "text block metadata arrays are missing");
        return false;
    }
    if (text->span_count > MEREADER_TUI_DOCUMENT_MAX_SPANS_PER_BLOCK ||
        text->anchor_count > MEREADER_TUI_DOCUMENT_MAX_ANCHORS_PER_BLOCK) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "text block metadata exceeds the supported count limit");
        return false;
    }
    if (!retained_add_string(size, text->text) ||
        text->span_count > SIZE_MAX / sizeof(*text->spans) ||
        !retained_add(size, text->span_count * sizeof(*text->spans)) ||
        text->anchor_count > SIZE_MAX / sizeof(*text->anchors) ||
        !retained_add(size, text->anchor_count * sizeof(*text->anchors))) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "text block retained size overflow");
        return false;
    }
    for (size_t index = 0; index < text->span_count; index++) {
        if (!retained_add_string(size, text->spans[index].link)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "text span retained size overflow");
            return false;
        }
    }
    for (size_t index = 0; index < text->anchor_count; index++) {
        if (!retained_add_string(size, text->anchors[index].target)) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "text anchor retained size overflow");
            return false;
        }
    }
    return true;
}

static bool block_retained_size(const MereaderTuiBlock *block, size_t *size, MereaderTuiError *error) {
    size_t total = sizeof(*block);
    if (!retained_add_string(&total, block->section_id)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "block retained size overflow");
        return false;
    }
    if (block->kind == MEREADER_TUI_BLOCK_TEXT) {
        if (!text_block_retained_size(&block->value.text, &total, error)) {
            return false;
        }
    } else if (block->kind == MEREADER_TUI_BLOCK_IMAGE) {
        if ((block->value.image.link_count > 0 && block->value.image.links == NULL) ||
            block->value.image.link_count > MEREADER_TUI_DOCUMENT_MAX_LINKS_PER_IMAGE) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "image link map exceeds the supported count limit");
            return false;
        }
        if (!retained_add_string(&total, block->value.image.uri) ||
            !retained_add_string(&total, block->value.image.alt) ||
            !retained_add_string(&total, block->value.image.anchor) ||
            !retained_add_string(&total, block->value.image.link) ||
            block->value.image.link_count > SIZE_MAX / sizeof(*block->value.image.links) ||
            !retained_add(&total, block->value.image.link_count * sizeof(*block->value.image.links))) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "image block retained size overflow");
            return false;
        }
        for (size_t index = 0; index < block->value.image.link_count; index++) {
            if (!retained_add_string(&total, block->value.image.links[index].target)) {
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "image link map retained size overflow");
                return false;
            }
        }
    }
    *size = total;
    return true;
}

static bool document_can_retain(const MereaderTuiDocument *document, size_t amount, MereaderTuiError *error) {
    if (document->retained_bytes > MEREADER_TUI_DOCUMENT_MAX_RETAINED_BYTES ||
        amount > MEREADER_TUI_DOCUMENT_MAX_RETAINED_BYTES - document->retained_bytes) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document retained data exceeds the supported size limit");
        return false;
    }
    return true;
}

static bool add_block(MereaderTuiDocument *document, MereaderTuiBlock *block, MereaderTuiBlockKind expected, MereaderTuiError *error) {
    if (document == NULL || block == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document and block are required");
        return false;
    }
    if (block->kind != expected) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "block kind does not match add helper");
        return false;
    }
    if (document->block_count >= MEREADER_TUI_DOCUMENT_MAX_BLOCKS) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document contains too many blocks");
        return false;
    }
    size_t retained_size = 0;
    if (!block_retained_size(block, &retained_size, error) ||
        !document_can_retain(document, retained_size, error)) {
        return false;
    }
    MereaderTuiError reserve_error = {0};
    MereaderTuiBlock *blocks = mereader_tui_array_reserve(document->blocks, &document->block_capacity, sizeof(*document->blocks),
                                           document->block_count + 1, &reserve_error);
    if (mereader_tui_error_is_set(&reserve_error)) {
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

bool mereader_tui_document_add_text_block(MereaderTuiDocument *document, MereaderTuiBlock *block, MereaderTuiError *error) {
    return add_block(document, block, MEREADER_TUI_BLOCK_TEXT, error);
}

bool mereader_tui_document_add_image_block(MereaderTuiDocument *document, MereaderTuiBlock *block, MereaderTuiError *error) {
    return add_block(document, block, MEREADER_TUI_BLOCK_IMAGE, error);
}

bool mereader_tui_document_add_toc(MereaderTuiDocument *document, const char *label, const char *target, unsigned depth,
                           MereaderTuiError *error) {
    if (document == NULL || label == NULL || target == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document, TOC label, and TOC target are required");
        return false;
    }
    if (document->toc_count >= MEREADER_TUI_DOCUMENT_MAX_TOC_ENTRIES) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document contains too many TOC entries");
        return false;
    }
    size_t retained_size = sizeof(*document->toc);
    if (!retained_add_string(&retained_size, label) || !retained_add_string(&retained_size, target)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "TOC entry retained size overflow");
        return false;
    }
    if (!document_can_retain(document, retained_size, error)) {
        return false;
    }

    char *owned_label = mereader_tui_strdup(label, error);
    if (owned_label == NULL) {
        return false;
    }
    char *owned_target = mereader_tui_strdup(target, error);
    if (owned_target == NULL) {
        free(owned_label);
        return false;
    }
    MereaderTuiError reserve_error = {0};
    MereaderTuiTocEntry *toc = mereader_tui_array_reserve(document->toc, &document->toc_capacity, sizeof(*document->toc),
                                           document->toc_count + 1, &reserve_error);
    if (mereader_tui_error_is_set(&reserve_error)) {
        if (error != NULL) {
            *error = reserve_error;
        }
        free(owned_label);
        free(owned_target);
        return false;
    }
    document->toc = toc;

    document->toc[document->toc_count++] = (MereaderTuiTocEntry) {
        .label = owned_label,
        .target = owned_target,
        .depth = depth,
        .section_index = SIZE_MAX,
    };
    document->retained_bytes += retained_size;
    return true;
}

bool mereader_tui_document_add_section(MereaderTuiDocument *document, MereaderTuiSection *section, MereaderTuiError *error) {
    if (document == NULL || section == NULL || section->id == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document and section id are required");
        return false;
    }
    if (document->section_count >= MEREADER_TUI_DOCUMENT_MAX_SECTIONS) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document contains too many sections");
        return false;
    }
    size_t retained_size = sizeof(*document->sections);
    if (!retained_add_string(&retained_size, section->id)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "section retained size overflow");
        return false;
    }
    if (!document_can_retain(document, retained_size, error)) {
        return false;
    }
    MereaderTuiError reserve_error = {0};
    MereaderTuiSection *sections =
        mereader_tui_array_reserve(document->sections, &document->section_capacity, sizeof(*document->sections),
                           document->section_count + 1, &reserve_error);
    if (mereader_tui_error_is_set(&reserve_error)) {
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

bool mereader_tui_document_account_metadata(MereaderTuiDocument *document, MereaderTuiError *error) {
    if (document == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document is required");
        return false;
    }
    const char *const values[] = {
        document->metadata.title,       document->metadata.author,   document->metadata.creator,
        document->metadata.description, document->metadata.publisher, document->metadata.producer,
        document->metadata.date,        document->metadata.creation_date,
        document->metadata.modification_date, document->metadata.language,
        document->metadata.format,      document->metadata.identifier,
        document->metadata.source,
    };
    size_t retained_size = 0;
    for (size_t index = 0; index < MEREADER_TUI_ARRAY_LEN(values); index++) {
        if (!retained_add_string(&retained_size, values[index])) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "metadata retained size overflow");
            return false;
        }
    }
    if (!document_can_retain(document, retained_size, error)) {
        return false;
    }
    document->retained_bytes += retained_size;
    return true;
}

void mereader_tui_document_rollback_blocks(MereaderTuiDocument *document, size_t first_block) {
    if (document == NULL) {
        return;
    }
    while (document->block_count > first_block) {
        document->block_count--;
        size_t retained_size = 0;
        MereaderTuiError ignored = {0};
        if (block_retained_size(&document->blocks[document->block_count], &retained_size, &ignored) &&
            retained_size <= document->retained_bytes) {
            document->retained_bytes -= retained_size;
        }
        mereader_tui_document_block_free(&document->blocks[document->block_count]);
    }
}

void mereader_tui_document_rollback_toc(MereaderTuiDocument *document, size_t first_entry) {
    if (document == NULL) {
        return;
    }
    while (document->toc_count > first_entry) {
        document->toc_count--;
        MereaderTuiTocEntry *entry = &document->toc[document->toc_count];
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

void mereader_tui_resource_free(MereaderTuiResource *resource) {
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
                          MereaderTuiError *error) {
    if (input_length > MEREADER_TUI_DATA_URI_MAX * 2U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "data URI exceeds the supported size limit");
        return false;
    }
    size_t capacity = (input_length / 4U + 1U) * 3U;
    if (capacity > MEREADER_TUI_DATA_URI_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "decoded data URI exceeds the supported size limit");
        return false;
    }
    unsigned char *decoded = malloc(capacity == 0 ? 1 : capacity);
    if (decoded == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate data URI payload");
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
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid base64 data URI");
                return false;
            }
        } else if (digit < 0) {
            free(decoded);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid base64 data URI");
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
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid base64 data URI padding");
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
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "truncated base64 data URI");
        return false;
    }
    if (quartet_length == 2 || quartet_length == 3) {
        if (quartet[0] < 0 || quartet[1] < 0 || (quartet_length == 3 && quartet[2] < 0)) {
            free(decoded);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid base64 data URI padding");
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

#define MEREADER_TUI_DOCUMENT_URI_MAX (1024U * 1024U)

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

static char *decode_uri_component(const char *value, size_t length, MereaderTuiError *error) {
    char *decoded = malloc(length + 1U);
    if (decoded == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate document URI");
        return NULL;
    }
    size_t output = 0;
    for (size_t input = 0; input < length; input++) {
        unsigned char byte = (unsigned char) value[input];
        if (byte == '%') {
            if (input + 2U >= length) {
                free(decoded);
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "incomplete percent escape in document URI");
                return NULL;
            }
            int high = hex_value(value[input + 1U]);
            int low = hex_value(value[input + 2U]);
            if (high < 0 || low < 0) {
                free(decoded);
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid percent escape in document URI");
                return NULL;
            }
            byte = (unsigned char) ((unsigned) high * 16U + (unsigned) low);
            input += 2U;
        }
        if (byte == '\0') {
            free(decoded);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document URI contains an encoded NUL byte");
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

static bool append_canonical_component(MereaderTuiString *output, const char *value, size_t length,
                                       UriComponent component, MereaderTuiError *error) {
    static const char hexadecimal[] = "0123456789ABCDEF";
    for (size_t index = 0; index < length; index++) {
        unsigned char byte = (unsigned char) value[index];
        bool literal = uri_pchar(byte) ||
                       (component != URI_COMPONENT_PATH && (byte == '/' || byte == '?'));
        if (literal) {
            if (!mereader_tui_string_append_char(output, (char) byte, error)) {
                return false;
            }
        } else {
            char encoded[3] = {'%', hexadecimal[byte >> 4U], hexadecimal[byte & 0x0fU]};
            if (!mereader_tui_string_append_n(output, encoded, sizeof(encoded), error)) {
                return false;
            }
        }
    }
    return true;
}

static bool normalize_uri_path(const char *path, MereaderTuiString *output, MereaderTuiError *error) {
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
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document URI contains an unsafe path byte");
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
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "relative URI escapes the document root");
                success = false;
                break;
            }
            output->length = segments[--segment_count];
            output->data[output->length] = '\0';
            continue;
        }

        MereaderTuiError reserve_error = {0};
        size_t *resized = mereader_tui_array_reserve(segments, &segment_capacity, sizeof(*segments),
                                             segment_count + 1U, &reserve_error);
        if (mereader_tui_error_is_set(&reserve_error)) {
            if (error != NULL) {
                *error = reserve_error;
            }
            success = false;
            break;
        }
        segments = resized;
        size_t rollback = output->length;
        if ((output->length > 0 && !mereader_tui_string_append_char(output, '/', error)) ||
            !append_canonical_component(output, path + start, segment_length, URI_COMPONENT_PATH, error)) {
            success = false;
            break;
        }
        segments[segment_count++] = rollback;
    }
    free(segments);
    return success;
}

static bool append_decoded_component(MereaderTuiString *output, char marker, const char *value, size_t length,
                                     UriComponent component, MereaderTuiError *error) {
    char *decoded = decode_uri_component(value, length, error);
    if (decoded == NULL) {
        return false;
    }
    bool success = mereader_tui_string_append_char(output, marker, error) &&
                   append_canonical_component(output, decoded, strlen(decoded), component, error);
    free(decoded);
    return success;
}

char *mereader_tui_document_resolve_uri(const char *base, const char *reference, bool allow_external,
                                MereaderTuiError *error) {
    if (base == NULL || reference == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "cannot resolve a null document URI");
        return NULL;
    }
    if (mereader_tui_is_external_uri(reference) || strncmp(reference, "//", 2) == 0) {
        if (!allow_external) {
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT,
                           "external URI used where a document member was required: %s", reference);
            return NULL;
        }
        return mereader_tui_strdup(reference, error);
    }
    if (strlen(base) > MEREADER_TUI_DOCUMENT_URI_MAX || strlen(reference) > MEREADER_TUI_DOCUMENT_URI_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document URI exceeds the supported size limit");
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

    MereaderTuiString merged = {0};
    bool success = true;
    if (reference_parts.path_length == 0) {
        success = mereader_tui_string_append(&merged, base_path, error);
    } else if (reference_path[0] == '/') {
        success = mereader_tui_string_append(&merged, reference_path, error);
    } else {
        const char *slash = strrchr(base_path, '/');
        if (slash != NULL) {
            success = mereader_tui_string_append_n(&merged, base_path, (size_t) (slash - base_path + 1), error);
        }
        if (success) {
            success = mereader_tui_string_append(&merged, reference_path, error);
        }
    }

    MereaderTuiString result = {0};
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
    mereader_tui_string_free(&merged);
    if (!success) {
        mereader_tui_string_free(&result);
        return NULL;
    }
    char *resolved = mereader_tui_string_take(&result);
    return resolved == NULL ? mereader_tui_strdup("", error) : resolved;
}

char *mereader_tui_document_uri_path(const char *uri, MereaderTuiError *error) {
    if (uri == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document URI is required");
        return NULL;
    }
    if (mereader_tui_is_external_uri(uri) || strncmp(uri, "//", 2) == 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "external URI has no document member path");
        return NULL;
    }
    if (strlen(uri) > MEREADER_TUI_DOCUMENT_URI_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document URI exceeds the supported size limit");
        return NULL;
    }
    UriParts parts = split_uri(uri);
    return decode_uri_component(parts.path, parts.path_length, error);
}

char *mereader_tui_document_fragment_target(const char *base, const char *fragment, MereaderTuiError *error) {
    if (base == NULL || fragment == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document target base and fragment are required");
        return NULL;
    }
    if (mereader_tui_is_external_uri(base) || strlen(base) > MEREADER_TUI_DOCUMENT_URI_MAX ||
        strlen(fragment) > MEREADER_TUI_DOCUMENT_URI_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid document target");
        return NULL;
    }
    UriParts parts = split_uri(base);
    MereaderTuiString target = {0};
    if (!mereader_tui_string_append_n(&target, parts.path, parts.path_length, error) ||
        !mereader_tui_string_append_char(&target, '#', error) ||
        !append_canonical_component(&target, fragment, strlen(fragment), URI_COMPONENT_FRAGMENT, error)) {
        mereader_tui_string_free(&target);
        return NULL;
    }
    return mereader_tui_string_take(&target);
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
                                size_t *output_length, MereaderTuiError *error) {
    if (input_length > MEREADER_TUI_DATA_URI_MAX) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "data URI exceeds the supported size limit");
        return false;
    }
    unsigned char *decoded = malloc(input_length == 0 ? 1 : input_length);
    if (decoded == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate data URI payload");
        return false;
    }
    size_t written = 0;
    for (size_t index = 0; index < input_length; index++) {
        if (input[index] == '%') {
            if (index + 2 >= input_length) {
                free(decoded);
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "truncated percent escape in data URI");
                return false;
            }
            int high = hex_value(input[index + 1]);
            int low = hex_value(input[index + 2]);
            if (high < 0 || low < 0) {
                free(decoded);
                mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "invalid percent escape in data URI");
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

static bool load_data_uri(const char *uri, MereaderTuiResource *resource, MereaderTuiError *error) {
    const char *comma = strchr(uri + 5, ',');
    if (comma == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "data URI has no payload separator");
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

    char *mime_type = mime_length == 0 ? mereader_tui_strdup("text/plain;charset=US-ASCII", error)
                                       : mereader_tui_strndup(uri + 5, mime_length, error);
    if (mime_type == NULL) {
        free(data);
        return false;
    }
    resource->data = data;
    resource->length = data_length;
    resource->mime_type = mime_type;
    return true;
}

static bool resource_output_is_empty(const MereaderTuiResource *resource) {
    return resource->data == NULL && resource->length == 0 && resource->mime_type == NULL;
}

bool mereader_tui_document_load_resource(MereaderTuiDocument *document, const char *uri, MereaderTuiResource *resource,
                                  MereaderTuiError *error) {
    if (error != NULL) {
        mereader_tui_error_clear(error);
    }
    if (document == NULL || uri == NULL || resource == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document, URI, and resource output are required");
        return false;
    }
    if (!resource_output_is_empty(resource)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "resource output must be zero-initialized");
        return false;
    }

    MereaderTuiResource loaded = {0};
    bool success;
    if (strlen(uri) >= 5 && ascii_case_equal_n(uri, 5, "data:")) {
        success = load_data_uri(uri, &loaded, error);
    } else if (document->ops != NULL && document->ops->load_resource != NULL) {
        success = document->ops->load_resource(document, uri, &loaded, error);
    } else {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED, "resources are not supported for this document format");
        success = false;
    }
    if (!success) {
        mereader_tui_resource_free(&loaded);
        return false;
    }
    *resource = loaded;
    return true;
}

bool mereader_tui_document_render_page(MereaderTuiDocument *document, int page_index, int width, int height,
                               uint32_t background, MereaderTuiResource *resource, MereaderTuiError *error) {
    if (error != NULL) {
        mereader_tui_error_clear(error);
    }
    if (document == NULL || resource == NULL || page_index < 0 || width <= 0 || height <= 0) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document, page, dimensions, and resource output are required");
        return false;
    }
    if (!resource_output_is_empty(resource)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "resource output must be zero-initialized");
        return false;
    }
    if (document->ops == NULL || document->ops->render_page == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED, "page rendering is not supported for this document format");
        return false;
    }
    MereaderTuiResource rendered = {0};
    if (!document->ops->render_page(document, page_index, width, height, background, &rendered, error)) {
        mereader_tui_resource_free(&rendered);
        return false;
    }
    *resource = rendered;
    return true;
}

static bool extension_is(const char *extension, const char *expected) {
    return extension != NULL && mereader_tui_casecmp(extension, expected) == 0;
}

static bool read_magic(const char *path, unsigned char *buffer, size_t capacity, size_t *length,
                       struct stat *identity, MereaderTuiError *error) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (descriptor < 0) {
        const int saved_errno = errno;
        const MereaderTuiErrorCode code = saved_errno == ENOENT || saved_errno == ENOTDIR
                                       ? MEREADER_TUI_ERROR_NOT_FOUND
                                       : MEREADER_TUI_ERROR_IO;
        mereader_tui_error_set(error, code, "could not open %s: %s", path, strerror(saved_errno));
        return false;
    }

    struct stat status;
    if (fstat(descriptor, &status) != 0) {
        const int saved_errno = errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect %s: %s", path, strerror(saved_errno));
        return false;
    }
    if (!S_ISREG(status.st_mode)) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED, "input is not a regular file: %s", path);
        return false;
    }

    size_t offset = 0U;
    while (offset < capacity) {
        const ssize_t count = read(descriptor, buffer + offset, capacity - offset);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        const int saved_errno = errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read %s: %s", path, strerror(saved_errno));
        return false;
    }
    if (close(descriptor) != 0) {
        const int saved_errno = errno;
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close %s: %s", path, strerror(saved_errno));
        return false;
    }
    *length = offset;
    *identity = status;
    return true;
}

static bool snapshot_identity_matches(const struct stat *expected, const struct stat *actual) {
    return expected->st_dev == actual->st_dev && expected->st_ino == actual->st_ino &&
           expected->st_mode == actual->st_mode && expected->st_size == actual->st_size &&
           expected->st_mtim.tv_sec == actual->st_mtim.tv_sec &&
           expected->st_mtim.tv_nsec == actual->st_mtim.tv_nsec &&
           expected->st_ctim.tv_sec == actual->st_ctim.tv_sec &&
           expected->st_ctim.tv_nsec == actual->st_ctim.tv_nsec;
}

bool mereader_tui_document_read_snapshot(const char *path, const struct stat *expected_identity, size_t maximum,
                                 MereaderTuiBuffer *output, MereaderTuiError *error) {
    if (path == NULL || path[0] == '\0' || expected_identity == NULL || output == NULL || maximum == 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "path, identity, limit, and snapshot output are required");
        return false;
    }
    if (output->data != NULL || output->length != 0U || output->capacity != 0U) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "snapshot output must be zero-initialized");
        return false;
    }

    const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (descriptor < 0) {
        const int saved_errno = errno;
        const MereaderTuiErrorCode code = saved_errno == ENOENT || saved_errno == ENOTDIR ?
                                       MEREADER_TUI_ERROR_NOT_FOUND :
                                       MEREADER_TUI_ERROR_IO;
        mereader_tui_error_set(error, code, "could not open %s: %s", path, strerror(saved_errno));
        return false;
    }
    struct stat before;
    if (fstat(descriptor, &before) != 0) {
        const int saved_errno = errno;
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not inspect %s: %s", path, strerror(saved_errno));
        return false;
    }
    if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
        !snapshot_identity_matches(expected_identity, &before)) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document changed between format detection and open: %s", path);
        return false;
    }
    if ((uintmax_t)before.st_size > maximum || (uintmax_t)before.st_size > SIZE_MAX - 1U) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document exceeds the supported input size limit: %s", path);
        return false;
    }

    const size_t length = (size_t)before.st_size;
    unsigned char *data = malloc(length + 1U);
    if (data == NULL) {
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_MEMORY, "could not allocate document snapshot");
        return false;
    }
    size_t offset = 0U;
    while (offset < length) {
        const ssize_t count = read(descriptor, data + offset, length - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count == 0) {
            free(data);
            (void)close(descriptor);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document was truncated while opening: %s", path);
            return false;
        } else if (errno != EINTR) {
            const int saved_errno = errno;
            free(data);
            (void)close(descriptor);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not read %s: %s", path, strerror(saved_errno));
            return false;
        }
    }
    data[length] = '\0';
    struct stat after;
    if (fstat(descriptor, &after) != 0 || !snapshot_identity_matches(&before, &after)) {
        free(data);
        (void)close(descriptor);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document changed while opening: %s", path);
        return false;
    }
    if (close(descriptor) != 0) {
        const int saved_errno = errno;
        free(data);
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO, "could not close %s: %s", path, strerror(saved_errno));
        return false;
    }
    *output = (MereaderTuiBuffer){.data = data, .length = length, .capacity = length + 1U};
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

static MereaderTuiDocumentFormat extension_format(const char *extension) {
    if (extension_is(extension, ".epub") || extension_is(extension, ".epub3")) {
        return MEREADER_TUI_FORMAT_EPUB;
    }
    if (extension_is(extension, ".mobi") || extension_is(extension, ".prc")) {
        return MEREADER_TUI_FORMAT_MOBI;
    }
    if (extension_is(extension, ".azw") || extension_is(extension, ".azw3") ||
        extension_is(extension, ".azw4")) {
        return MEREADER_TUI_FORMAT_AZW;
    }
    if (extension_is(extension, ".pdf")) {
        return MEREADER_TUI_FORMAT_PDF;
    }
    if (extension_is(extension, ".cbz") || extension_is(extension, ".cbr") ||
        extension_is(extension, ".cb7")) {
        return MEREADER_TUI_FORMAT_COMIC;
    }
    if (extension_is(extension, ".png") || extension_is(extension, ".jpg") ||
        extension_is(extension, ".jpeg") || extension_is(extension, ".gif") ||
        extension_is(extension, ".webp") || extension_is(extension, ".bmp") ||
        extension_is(extension, ".svg")) {
        return MEREADER_TUI_FORMAT_IMAGE;
    }
    if (extension_is(extension, ".txt") || extension_is(extension, ".md")) {
        return MEREADER_TUI_FORMAT_TEXT;
    }
    if (extension_is(extension, ".fb2")) {
        return MEREADER_TUI_FORMAT_FB2;
    }
    return MEREADER_TUI_FORMAT_UNKNOWN;
}

static bool magic_starts_with(const unsigned char *magic, size_t magic_length, size_t offset,
                              const char *value, size_t value_length) {
    return offset <= magic_length && value_length <= magic_length - offset &&
           memcmp(magic + offset, value, value_length) == 0;
}

static size_t magic_find(const unsigned char *magic, size_t magic_length, size_t offset,
                         const char *value, size_t value_length) {
    if (value_length == 0U || offset > magic_length || value_length > magic_length - offset) {
        return SIZE_MAX;
    }
    for (size_t index = offset; index <= magic_length - value_length; ++index) {
        if (memcmp(magic + index, value, value_length) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static size_t magic_xml_root_offset(const unsigned char *magic, size_t magic_length) {
    size_t offset = 0U;
    if (magic_length >= 3U && memcmp(magic, "\xef\xbb\xbf", 3U) == 0) {
        offset = 3U;
    }
    for (;;) {
        while (offset < magic_length &&
               (magic[offset] == ' ' || magic[offset] == '\t' || magic[offset] == '\r' ||
                magic[offset] == '\n' || magic[offset] == '\f')) {
            ++offset;
        }
        if (magic_starts_with(magic, magic_length, offset, "<?xml", 5U)) {
            const size_t end = magic_find(magic, magic_length, offset + 5U, "?>", 2U);
            if (end == SIZE_MAX) {
                return SIZE_MAX;
            }
            offset = end + 2U;
            continue;
        }
        if (magic_starts_with(magic, magic_length, offset, "<!--", 4U)) {
            const size_t end = magic_find(magic, magic_length, offset + 4U, "-->", 3U);
            if (end == SIZE_MAX) {
                return SIZE_MAX;
            }
            offset = end + 3U;
            continue;
        }
        break;
    }
    return offset;
}

static bool magic_svg(const unsigned char *magic, size_t magic_length) {
    const size_t offset = magic_xml_root_offset(magic, magic_length);
    if (offset == SIZE_MAX) {
        return false;
    }
    if (!magic_starts_with(magic, magic_length, offset, "<svg", 4U) || offset + 4U >= magic_length) {
        return false;
    }
    const unsigned char delimiter = magic[offset + 4U];
    return delimiter == ' ' || delimiter == '\t' || delimiter == '\r' || delimiter == '\n' ||
           delimiter == '\f' || delimiter == '>' || delimiter == '/' || delimiter == ':';
}

static bool magic_fb2(const unsigned char *magic, size_t magic_length) {
    const size_t offset = magic_xml_root_offset(magic, magic_length);
    static const char root[] = "<FictionBook";
    if (offset == SIZE_MAX || !magic_starts_with(magic, magic_length, offset, root, sizeof(root) - 1U) ||
        offset + sizeof(root) - 1U >= magic_length) {
        return false;
    }
    const unsigned char delimiter = magic[offset + sizeof(root) - 1U];
    return delimiter == ' ' || delimiter == '\t' || delimiter == '\r' || delimiter == '\n' ||
           delimiter == '\f' || delimiter == '>' || delimiter == '/';
}

static MereaderTuiDocumentFormat detect_format(const char *path, const char *extension, const unsigned char *magic,
                                          size_t magic_length) {
    if (magic_length >= 68 && (memcmp(magic + 60, "BOOKMOBI", 8) == 0 ||
                               memcmp(magic + 60, "TEXtREAd", 8) == 0)) {
        return extension_is(extension, ".azw") || extension_is(extension, ".azw3") ||
                       extension_is(extension, ".azw4") ?
                   MEREADER_TUI_FORMAT_AZW :
                   MEREADER_TUI_FORMAT_MOBI;
    }
    if (magic_length >= 4 && memcmp(magic, "PK\003\004", 4) == 0 && has_epub_mimetype(path)) {
        return MEREADER_TUI_FORMAT_EPUB;
    }
    if (magic_length >= 5 && memcmp(magic, "%PDF-", 5) == 0) {
        return MEREADER_TUI_FORMAT_PDF;
    }
    if ((magic_length >= 7U && memcmp(magic, "Rar!\032\007\000", 7U) == 0) ||
        (magic_length >= 8U && memcmp(magic, "Rar!\032\007\001\000", 8U) == 0) ||
        (magic_length >= 6U && memcmp(magic, "7z\274\257\047\034", 6U) == 0)) {
        return MEREADER_TUI_FORMAT_COMIC;
    }
    if (magic_fb2(magic, magic_length)) {
        return MEREADER_TUI_FORMAT_FB2;
    }
    if ((magic_length >= 8U && memcmp(magic, "\211PNG\r\n\032\n", 8U) == 0) ||
        (magic_length >= 3U && memcmp(magic, "\377\330\377", 3U) == 0) ||
        (magic_length >= 6U &&
         (memcmp(magic, "GIF87a", 6U) == 0 || memcmp(magic, "GIF89a", 6U) == 0)) ||
        (magic_length >= 12U && memcmp(magic, "RIFF", 4U) == 0 &&
         memcmp(magic + 8U, "WEBP", 4U) == 0) ||
        (magic_length >= 2U && memcmp(magic, "BM", 2U) == 0) || magic_svg(magic, magic_length)) {
        return MEREADER_TUI_FORMAT_IMAGE;
    }
    return extension_format(extension);
}

static bool metadata_is_empty(const MereaderTuiMetadata *metadata) {
    return metadata->title == NULL && metadata->author == NULL && metadata->creator == NULL &&
           metadata->description == NULL && metadata->publisher == NULL && metadata->producer == NULL &&
           metadata->date == NULL && metadata->creation_date == NULL && metadata->modification_date == NULL &&
           metadata->language == NULL && metadata->format == NULL && metadata->identifier == NULL &&
           metadata->source == NULL;
}

static bool document_output_is_empty(const MereaderTuiDocument *document) {
    return document->path == NULL && document->instance_id == 0U && document->format == MEREADER_TUI_FORMAT_UNKNOWN &&
           metadata_is_empty(&document->metadata) && document->toc == NULL && document->toc_count == 0 &&
           document->toc_capacity == 0 && document->sections == NULL && document->section_count == 0 &&
            document->section_capacity == 0 && document->blocks == NULL && document->block_count == 0 &&
            document->block_capacity == 0 && document->retained_bytes == 0 && document->backend == NULL &&
            document->default_presentation == MEREADER_TUI_PRESENTATION_DEFAULT && document->ops == NULL;
}

typedef struct MereaderTuiImageProbe {
    const char *uri;
    int width;
    int height;
    bool broken;
} MereaderTuiImageProbe;

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
        if (payload_length > MEREADER_TUI_DATA_URI_MAX) {
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

    if (payload_length > MEREADER_TUI_DATA_URI_MAX * 2U) {
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

static bool image_probe_size(const MereaderTuiDocument *document, const char *uri, size_t *size) {
    if (strlen(uri) >= 5U && ascii_case_equal_n(uri, 5U, "data:")) {
        return data_uri_probe_size(uri, size);
    }
    return document->ops != NULL && document->ops->resource_size != NULL &&
           document->ops->resource_size(document, uri, size);
}

void mereader_tui_document_probe_images(MereaderTuiDocument *document, bool enabled) {
    if (document == NULL || (document->block_count > 0U && document->blocks == NULL)) {
        return;
    }
    size_t image_count = 0U;
    for (size_t index = 0U; index < document->block_count; ++index) {
        MereaderTuiBlock *block = &document->blocks[index];
        if (block->kind != MEREADER_TUI_BLOCK_IMAGE || block->value.image.page_index >= 0) {
            continue;
        }
        ++image_count;
        MereaderTuiImageBlock *image = &block->value.image;
        image->intrinsic_width = 0;
        image->intrinsic_height = 0;
        image->broken = true;
    }
    if (!enabled || image_count == 0U) {
        return;
    }

    size_t probe_capacity = image_count;
    if (probe_capacity > MEREADER_TUI_GRAPHICS_MAX_PROBES) {
        probe_capacity = MEREADER_TUI_GRAPHICS_MAX_PROBES;
    }
    MereaderTuiImageProbe *probes = calloc(probe_capacity, sizeof(*probes));
    if (probes == NULL) {
        return;
    }
    size_t probe_count = 0U;
    size_t probe_bytes = 0U;
    for (size_t index = 0U; index < document->block_count; ++index) {
        MereaderTuiBlock *block = &document->blocks[index];
        if (block->kind != MEREADER_TUI_BLOCK_IMAGE || block->value.image.page_index >= 0 ||
            block->value.image.uri == NULL) {
            continue;
        }
        MereaderTuiImageBlock *image = &block->value.image;
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

        MereaderTuiError ignored = {0};
        MereaderTuiResource resource = {0};
        size_t resource_size = 0U;
        if (image_probe_size(document, image->uri, &resource_size) &&
            resource_size <= MEREADER_TUI_GRAPHICS_MAX_INPUT_BYTES &&
            resource_size <= MEREADER_TUI_GRAPHICS_MAX_PROBE_BYTES - probe_bytes) {
            probe_bytes += resource_size;
            if (mereader_tui_document_load_resource(document, image->uri, &resource, &ignored)) {
                if (resource.length <= resource_size &&
                    mereader_tui_graphics_probe_resource(&resource, &image->intrinsic_width,
                                                 &image->intrinsic_height, &ignored)) {
                    image->broken = false;
                } else if (resource.length > resource_size) {
                    /* The backend violated the size established before loading. */
                    probe_bytes = MEREADER_TUI_GRAPHICS_MAX_PROBE_BYTES;
                }
            }
        }
        mereader_tui_resource_free(&resource);
        probes[probe_count++] = (MereaderTuiImageProbe) {
            .uri = image->uri,
            .width = image->intrinsic_width,
            .height = image->intrinsic_height,
            .broken = image->broken,
        };
    }
    free(probes);
}

bool mereader_tui_document_open(MereaderTuiDocument *document, const char *path, MereaderTuiError *error) {
    if (error != NULL) {
        mereader_tui_error_clear(error);
    }
    if (document == NULL || path == NULL || path[0] == '\0') {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document output and path are required");
        return false;
    }
    if (!document_output_is_empty(document)) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document output must be zero-initialized");
        return false;
    }

    MereaderTuiDocument opened = {0};
    opened.path = mereader_tui_realpath(path, error);
    if (opened.path == NULL) {
        return false;
    }

    unsigned char magic[1024] = {0};
    size_t magic_length = 0;
    struct stat detected_identity;
    if (!read_magic(opened.path, magic, sizeof(magic), &magic_length, &detected_identity, error)) {
        mereader_tui_document_close(&opened);
        return false;
    }
    const char *extension = mereader_tui_path_extension(opened.path);
    opened.format = detect_format(opened.path, extension, magic, magic_length);

    bool success = false;
    if (opened.format == MEREADER_TUI_FORMAT_EPUB) {
        success = mereader_tui_epub_open(&opened, opened.path, NULL, error);
    } else if (opened.format == MEREADER_TUI_FORMAT_MOBI || opened.format == MEREADER_TUI_FORMAT_AZW) {
        success = mereader_tui_mobi_open(&opened, opened.path, error);
    } else if (opened.format == MEREADER_TUI_FORMAT_PDF) {
        success = mereader_tui_pdf_open(&opened, opened.path, error);
    } else if (opened.format == MEREADER_TUI_FORMAT_IMAGE) {
        success = mereader_tui_image_open(&opened, opened.path, &detected_identity, error);
    } else if (opened.format == MEREADER_TUI_FORMAT_COMIC) {
        success = mereader_tui_comic_open(&opened, opened.path, &detected_identity, error);
    } else if (opened.format == MEREADER_TUI_FORMAT_TEXT) {
        success = mereader_tui_text_open(&opened, opened.path, &detected_identity, error);
    } else if (opened.format == MEREADER_TUI_FORMAT_FB2) {
        success = mereader_tui_fb2_open(&opened, opened.path, &detected_identity, error);
    } else {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_UNSUPPORTED, "unsupported document format: %s", path);
    }

    if (!success) {
        mereader_tui_document_close(&opened);
        return false;
    }
    opened.instance_id = next_document_instance_id();
    *document = opened;
    return true;
}

void mereader_tui_document_close(MereaderTuiDocument *document) {
    if (document == NULL) {
        return;
    }
    if (document->ops != NULL && document->ops->close != NULL) {
        document->ops->close(document);
    }
    for (size_t index = 0; index < document->block_count; index++) {
        mereader_tui_document_block_free(&document->blocks[index]);
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

const char *mereader_tui_document_format_name(MereaderTuiDocumentFormat format) {
    switch (format) {
        case MEREADER_TUI_FORMAT_EPUB:
            return "EPUB";
        case MEREADER_TUI_FORMAT_MOBI:
            return "MOBI";
        case MEREADER_TUI_FORMAT_AZW:
            return "AZW";
        case MEREADER_TUI_FORMAT_PDF:
            return "PDF";
        case MEREADER_TUI_FORMAT_IMAGE:
            return "image";
        case MEREADER_TUI_FORMAT_COMIC:
            return "comic archive";
        case MEREADER_TUI_FORMAT_TEXT:
            return "text";
        case MEREADER_TUI_FORMAT_FB2:
            return "FB2";
        case MEREADER_TUI_FORMAT_UNKNOWN:
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

bool mereader_tui_document_index_toc_sections(MereaderTuiDocument *document, MereaderTuiError *error) {
    if (document == NULL) {
        mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT, "document is required");
        return false;
    }
    for (size_t index = 0; index < document->toc_count; index++) {
        document->toc[index].section_index = SIZE_MAX;
    }
    if (document->section_count == 0 || document->toc_count == 0) {
        return true;
    }
    SectionPathIndex *sections = mereader_tui_reallocarray(NULL, document->section_count, sizeof(*sections), error);
    if (sections == NULL) {
        return false;
    }
    for (size_t index = 0; index < document->section_count; index++) {
        if (document->sections[index].id == NULL) {
            free(sections);
            mereader_tui_error_set(error, MEREADER_TUI_ERROR_CORRUPT, "document section has no id");
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

const MereaderTuiTocEntry *mereader_tui_document_current_toc(const MereaderTuiDocument *document, const char *target) {
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
    const MereaderTuiTocEntry *current = NULL;
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
