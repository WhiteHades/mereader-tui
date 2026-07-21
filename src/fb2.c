#include "baca/document_backend.h"
#include "baca/graphics.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <glib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <libxml/parser.h>
#include <libxml/tree.h>

enum {
    BACA_FB2_MAX_BINARIES = 10000,
    BACA_FB2_MAX_XML_DEPTH = 256,
};

#define BACA_FB2_MAX_INPUT_BYTES (64U * 1024U * 1024U)
#define BACA_FB2_MAX_HTML_BYTES (64U * 1024U * 1024U)
#define BACA_FB2_MAX_FIELD_BYTES (1024U * 1024U)
#define BACA_FB2_MAX_BINARY_ID_BYTES (8U * 1024U * 1024U)
#define BACA_FB2_MAX_TOTAL_BINARY_BYTES (1024ULL * 1024ULL * 1024ULL)
#define BACA_FB2_SECTION_ID "fb2/document"
#define BACA_FB2_BINARY_PREFIX "fb2://binary/"

typedef struct BacaFb2Binary {
    char *id;
    const char *mime_type;
    const char *extension;
    xmlNode *node;
    size_t size;
} BacaFb2Binary;

typedef struct BacaFb2Section {
    xmlNode *node;
    char *label;
    char *anchor;
    unsigned depth;
} BacaFb2Section;

typedef struct BacaFb2Backend {
    xmlDocPtr xml;
    BacaFb2Binary *binaries;
    size_t binary_count;
    size_t binary_capacity;
    size_t binary_id_bytes;
    uint64_t binary_bytes;
    BacaFb2Section *sections;
    size_t section_count;
    size_t section_capacity;
} BacaFb2Backend;

typedef struct BacaFb2Html {
    const BacaFb2Backend *backend;
    BacaString output;
    size_t section_index;
    unsigned section_depth;
    BacaError *error;
} BacaFb2Html;

static bool fb2_node_is(const xmlNode *node, const char *name) {
    return node != NULL && node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, BAD_CAST name) == 0;
}

static xmlNode *fb2_child(xmlNode *node, const char *name) {
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        if (fb2_node_is(child, name)) {
            return child;
        }
    }
    return NULL;
}

static xmlChar *fb2_attribute(const xmlNode *node, const char *name) {
    for (const xmlAttr *attribute = node == NULL ? NULL : node->properties; attribute != NULL;
         attribute = attribute->next) {
        if (xmlStrcmp(attribute->name, BAD_CAST name) == 0) {
            return xmlNodeListGetString(node->doc, attribute->children, 1);
        }
    }
    return NULL;
}

static bool fb2_xml_space(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f';
}

static char *fb2_normalize_value(const char *value, size_t length, const char *field, BacaError *error) {
    if (length > BACA_FB2_MAX_FIELD_BYTES) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 %s exceeds the supported size limit", field);
        return NULL;
    }
    BacaString normalized = {0};
    bool pending_space = false;
    for (size_t index = 0U; index < length; ++index) {
        if (fb2_xml_space((unsigned char)value[index])) {
            pending_space = normalized.length > 0U;
            continue;
        }
        if (pending_space && !baca_string_append_char(&normalized, ' ', error)) {
            baca_string_free(&normalized);
            return NULL;
        }
        pending_space = false;
        if (!baca_string_append_char(&normalized, value[index], error)) {
            baca_string_free(&normalized);
            return NULL;
        }
    }
    return baca_string_take(&normalized);
}

static char *fb2_node_text(xmlNode *node, const char *field, BacaError *error) {
    if (node == NULL) {
        return NULL;
    }
    xmlChar *content = xmlNodeGetContent(node);
    if (content == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not read FB2 %s", field);
        return NULL;
    }
    const size_t length = strlen((const char *)content);
    char *text = fb2_normalize_value((const char *)content, length, field, error);
    xmlFree(content);
    return text;
}

static char *fb2_date_text(xmlNode *node, const char *field, BacaError *error) {
    xmlChar *value = fb2_attribute(node, "value");
    char *date = NULL;
    if (value != NULL && value[0] != '\0') {
        date = fb2_normalize_value((const char *)value, strlen((const char *)value), field, error);
    }
    xmlFree(value);
    return date != NULL || baca_error_is_set(error) ? date : fb2_node_text(node, field, error);
}

static bool fb2_append_name_part(BacaString *name, xmlNode *person, const char *part, BacaError *error) {
    char *value = fb2_node_text(fb2_child(person, part), "author name", error);
    if (value == NULL) {
        return !baca_error_is_set(error);
    }
    bool success =
        (name->length == 0U || baca_string_append_char(name, ' ', error)) && baca_string_append(name, value, error);
    free(value);
    return success;
}

static char *fb2_person_name(xmlNode *person, BacaError *error) {
    BacaString name = {0};
    if (!fb2_append_name_part(&name, person, "first-name", error) ||
        !fb2_append_name_part(&name, person, "middle-name", error) ||
        !fb2_append_name_part(&name, person, "last-name", error)) {
        baca_string_free(&name);
        return NULL;
    }
    if (name.length == 0U) {
        char *nickname = fb2_node_text(fb2_child(person, "nickname"), "author nickname", error);
        if (nickname != NULL && !baca_string_append(&name, nickname, error)) {
            free(nickname);
            baca_string_free(&name);
            return NULL;
        }
        free(nickname);
    }
    if (name.length > BACA_FB2_MAX_FIELD_BYTES) {
        baca_string_free(&name);
        baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 author name exceeds the supported size limit");
        return NULL;
    }
    return baca_string_take(&name);
}

static char *fb2_authors(xmlNode *parent, BacaError *error) {
    BacaString authors = {0};
    for (xmlNode *node = parent == NULL ? NULL : parent->children; node != NULL; node = node->next) {
        if (!fb2_node_is(node, "author")) {
            continue;
        }
        char *name = fb2_person_name(node, error);
        if (name == NULL) {
            if (baca_error_is_set(error)) {
                baca_string_free(&authors);
                return NULL;
            }
            continue;
        }
        bool success = (authors.length == 0U || baca_string_append(&authors, ", ", error)) &&
                       baca_string_append(&authors, name, error);
        free(name);
        if (!success || authors.length > BACA_FB2_MAX_FIELD_BYTES) {
            baca_string_free(&authors);
            if (!baca_error_is_set(error)) {
                baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 authors exceed the supported size limit");
            }
            return NULL;
        }
    }
    return baca_string_take(&authors);
}

static bool fb2_metadata(BacaDocument *document, xmlNode *root, const char *path, BacaError *error) {
    xmlNode *description = fb2_child(root, "description");
    xmlNode *title_info = fb2_child(description, "title-info");
    xmlNode *document_info = fb2_child(description, "document-info");
    xmlNode *publish_info = fb2_child(description, "publish-info");
    document->metadata.title = fb2_node_text(fb2_child(title_info, "book-title"), "book title", error);
    if (document->metadata.title == NULL && !baca_error_is_set(error)) {
        document->metadata.title = baca_path_basename(path, error);
    }
    document->metadata.author = fb2_authors(title_info, error);
    document->metadata.creator = fb2_authors(document_info, error);
    document->metadata.description = fb2_node_text(fb2_child(title_info, "annotation"), "annotation", error);
    document->metadata.publisher = fb2_node_text(fb2_child(publish_info, "publisher"), "publisher", error);
    document->metadata.producer = fb2_node_text(fb2_child(document_info, "program-used"), "program used", error);
    document->metadata.date = fb2_date_text(fb2_child(title_info, "date"), "book date", error);
    document->metadata.creation_date = fb2_date_text(fb2_child(document_info, "date"), "document date", error);
    document->metadata.language = fb2_node_text(fb2_child(title_info, "lang"), "language", error);
    document->metadata.identifier = fb2_node_text(fb2_child(document_info, "id"), "identifier", error);
    document->metadata.source = fb2_node_text(fb2_child(document_info, "src-url"), "source URL", error);
    document->metadata.format = baca_strdup("FictionBook 2", error);
    return !baca_error_is_set(error) && document->metadata.title != NULL && document->metadata.format != NULL &&
           baca_document_account_metadata(document, error);
}

static int fb2_base64_value(unsigned char value) {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    return value == '+' ? 62 : value == '/' ? 63 : -1;
}

static bool fb2_base64_size(xmlNode *node, size_t *size, BacaError *error) {
    xmlChar *content = xmlNodeGetContent(node);
    if (content == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not read FB2 binary data");
        return false;
    }
    size_t significant = 0U;
    size_t padding = 0U;
    bool padded = false;
    bool valid = true;
    for (const unsigned char *cursor = content; *cursor != '\0'; ++cursor) {
        if (fb2_xml_space(*cursor)) {
            continue;
        }
        if (*cursor == '=') {
            padded = true;
            ++padding;
        } else if (padded || fb2_base64_value(*cursor) < 0) {
            valid = false;
            break;
        }
        ++significant;
    }
    if (!valid || significant == 0U || significant % 4U != 0U || padding > 2U) {
        xmlFree(content);
        baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 binary contains invalid base64 data");
        return false;
    }
    *size = (significant / 4U) * 3U - padding;
    xmlFree(content);
    return true;
}

static bool fb2_image_type(const char *value, const char **mime_type, const char **extension) {
    static const struct {
        const char *mime_type;
        const char *extension;
    } types[] = {
        {.mime_type = "image/png", .extension = ".png"},     {.mime_type = "image/jpeg", .extension = ".jpg"},
        {.mime_type = "image/jpg", .extension = ".jpg"},     {.mime_type = "image/gif", .extension = ".gif"},
        {.mime_type = "image/webp", .extension = ".webp"},   {.mime_type = "image/bmp", .extension = ".bmp"},
        {.mime_type = "image/svg+xml", .extension = ".svg"},
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(types); ++index) {
        if (baca_casecmp(value, types[index].mime_type) == 0) {
            *mime_type = types[index].mime_type;
            *extension = types[index].extension;
            return true;
        }
    }
    return false;
}

static int fb2_binary_compare(const void *left_pointer, const void *right_pointer) {
    const BacaFb2Binary *left = left_pointer;
    const BacaFb2Binary *right = right_pointer;
    return strcmp(left->id, right->id);
}

static bool fb2_collect_binaries(BacaFb2Backend *backend, xmlNode *root, BacaError *error) {
    for (xmlNode *node = root->children; node != NULL; node = node->next) {
        if (!fb2_node_is(node, "binary")) {
            continue;
        }
        xmlChar *id_value = fb2_attribute(node, "id");
        xmlChar *type_value = fb2_attribute(node, "content-type");
        const char *mime_type = NULL;
        const char *extension = NULL;
        const bool supported = id_value != NULL && id_value[0] != '\0' && type_value != NULL &&
                               fb2_image_type((const char *)type_value, &mime_type, &extension);
        if (!supported) {
            xmlFree(id_value);
            xmlFree(type_value);
            continue;
        }
        if (backend->binary_count >= BACA_FB2_MAX_BINARIES) {
            xmlFree(id_value);
            xmlFree(type_value);
            baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 contains more than %d supported images",
                           BACA_FB2_MAX_BINARIES);
            return false;
        }
        const size_t id_length = strlen((const char *)id_value);
        size_t decoded_size = 0U;
        if (id_length + 1U > BACA_FB2_MAX_BINARY_ID_BYTES - backend->binary_id_bytes ||
            !fb2_base64_size(node, &decoded_size, error) || decoded_size > BACA_GRAPHICS_MAX_INPUT_BYTES ||
            (uint64_t)decoded_size > BACA_FB2_MAX_TOTAL_BINARY_BYTES - backend->binary_bytes) {
            xmlFree(id_value);
            xmlFree(type_value);
            if (!baca_error_is_set(error)) {
                baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 embedded images exceed the supported size limit");
            }
            return false;
        }
        BacaError reserve_error = {0};
        BacaFb2Binary *binaries =
            baca_array_reserve(backend->binaries, &backend->binary_capacity, sizeof(*backend->binaries),
                               backend->binary_count + 1U, &reserve_error);
        if (baca_error_is_set(&reserve_error)) {
            xmlFree(id_value);
            xmlFree(type_value);
            if (error != NULL) {
                *error = reserve_error;
            }
            return false;
        }
        backend->binaries = binaries;
        char *id = baca_strdup((const char *)id_value, error);
        xmlFree(id_value);
        xmlFree(type_value);
        if (id == NULL) {
            return false;
        }
        backend->binaries[backend->binary_count++] = (BacaFb2Binary){
            .id = id,
            .mime_type = mime_type,
            .extension = extension,
            .node = node,
            .size = decoded_size,
        };
        backend->binary_id_bytes += id_length + 1U;
        backend->binary_bytes += (uint64_t)decoded_size;
    }
    if (backend->binary_count > 1U) {
        qsort(backend->binaries, backend->binary_count, sizeof(*backend->binaries), fb2_binary_compare);
    }
    for (size_t index = 1U; index < backend->binary_count; ++index) {
        if (strcmp(backend->binaries[index - 1U].id, backend->binaries[index].id) == 0) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 contains duplicate binary id: %s",
                           backend->binaries[index].id);
            return false;
        }
    }
    return true;
}

static const BacaFb2Binary *fb2_binary_by_id(const BacaFb2Backend *backend, const char *id, size_t *index) {
    size_t first = 0U;
    size_t last = backend->binary_count;
    while (first < last) {
        const size_t middle = first + (last - first) / 2U;
        const int comparison = strcmp(id, backend->binaries[middle].id);
        if (comparison == 0) {
            if (index != NULL) {
                *index = middle;
            }
            return &backend->binaries[middle];
        }
        if (comparison < 0) {
            last = middle;
        } else {
            first = middle + 1U;
        }
    }
    return NULL;
}

static bool fb2_binary_uri(size_t index, const char *extension, char output[64]) {
    const int length = snprintf(output, 64U, BACA_FB2_BINARY_PREFIX "%zu%s", index, extension);
    return length > 0 && length < 64;
}

static const BacaFb2Binary *fb2_binary_for_uri(const BacaFb2Backend *backend, const char *uri) {
    const size_t prefix_length = sizeof(BACA_FB2_BINARY_PREFIX) - 1U;
    if (uri == NULL || strncmp(uri, BACA_FB2_BINARY_PREFIX, prefix_length) != 0) {
        return NULL;
    }
    errno = 0;
    char *end = NULL;
    const uintmax_t parsed = strtoumax(uri + prefix_length, &end, 10);
    if (errno == ERANGE || end == uri + prefix_length || parsed > SIZE_MAX || (size_t)parsed >= backend->binary_count) {
        return NULL;
    }
    const BacaFb2Binary *binary = &backend->binaries[(size_t)parsed];
    char expected[64] = {0};
    return fb2_binary_uri((size_t)parsed, binary->extension, expected) && strcmp(uri, expected) == 0 ? binary : NULL;
}

static bool fb2_resource_size(const BacaDocument *document, const char *uri, size_t *size) {
    if (document == NULL || document->backend == NULL || size == NULL) {
        return false;
    }
    const BacaFb2Binary *binary = fb2_binary_for_uri(document->backend, uri);
    if (binary == NULL) {
        return false;
    }
    *size = binary->size;
    return true;
}

static bool fb2_load_resource(BacaDocument *document, const char *uri, BacaResource *resource, BacaError *error) {
    BacaFb2Backend *backend = document->backend;
    const BacaFb2Binary *binary = backend == NULL ? NULL : fb2_binary_for_uri(backend, uri);
    if (binary == NULL) {
        baca_error_set(error, BACA_ERROR_NOT_FOUND, "FB2 image resource not found: %s", uri);
        return false;
    }
    xmlChar *content = xmlNodeGetContent(binary->node);
    if (content == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not read FB2 image data");
        return false;
    }
    gsize decoded_length = 0U;
    guchar *decoded = g_base64_decode((const gchar *)content, &decoded_length);
    xmlFree(content);
    if (decoded == NULL || decoded_length != binary->size) {
        g_free(decoded);
        baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 image base64 changed after validation");
        return false;
    }
    unsigned char *data = malloc(binary->size == 0U ? 1U : binary->size);
    char *mime_type = baca_strdup(binary->mime_type, error);
    if (data == NULL || mime_type == NULL) {
        free(data);
        free(mime_type);
        g_free(decoded);
        if (!baca_error_is_set(error)) {
            baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate FB2 image resource");
        }
        return false;
    }
    memcpy(data, decoded, binary->size);
    g_free(decoded);
    *resource = (BacaResource){.data = data, .length = binary->size, .mime_type = mime_type};
    return true;
}

static void fb2_backend_destroy(BacaFb2Backend *backend) {
    if (backend == NULL) {
        return;
    }
    for (size_t index = 0U; index < backend->binary_count; ++index) {
        free(backend->binaries[index].id);
    }
    for (size_t index = 0U; index < backend->section_count; ++index) {
        free(backend->sections[index].label);
        free(backend->sections[index].anchor);
    }
    free(backend->binaries);
    free(backend->sections);
    xmlFreeDoc(backend->xml);
    free(backend);
}

static void fb2_close(BacaDocument *document) {
    fb2_backend_destroy(document->backend);
    document->backend = NULL;
}

static const BacaDocumentOps fb2_document_ops = {
    .load_resource = fb2_load_resource,
    .resource_size = fb2_resource_size,
    .close = fb2_close,
};

static bool fb2_collect_sections(BacaFb2Backend *backend, xmlNode *node, unsigned depth, unsigned xml_depth,
                                 BacaError *error) {
    if (xml_depth > BACA_FB2_MAX_XML_DEPTH) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 sections are nested too deeply");
        return false;
    }
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        if (!fb2_node_is(child, "section")) {
            if (child->type == XML_ELEMENT_NODE &&
                !fb2_collect_sections(backend, child, depth, xml_depth + 1U, error)) {
                return false;
            }
            continue;
        }
        char *label = fb2_node_text(fb2_child(child, "title"), "section title", error);
        if (baca_error_is_set(error)) {
            return false;
        }
        char anchor_buffer[64] = {0};
        const int anchor_length =
            snprintf(anchor_buffer, sizeof(anchor_buffer), "__baca_fb2_section_%zu", backend->section_count);
        if (anchor_length <= 0 || (size_t)anchor_length >= sizeof(anchor_buffer)) {
            free(label);
            baca_error_set(error, BACA_ERROR_INTERNAL, "could not format FB2 section anchor");
            return false;
        }
        if (backend->section_count >= BACA_DOCUMENT_MAX_TOC_ENTRIES) {
            free(label);
            baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 contains too many sections");
            return false;
        }
        BacaError reserve_error = {0};
        BacaFb2Section *sections =
            baca_array_reserve(backend->sections, &backend->section_capacity, sizeof(*backend->sections),
                               backend->section_count + 1U, &reserve_error);
        if (baca_error_is_set(&reserve_error)) {
            free(label);
            if (error != NULL) {
                *error = reserve_error;
            }
            return false;
        }
        backend->sections = sections;
        char *anchor = baca_strdup(anchor_buffer, error);
        if (anchor == NULL) {
            free(label);
            return false;
        }
        backend->sections[backend->section_count++] = (BacaFb2Section){
            .node = child,
            .label = label,
            .anchor = anchor,
            .depth = depth,
        };
        if (!fb2_collect_sections(backend, child, depth + 1U, xml_depth + 1U, error)) {
            return false;
        }
    }
    return true;
}

static bool fb2_html_append_n(BacaFb2Html *context, const char *value, size_t length) {
    if (context->output.length > BACA_FB2_MAX_HTML_BYTES || length > BACA_FB2_MAX_HTML_BYTES - context->output.length) {
        baca_error_set(context->error, BACA_ERROR_CORRUPT, "FB2 rendered markup exceeds the supported size limit");
        return false;
    }
    return baca_string_append_n(&context->output, value, length, context->error);
}

static bool fb2_html_append(BacaFb2Html *context, const char *value) {
    return fb2_html_append_n(context, value, strlen(value));
}

static bool fb2_html_escape(BacaFb2Html *context, const char *value, size_t length, bool attribute) {
    size_t start = 0U;
    for (size_t index = 0U; index < length; ++index) {
        const char *replacement = NULL;
        if (value[index] == '&') {
            replacement = "&amp;";
        } else if (value[index] == '<') {
            replacement = "&lt;";
        } else if (value[index] == '>') {
            replacement = "&gt;";
        } else if (attribute && value[index] == '"') {
            replacement = "&quot;";
        }
        if (replacement == NULL) {
            continue;
        }
        if ((index > start && !fb2_html_append_n(context, value + start, index - start)) ||
            !fb2_html_append(context, replacement)) {
            return false;
        }
        start = index + 1U;
    }
    if (length > start && !fb2_html_append_n(context, value + start, length - start)) {
        return false;
    }
    return true;
}

static bool fb2_html_attribute(BacaFb2Html *context, const char *name, const xmlChar *value) {
    return value == NULL ||
           (fb2_html_append(context, " ") && fb2_html_append(context, name) && fb2_html_append(context, "=\"") &&
            fb2_html_escape(context, (const char *)value, strlen((const char *)value), true) &&
            fb2_html_append(context, "\""));
}

static bool fb2_html_node(BacaFb2Html *context, xmlNode *node, unsigned xml_depth);

static bool fb2_html_children(BacaFb2Html *context, xmlNode *node, unsigned xml_depth) {
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        if (!fb2_html_node(context, child, xml_depth + 1U)) {
            return false;
        }
    }
    return true;
}

static bool fb2_html_wrapped(BacaFb2Html *context, xmlNode *node, const char *tag, unsigned xml_depth) {
    xmlChar *id = fb2_attribute(node, "id");
    bool success = fb2_html_append(context, "<") && fb2_html_append(context, tag) &&
                   fb2_html_attribute(context, "id", id) && fb2_html_append(context, ">") &&
                   fb2_html_children(context, node, xml_depth) && fb2_html_append(context, "</") &&
                   fb2_html_append(context, tag) && fb2_html_append(context, ">");
    xmlFree(id);
    return success;
}

static bool fb2_html_title(BacaFb2Html *context, xmlNode *node, unsigned level, unsigned xml_depth) {
    char tag[4] = {'h', (char)('0' + (level > 6U ? 6U : level)), '\0', '\0'};
    xmlChar *id = fb2_attribute(node, "id");
    bool success = fb2_html_append(context, "<") && fb2_html_append(context, tag) &&
                   fb2_html_attribute(context, "id", id) && fb2_html_append(context, ">");
    bool wrote_paragraph = false;
    for (xmlNode *child = node->children; child != NULL && success; child = child->next) {
        if (fb2_node_is(child, "p")) {
            if (wrote_paragraph) {
                success = fb2_html_append(context, "<br>");
            }
            if (success) {
                success = fb2_html_children(context, child, xml_depth + 1U);
            }
            wrote_paragraph = true;
        } else {
            success = fb2_html_node(context, child, xml_depth + 1U);
        }
    }
    if (success) {
        success = fb2_html_append(context, "</") && fb2_html_append(context, tag) && fb2_html_append(context, ">");
    }
    xmlFree(id);
    return success;
}

static bool fb2_html_image(BacaFb2Html *context, xmlNode *node) {
    xmlChar *href = fb2_attribute(node, "href");
    xmlChar *alt = fb2_attribute(node, "alt");
    char uri[64] = "fb2://missing";
    if (href != NULL && href[0] == '#') {
        size_t index = 0U;
        const BacaFb2Binary *binary = fb2_binary_by_id(context->backend, (const char *)href + 1U, &index);
        if (binary != NULL && !fb2_binary_uri(index, binary->extension, uri)) {
            xmlFree(href);
            xmlFree(alt);
            baca_error_set(context->error, BACA_ERROR_INTERNAL, "could not format FB2 image URI");
            return false;
        }
    }
    bool success = fb2_html_append(context, "<img src=\"") && fb2_html_escape(context, uri, strlen(uri), true) &&
                   fb2_html_append(context, "\"") && fb2_html_attribute(context, "alt", alt) &&
                   fb2_html_append(context, ">");
    xmlFree(href);
    xmlFree(alt);
    return success;
}

static bool fb2_html_link(BacaFb2Html *context, xmlNode *node, unsigned xml_depth) {
    xmlChar *href = fb2_attribute(node, "href");
    xmlChar *id = fb2_attribute(node, "id");
    bool success = fb2_html_append(context, "<a") && fb2_html_attribute(context, "href", href) &&
                   fb2_html_attribute(context, "id", id) && fb2_html_append(context, ">") &&
                   fb2_html_children(context, node, xml_depth) && fb2_html_append(context, "</a>");
    xmlFree(href);
    xmlFree(id);
    return success;
}

static bool fb2_html_section(BacaFb2Html *context, xmlNode *node, unsigned xml_depth) {
    if (context->section_index >= context->backend->section_count ||
        context->backend->sections[context->section_index].node != node) {
        baca_error_set(context->error, BACA_ERROR_INTERNAL, "FB2 section traversal changed");
        return false;
    }
    const BacaFb2Section *section = &context->backend->sections[context->section_index++];
    xmlChar *original_id = fb2_attribute(node, "id");
    bool success = fb2_html_append(context, "<section id=\"") &&
                   fb2_html_escape(context, section->anchor, strlen(section->anchor), true) &&
                   fb2_html_append(context, "\">");
    if (success && original_id != NULL && original_id[0] != '\0') {
        success = fb2_html_append(context, "<a id=\"") &&
                  fb2_html_escape(context, (const char *)original_id, strlen((const char *)original_id), true) &&
                  fb2_html_append(context, "\"></a>");
    }
    ++context->section_depth;
    if (success) {
        success = fb2_html_children(context, node, xml_depth);
    }
    --context->section_depth;
    if (success) {
        success = fb2_html_append(context, "</section>");
    }
    xmlFree(original_id);
    return success;
}

static bool fb2_html_node(BacaFb2Html *context, xmlNode *node, unsigned xml_depth) {
    if (xml_depth > BACA_FB2_MAX_XML_DEPTH) {
        baca_error_set(context->error, BACA_ERROR_CORRUPT, "FB2 markup is nested too deeply");
        return false;
    }
    if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
        const char *text = (const char *)node->content;
        return text == NULL || fb2_html_escape(context, text, strlen(text), false);
    }
    if (node->type != XML_ELEMENT_NODE) {
        return true;
    }
    if (fb2_node_is(node, "section")) {
        return fb2_html_section(context, node, xml_depth);
    }
    if (fb2_node_is(node, "title")) {
        return fb2_html_title(context, node, context->section_depth + 1U, xml_depth);
    }
    if (fb2_node_is(node, "subtitle")) {
        return fb2_html_title(context, node, context->section_depth + 2U, xml_depth);
    }
    if (fb2_node_is(node, "image")) {
        return fb2_html_image(context, node);
    }
    if (fb2_node_is(node, "a")) {
        return fb2_html_link(context, node, xml_depth);
    }
    if (fb2_node_is(node, "empty-line")) {
        return fb2_html_append(context, "<br>");
    }
    if (fb2_node_is(node, "strong")) {
        return fb2_html_wrapped(context, node, "strong", xml_depth);
    }
    if (fb2_node_is(node, "emphasis")) {
        return fb2_html_wrapped(context, node, "em", xml_depth);
    }
    if (fb2_node_is(node, "code")) {
        return fb2_html_wrapped(context, node, "code", xml_depth);
    }
    if (fb2_node_is(node, "p") || fb2_node_is(node, "v") || fb2_node_is(node, "date")) {
        return fb2_html_wrapped(context, node, "p", xml_depth);
    }
    if (fb2_node_is(node, "text-author")) {
        return fb2_html_append(context, "<p><em>") && fb2_html_children(context, node, xml_depth) &&
               fb2_html_append(context, "</em></p>");
    }
    if (fb2_node_is(node, "poem") || fb2_node_is(node, "stanza") || fb2_node_is(node, "epigraph") ||
        fb2_node_is(node, "cite") || fb2_node_is(node, "annotation")) {
        return fb2_html_wrapped(context, node, "div", xml_depth);
    }
    static const char *const table_tags[] = {"table", "tr", "td", "th"};
    for (size_t index = 0U; index < BACA_ARRAY_LEN(table_tags); ++index) {
        if (fb2_node_is(node, table_tags[index])) {
            return fb2_html_wrapped(context, node, table_tags[index], xml_depth);
        }
    }
    xmlChar *id = fb2_attribute(node, "id");
    bool success = true;
    if (id != NULL && id[0] != '\0') {
        success = fb2_html_append(context, "<span id=\"") &&
                  fb2_html_escape(context, (const char *)id, strlen((const char *)id), true) &&
                  fb2_html_append(context, "\">") && fb2_html_children(context, node, xml_depth) &&
                  fb2_html_append(context, "</span>");
    } else {
        success = fb2_html_children(context, node, xml_depth);
    }
    xmlFree(id);
    return success;
}

static bool fb2_build_html(BacaFb2Html *context, xmlNode *root, BacaError *error) {
    if (!fb2_html_append(context, "<html><body>")) {
        return false;
    }
    xmlNode *description = fb2_child(root, "description");
    xmlNode *title_info = fb2_child(description, "title-info");
    xmlNode *cover = fb2_child(title_info, "coverpage");
    if (cover != NULL && !fb2_html_children(context, cover, 0U)) {
        return false;
    }
    size_t body_count = 0U;
    for (xmlNode *body = root->children; body != NULL; body = body->next) {
        if (!fb2_node_is(body, "body")) {
            continue;
        }
        ++body_count;
        if (!fb2_html_append(context, "<div>") || !fb2_html_children(context, body, 0U) ||
            !fb2_html_append(context, "</div>")) {
            return false;
        }
    }
    if (body_count == 0U) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 document contains no body");
        return false;
    }
    return fb2_html_append(context, "</body></html>");
}

static bool fb2_add_toc(BacaDocument *document, const BacaFb2Backend *backend, BacaError *error) {
    for (size_t index = 0U; index < backend->section_count; ++index) {
        const BacaFb2Section *section = &backend->sections[index];
        if (section->label == NULL) {
            continue;
        }
        char target[128] = {0};
        const int length = snprintf(target, sizeof(target), BACA_FB2_SECTION_ID "#%s", section->anchor);
        if (length <= 0 || (size_t)length >= sizeof(target) ||
            !baca_document_add_toc(document, section->label, target, section->depth, error)) {
            if (!baca_error_is_set(error)) {
                baca_error_set(error, BACA_ERROR_INTERNAL, "could not format FB2 TOC target");
            }
            return false;
        }
    }
    return true;
}

bool baca_fb2_open(BacaDocument *document, const char *path, const struct stat *expected_identity, BacaError *error) {
    if (document == NULL || path == NULL || path[0] == '\0' || document->path == NULL ||
        strcmp(path, document->path) != 0 || expected_identity == NULL) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document, canonical FB2 path, and detected identity are required");
        return false;
    }
    BacaBuffer snapshot = {0};
    if (!baca_document_read_snapshot(path, expected_identity, BACA_FB2_MAX_INPUT_BYTES, &snapshot, error)) {
        return false;
    }
    const int options = XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
                        XML_PARSE_COMPACT | XML_PARSE_BIG_LINES | XML_PARSE_NO_XXE | XML_PARSE_NO_SYS_CATALOG;
    xmlResetLastError();
    xmlDocPtr xml = xmlReadMemory((const char *)snapshot.data, (int)snapshot.length, path, NULL, options);
    baca_buffer_free(&snapshot);
    if (xml == NULL) {
        const xmlError *parse_error = xmlGetLastError();
        const char *detail = parse_error == NULL || parse_error->message == NULL ? "invalid XML" : parse_error->message;
        baca_error_set(error, BACA_ERROR_CORRUPT, "could not parse FB2 XML: %.400s", detail);
        return false;
    }
    xmlNode *root = xmlDocGetRootElement(xml);
    if (!fb2_node_is(root, "FictionBook") || xml->extSubset != NULL ||
        (xml->intSubset != NULL && xml->intSubset->children != NULL)) {
        xmlFreeDoc(xml);
        baca_error_set(error, BACA_ERROR_CORRUPT, "FB2 root is invalid or contains a DTD subset");
        return false;
    }
    BacaFb2Backend *backend = calloc(1U, sizeof(*backend));
    if (backend == NULL) {
        xmlFreeDoc(xml);
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate FB2 backend");
        return false;
    }
    backend->xml = xml;
    document->format = BACA_FORMAT_FB2;
    document->backend = backend;
    document->ops = &fb2_document_ops;
    if (!fb2_collect_binaries(backend, root, error) || !fb2_metadata(document, root, path, error)) {
        return false;
    }
    for (xmlNode *body = root->children; body != NULL; body = body->next) {
        if (fb2_node_is(body, "body") && !fb2_collect_sections(backend, body, 0U, 0U, error)) {
            return false;
        }
    }
    BacaFb2Html html = {.backend = backend, .error = error};
    const bool rendered = fb2_build_html(&html, root, error) && html.section_index == backend->section_count &&
                          baca_html_append_section(document, html.output.data == NULL ? "" : html.output.data,
                                                   html.output.length, BACA_FB2_SECTION_ID, error);
    baca_string_free(&html.output);
    if (!rendered) {
        if (!baca_error_is_set(error)) {
            baca_error_set(error, BACA_ERROR_INTERNAL, "FB2 section rendering was incomplete");
        }
        return false;
    }
    return fb2_add_toc(document, backend, error) && baca_document_index_toc_sections(document, error);
}
