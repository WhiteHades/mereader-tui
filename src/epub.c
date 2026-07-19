#include "baca/document_backend.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <zip.h>

#define EPUB_MAX_ENTRIES 100000
#define EPUB_MAX_MEMBER (64U * 1024U * 1024U)
#define EPUB_MAX_MARKUP (32U * 1024U * 1024U)
#define EPUB_MAX_TOTAL (1024ULL * 1024ULL * 1024ULL)
#define EPUB_RATIO_THRESHOLD (1024U * 1024U)
#define EPUB_MAX_COMPRESSION_RATIO 1000U
#define EPUB_MAX_MANIFEST_BYTES (64U * 1024U * 1024U)

#define CONTAINER_NAMESPACE "urn:oasis:names:tc:opendocument:xmlns:container"
#define DC_NAMESPACE "http://purl.org/dc/elements/1.1/"
#define EPUB_NAMESPACE "http://www.idpf.org/2007/ops"
#define NCX_NAMESPACE "http://www.daisy.org/z3986/2005/ncx/"
#define OPF_NAMESPACE "http://www.idpf.org/2007/opf"
#define SVG_NAMESPACE "http://www.w3.org/2000/svg"
#define XHTML_NAMESPACE "http://www.w3.org/1999/xhtml"
#define XML_ENCRYPTION_NAMESPACE "http://www.w3.org/2001/04/xmlenc#"

typedef struct EpubManifestItem {
    char *id;
    char *path;
    char *member_path;
    char *media_type;
    char *properties;
    char *fallback;
} EpubManifestItem;

typedef struct EpubBackend {
    zip_t *archive;
    char *opf_path;
    char *opf_member_path;
    char *cleanup_directory;
    EpubManifestItem *manifest;
    size_t manifest_count;
    size_t manifest_capacity;
    size_t manifest_bytes;
    EpubManifestItem **manifest_by_id;
    EpubManifestItem **manifest_by_path;
} EpubBackend;

static bool epub_load_resource(BacaDocument *document, const char *uri, BacaResource *resource,
                               BacaError *error);
static bool epub_resource_size(const BacaDocument *document, const char *uri, size_t *size);
static void epub_close(BacaDocument *document);

static const BacaDocumentOps EPUB_OPS = {
    .load_resource = epub_load_resource,
    .resource_size = epub_resource_size,
    .render_page = NULL,
    .close = epub_close,
};

static void manifest_item_free(EpubManifestItem *item) {
    free(item->id);
    free(item->path);
    free(item->member_path);
    free(item->media_type);
    free(item->properties);
    free(item->fallback);
    memset(item, 0, sizeof(*item));
}

static void backend_destroy(EpubBackend *backend) {
    if (backend == NULL) {
        return;
    }
    if (backend->archive != NULL) {
        zip_discard(backend->archive);
    }
    for (size_t index = 0; index < backend->manifest_count; index++) {
        manifest_item_free(&backend->manifest[index]);
    }
    free(backend->manifest);
    free(backend->manifest_by_id);
    free(backend->manifest_by_path);
    free(backend->opf_path);
    free(backend->opf_member_path);
    if (backend->cleanup_directory != NULL) {
        BacaError ignored = {0};
        (void) baca_remove_tree(backend->cleanup_directory, &ignored);
    }
    free(backend->cleanup_directory);
    free(backend);
}

static void epub_close(BacaDocument *document) {
    backend_destroy(document->backend);
    document->backend = NULL;
    document->ops = NULL;
}

static bool token_contains(const char *tokens, const char *wanted) {
    if (tokens == NULL) {
        return false;
    }
    size_t wanted_length = strlen(wanted);
    const char *cursor = tokens;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n' ||
               *cursor == '\f') {
            cursor++;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' &&
               *cursor != '\n' && *cursor != '\f') {
            cursor++;
        }
        if ((size_t) (cursor - start) == wanted_length && memcmp(start, wanted, wanted_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool archive_path_safe(const char *path, size_t length, bool allow_trailing_slash) {
    if (length == 0 || path[0] == '/' || path[0] == '\\') {
        return false;
    }
    size_t segment_start = 0;
    for (size_t index = 0; index <= length; index++) {
        unsigned char value = index == length ? '/' : (unsigned char) path[index];
        if (value == '\\' || value < 0x20U || value == 0x7fU) {
            return false;
        }
        if (value != '/') {
            continue;
        }
        size_t segment_length = index - segment_start;
        bool final_directory_slash = allow_trailing_slash && index == length && segment_length == 0;
        if (!final_directory_slash &&
            (segment_length == 0 || (segment_length == 1 && path[segment_start] == '.') ||
             (segment_length == 2 && path[segment_start] == '.' && path[segment_start + 1] == '.'))) {
            return false;
        }
        segment_start = index + 1;
    }
    return true;
}

static bool validate_archive(EpubBackend *backend, BacaError *error) {
    zip_int64_t entry_count = zip_get_num_entries(backend->archive, 0);
    if (entry_count < 0) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "could not inspect EPUB archive: %s",
                       zip_strerror(backend->archive));
        return false;
    }
    if (entry_count > EPUB_MAX_ENTRIES) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB contains too many archive members");
        return false;
    }

    zip_uint64_t total_size = 0;
    for (zip_int64_t index = 0; index < entry_count; index++) {
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(backend->archive, (zip_uint64_t) index, ZIP_FL_ENC_GUESS, &stat) != 0 ||
            (stat.valid & ZIP_STAT_NAME) == 0 || stat.name == NULL) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "could not inspect an EPUB archive member");
            return false;
        }
        size_t name_length = strlen(stat.name);
        bool directory = name_length > 0 && stat.name[name_length - 1] == '/';
        if (!archive_path_safe(stat.name, name_length, directory)) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "unsafe EPUB archive path: %s", stat.name);
            return false;
        }
        if ((stat.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 && stat.encryption_method != ZIP_EM_NONE) {
            baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                           "encrypted EPUB members are not supported; the book may be DRM protected");
            return false;
        }
        if ((stat.valid & ZIP_STAT_SIZE) == 0 || stat.size > EPUB_MAX_MEMBER) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB member exceeds the supported size limit: %s",
                           stat.name);
            return false;
        }
        if (total_size > EPUB_MAX_TOTAL - stat.size) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB uncompressed size exceeds the supported limit");
            return false;
        }
        total_size += stat.size;
        if ((stat.valid & ZIP_STAT_COMP_SIZE) != 0 && stat.size >= EPUB_RATIO_THRESHOLD) {
            if (stat.comp_size == 0 || stat.size / stat.comp_size > EPUB_MAX_COMPRESSION_RATIO) {
                baca_error_set(error, BACA_ERROR_CORRUPT, "suspicious EPUB compression ratio: %s", stat.name);
                return false;
            }
        }
    }
    return true;
}

static bool read_member(EpubBackend *backend, const char *path, size_t maximum, BacaBuffer *output,
                        BacaError *error) {
    zip_int64_t member_index = zip_name_locate(backend->archive, path, ZIP_FL_ENC_GUESS);
    if (member_index < 0) {
        baca_error_set(error, BACA_ERROR_NOT_FOUND, "EPUB member not found: %s", path);
        return false;
    }
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(backend->archive, (zip_uint64_t) member_index, ZIP_FL_ENC_GUESS, &stat) != 0 ||
        (stat.valid & ZIP_STAT_SIZE) == 0) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "could not inspect EPUB member: %s", path);
        return false;
    }
    if (stat.size > maximum || stat.size > (zip_uint64_t) SIZE_MAX - 1U) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB member exceeds the supported size limit: %s", path);
        return false;
    }

    size_t size = (size_t) stat.size;
    unsigned char *data = malloc(size + 1U);
    if (data == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate EPUB member: %s", path);
        return false;
    }
    zip_file_t *member = zip_fopen_index(backend->archive, (zip_uint64_t) member_index, ZIP_FL_ENC_GUESS);
    if (member == NULL) {
        free(data);
        baca_error_set(error, BACA_ERROR_CORRUPT, "could not open EPUB member %s: %s", path,
                       zip_strerror(backend->archive));
        return false;
    }

    size_t offset = 0;
    while (offset < size) {
        zip_int64_t count = zip_fread(member, data + offset, (zip_uint64_t) (size - offset));
        if (count <= 0) {
            zip_fclose(member);
            free(data);
            baca_error_set(error, BACA_ERROR_CORRUPT, "truncated EPUB member: %s", path);
            return false;
        }
        offset += (size_t) count;
    }
    if (zip_fclose(member) != 0) {
        free(data);
        baca_error_set(error, BACA_ERROR_CORRUPT, "could not finish reading EPUB member: %s", path);
        return false;
    }
    data[size] = '\0';
    output->data = data;
    output->length = size;
    output->capacity = size + 1U;
    return true;
}

static char *resolve_reference(const char *base_path, const char *reference, bool allow_external,
                                BacaError *error) {
    if (reference == NULL || reference[0] == '\0') {
        baca_error_set(error, BACA_ERROR_CORRUPT, "invalid empty EPUB reference");
        return NULL;
    }
    return baca_document_resolve_uri(base_path, reference, allow_external, error);
}

static char *member_path_from_reference(const char *base_path, const char *reference, BacaError *error) {
    char *resolved = resolve_reference(base_path, reference, false, error);
    if (resolved == NULL) {
        return NULL;
    }
    resolved[strcspn(resolved, "?#")] = '\0';
    char *member_path = baca_document_uri_path(resolved, error);
    if (member_path == NULL) {
        free(resolved);
        return NULL;
    }
    size_t member_length = strlen(member_path);
    bool safe = archive_path_safe(member_path, member_length, false);
    free(member_path);
    if (!safe) {
        free(resolved);
        baca_error_set(error, BACA_ERROR_CORRUPT, "unsafe EPUB reference: %s", reference);
        return NULL;
    }
    return resolved;
}

static char *archive_member_path(const char *canonical_path, BacaError *error) {
    char *member_path = baca_document_uri_path(canonical_path, error);
    if (member_path == NULL) {
        return NULL;
    }
    if (!archive_path_safe(member_path, strlen(member_path), false)) {
        free(member_path);
        baca_error_set(error, BACA_ERROR_CORRUPT, "unsafe EPUB member path: %s", canonical_path);
        return NULL;
    }
    return member_path;
}

static xmlDocPtr parse_xml(const BacaBuffer *buffer, const char *name, BacaError *error) {
    if (buffer->length > (size_t) INT_MAX) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "XML member is too large: %s", name);
        return NULL;
    }
    int options = XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
                  XML_PARSE_COMPACT | XML_PARSE_BIG_LINES | XML_PARSE_NO_XXE | XML_PARSE_NO_SYS_CATALOG;
    xmlDocPtr document = xmlReadMemory((const char *) buffer->data, (int) buffer->length, name, NULL, options);
    if (document == NULL) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "could not parse EPUB XML member: %s", name);
        return NULL;
    }
    if (document->extSubset != NULL ||
        (document->intSubset != NULL && document->intSubset->children != NULL)) {
        xmlFreeDoc(document);
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB XML member contains a DTD subset: %s", name);
        return NULL;
    }
    return document;
}

static bool xml_node_is_ns(const xmlNode *node, const char *name, const char *namespace_uri) {
    if (node == NULL || node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, BAD_CAST name) != 0) {
        return false;
    }
    const xmlChar *actual_namespace = node->ns == NULL ? NULL : node->ns->href;
    return namespace_uri == NULL ? actual_namespace == NULL :
                                   actual_namespace != NULL &&
                                       xmlStrcmp(actual_namespace, BAD_CAST namespace_uri) == 0;
}

static xmlNode *xml_find_descendant_ns(xmlNode *node, const char *name, const char *namespace_uri) {
    if (xml_node_is_ns(node, name, namespace_uri)) {
        return node;
    }
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        xmlNode *found = xml_find_descendant_ns(child, name, namespace_uri);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static xmlNode *find_direct_child_ns(xmlNode *node, const char *name, const char *namespace_uri) {
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        if (xml_node_is_ns(child, name, namespace_uri)) {
            return child;
        }
    }
    return NULL;
}

static xmlChar *xml_attribute_ns(const xmlNode *node, const char *name, const char *namespace_uri) {
    for (const xmlAttr *attribute = node->properties; attribute != NULL; attribute = attribute->next) {
        const xmlChar *actual_namespace = attribute->ns == NULL ? NULL : attribute->ns->href;
        bool namespace_matches = namespace_uri == NULL ? actual_namespace == NULL :
                                                       actual_namespace != NULL &&
                                                           xmlStrcmp(actual_namespace,
                                                                     BAD_CAST namespace_uri) == 0;
        if (namespace_matches && xmlStrcmp(attribute->name, BAD_CAST name) == 0) {
            return xmlNodeListGetString(node->doc, attribute->children, 1);
        }
    }
    return NULL;
}

static xmlChar *xml_attribute(const xmlNode *node, const char *name) {
    return xml_attribute_ns(node, name, NULL);
}

static char *trimmed_xml_content(xmlNode *node, BacaError *error) {
    xmlChar *content = xmlNodeGetContent(node);
    if (content == NULL) {
        return baca_strdup("", error);
    }
    const char *start = (const char *) content;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n' || *start == '\f') {
        start++;
    }
    const char *end = (const char *) content + strlen((const char *) content);
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n' || end[-1] == '\f')) {
        end--;
    }
    char *result = baca_strndup(start, (size_t) (end - start), error);
    xmlFree(content);
    return result;
}

static bool collapse_label(xmlNode *node, char **label, BacaError *error) {
    xmlChar *content = xmlNodeGetContent(node);
    if (content == NULL) {
        *label = baca_strdup("", error);
        return *label != NULL;
    }
    BacaString normalized = {0};
    bool pending = false;
    const char *cursor = (const char *) content;
    while (*cursor != '\0') {
        unsigned char value = (unsigned char) *cursor;
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f') {
            pending = normalized.length > 0;
            cursor++;
            continue;
        }
        if (pending && !baca_string_append_char(&normalized, ' ', error)) {
            xmlFree(content);
            baca_string_free(&normalized);
            return false;
        }
        pending = false;
        const char *start = cursor;
        while (*cursor != '\0') {
            value = (unsigned char) *cursor;
            if (value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f') {
                break;
            }
            cursor++;
        }
        if (!baca_string_append_n(&normalized, start, (size_t) (cursor - start), error)) {
            xmlFree(content);
            baca_string_free(&normalized);
            return false;
        }
    }
    xmlFree(content);
    *label = baca_string_take(&normalized);
    if (*label == NULL) {
        *label = baca_strdup("", error);
    }
    return *label != NULL;
}

static xmlNode *find_metadata_value(xmlNode *node, const char *name) {
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        if (xml_node_is_ns(child, name, DC_NAMESPACE)) {
            return child;
        }
    }
    return NULL;
}

static bool set_metadata_value(char **field, xmlNode *metadata, const char *name, BacaError *error) {
    xmlNode *node = find_metadata_value(metadata, name);
    if (node == NULL) {
        return true;
    }
    char *value = trimmed_xml_content(node, error);
    if (value == NULL) {
        return false;
    }
    free(*field);
    *field = value;
    return true;
}

static bool parse_metadata(BacaDocument *document, xmlNode *metadata, BacaError *error) {
    return set_metadata_value(&document->metadata.title, metadata, "title", error) &&
           set_metadata_value(&document->metadata.creator, metadata, "creator", error) &&
           set_metadata_value(&document->metadata.description, metadata, "description", error) &&
           set_metadata_value(&document->metadata.publisher, metadata, "publisher", error) &&
           set_metadata_value(&document->metadata.date, metadata, "date", error) &&
           set_metadata_value(&document->metadata.language, metadata, "language", error) &&
           set_metadata_value(&document->metadata.format, metadata, "format", error) &&
           set_metadata_value(&document->metadata.identifier, metadata, "identifier", error) &&
           set_metadata_value(&document->metadata.source, metadata, "source", error);
}

static bool manifest_add(EpubBackend *backend, EpubManifestItem *item, BacaError *error) {
    if (backend->manifest_count >= EPUB_MAX_ENTRIES) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest contains too many items");
        return false;
    }
    const char *const values[] = {
        item->id, item->path, item->member_path, item->media_type, item->properties, item->fallback,
    };
    size_t retained_size = sizeof(*item);
    for (size_t index = 0; index < BACA_ARRAY_LEN(values); index++) {
        if (values[index] == NULL) {
            continue;
        }
        size_t length = strlen(values[index]);
        if (length == SIZE_MAX || length + 1U > SIZE_MAX - retained_size) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest retained size overflow");
            return false;
        }
        retained_size += length + 1U;
    }
    if (backend->manifest_bytes > EPUB_MAX_MANIFEST_BYTES ||
        retained_size > EPUB_MAX_MANIFEST_BYTES - backend->manifest_bytes) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest exceeds the supported size limit");
        return false;
    }
    BacaError reserve_error = {0};
    EpubManifestItem *manifest =
        baca_array_reserve(backend->manifest, &backend->manifest_capacity, sizeof(*backend->manifest),
                           backend->manifest_count + 1, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (error != NULL) {
            *error = reserve_error;
        }
        return false;
    }
    backend->manifest = manifest;
    backend->manifest[backend->manifest_count++] = *item;
    backend->manifest_bytes += retained_size;
    memset(item, 0, sizeof(*item));
    return true;
}

static bool parse_manifest(EpubBackend *backend, xmlNode *manifest, BacaError *error) {
    for (xmlNode *node = manifest->children; node != NULL; node = node->next) {
        if (!xml_node_is_ns(node, "item", OPF_NAMESPACE)) {
            continue;
        }
        xmlChar *id = xml_attribute(node, "id");
        xmlChar *href = xml_attribute(node, "href");
        xmlChar *media_type = xml_attribute(node, "media-type");
        xmlChar *properties = xml_attribute(node, "properties");
        xmlChar *fallback = xml_attribute(node, "fallback");
        bool has_properties = properties != NULL;
        bool has_fallback = fallback != NULL;
        if (id == NULL || href == NULL || media_type == NULL || id[0] == '\0' || href[0] == '\0' ||
            media_type[0] == '\0' || strpbrk((const char *) href, "?#") != NULL) {
            xmlFree(id);
            xmlFree(href);
            xmlFree(media_type);
            xmlFree(properties);
            xmlFree(fallback);
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest item has invalid required attributes");
            return false;
        }
        char *canonical_path = member_path_from_reference(backend->opf_path, (const char *) href, error);
        EpubManifestItem item = {
            .id = baca_strdup((const char *) id, error),
            .path = canonical_path,
            .member_path = canonical_path == NULL ? NULL : archive_member_path(canonical_path, error),
            .media_type = baca_strdup((const char *) media_type, error),
            .properties = properties == NULL ? NULL : baca_strdup((const char *) properties, error),
            .fallback = fallback == NULL ? NULL : baca_strdup((const char *) fallback, error),
        };
        xmlFree(id);
        xmlFree(href);
        xmlFree(media_type);
        xmlFree(properties);
        xmlFree(fallback);
        if (item.id == NULL || item.path == NULL || item.member_path == NULL || item.media_type == NULL ||
            (has_properties && item.properties == NULL) || (has_fallback && item.fallback == NULL) ||
            !manifest_add(backend, &item, error)) {
            manifest_item_free(&item);
            return false;
        }
    }
    if (backend->manifest_count == 0) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest is empty");
        return false;
    }
    return true;
}

static int compare_manifest_id(const void *left, const void *right) {
    const EpubManifestItem *const *left_item = left;
    const EpubManifestItem *const *right_item = right;
    return strcmp((*left_item)->id, (*right_item)->id);
}

static int compare_manifest_path(const void *left, const void *right) {
    const EpubManifestItem *const *left_item = left;
    const EpubManifestItem *const *right_item = right;
    return strcmp((*left_item)->path, (*right_item)->path);
}

static EpubManifestItem *manifest_by_id(EpubBackend *backend, const char *id) {
    size_t first = 0;
    size_t last = backend->manifest_count;
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        int comparison = strcmp(id, backend->manifest_by_id[middle]->id);
        if (comparison > 0) {
            first = middle + 1U;
        } else {
            last = middle;
        }
    }
    return first < backend->manifest_count && strcmp(id, backend->manifest_by_id[first]->id) == 0 ?
               backend->manifest_by_id[first] :
               NULL;
}

static EpubManifestItem *manifest_by_path(EpubBackend *backend, const char *path) {
    size_t first = 0;
    size_t last = backend->manifest_count;
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        int comparison = strcmp(path, backend->manifest_by_path[middle]->path);
        if (comparison > 0) {
            first = middle + 1U;
        } else {
            last = middle;
        }
    }
    return first < backend->manifest_count && strcmp(path, backend->manifest_by_path[first]->path) == 0 ?
               backend->manifest_by_path[first] :
               NULL;
}

static bool index_manifest(EpubBackend *backend, BacaError *error) {
    backend->manifest_by_id = baca_reallocarray(NULL, backend->manifest_count,
                                                sizeof(*backend->manifest_by_id), error);
    backend->manifest_by_path = baca_reallocarray(NULL, backend->manifest_count,
                                                  sizeof(*backend->manifest_by_path), error);
    if (backend->manifest_by_id == NULL || backend->manifest_by_path == NULL) {
        return false;
    }
    for (size_t index = 0; index < backend->manifest_count; index++) {
        backend->manifest_by_id[index] = &backend->manifest[index];
        backend->manifest_by_path[index] = &backend->manifest[index];
        if (zip_name_locate(backend->archive, backend->manifest[index].member_path, ZIP_FL_ENC_GUESS) < 0) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest member not found: %s",
                           backend->manifest[index].path);
            return false;
        }
    }
    qsort(backend->manifest_by_id, backend->manifest_count, sizeof(*backend->manifest_by_id),
          compare_manifest_id);
    qsort(backend->manifest_by_path, backend->manifest_count, sizeof(*backend->manifest_by_path),
          compare_manifest_path);
    for (size_t index = 1; index < backend->manifest_count; index++) {
        if (strcmp(backend->manifest_by_id[index - 1U]->id, backend->manifest_by_id[index]->id) == 0) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest contains duplicate id: %s",
                           backend->manifest_by_id[index]->id);
            return false;
        }
        if (strcmp(backend->manifest_by_path[index - 1U]->path,
                   backend->manifest_by_path[index]->path) == 0) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest contains duplicate path: %s",
                           backend->manifest_by_path[index]->path);
            return false;
        }
    }
    for (size_t index = 0; index < backend->manifest_count; index++) {
        const char *fallback = backend->manifest[index].fallback;
        if (fallback != NULL && (fallback[0] == '\0' || manifest_by_id(backend, fallback) == NULL)) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest item has an invalid fallback: %s",
                           backend->manifest[index].id);
            return false;
        }
    }
    return true;
}

static bool item_is_html(const EpubManifestItem *item) {
    return strcmp(item->media_type, "application/xhtml+xml") == 0 ||
           strcmp(item->media_type, "text/html") == 0;
}

static bool item_is_svg(const EpubManifestItem *item) {
    return strcmp(item->media_type, "image/svg+xml") == 0;
}

static EpubManifestItem *supported_spine_item(EpubBackend *backend, EpubManifestItem *item,
                                              BacaError *error) {
    EpubManifestItem *current = item;
    for (size_t depth = 0; depth <= backend->manifest_count; depth++) {
        if (item_is_html(current) || item_is_svg(current)) {
            return current;
        }
        if (current->fallback == NULL) {
            baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                           "EPUB spine media type is unsupported and has no fallback: %s",
                           current->media_type);
            return NULL;
        }
        current = manifest_by_id(backend, current->fallback);
        if (current == NULL) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB fallback references a missing manifest item");
            return NULL;
        }
    }
    baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB manifest fallback cycle detected");
    return NULL;
}

static bool append_svg_section(BacaDocument *document, EpubBackend *backend,
                               const EpubManifestItem *item, bool linear, size_t search_offset,
                               BacaError *error) {
    BacaBuffer markup = {0};
    if (!read_member(backend, item->member_path, EPUB_MAX_MARKUP, &markup, error)) {
        return false;
    }
    xmlDocPtr parsed = parse_xml(&markup, item->member_path, error);
    if (parsed == NULL || !xml_node_is_ns(xmlDocGetRootElement(parsed), "svg", SVG_NAMESPACE)) {
        if (parsed != NULL) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB SVG spine item has an invalid root element: %s",
                           item->path);
        }
        xmlFreeDoc(parsed);
        baca_buffer_free(&markup);
        return false;
    }
    xmlFreeDoc(parsed);

    size_t first_block = document->block_count;
    BacaBlock block = {
        .kind = BACA_BLOCK_IMAGE,
        .section_id = baca_strdup(item->path, error),
        .value.image = {
            .uri = baca_strdup(item->path, error),
            .page_index = -1,
        },
    };
    if (block.section_id == NULL || block.value.image.uri == NULL ||
        !baca_document_add_image_block(document, &block, error)) {
        free(block.section_id);
        free(block.value.image.uri);
        baca_buffer_free(&markup);
        return false;
    }
    BacaSection section = {
        .id = baca_strdup(item->path, error),
        .first_block = first_block,
        .block_count = 1,
        .search_offset = search_offset,
        .source_size = markup.length,
        .linear = linear,
    };
    baca_buffer_free(&markup);
    if (section.id == NULL || !baca_document_add_section(document, &section, error)) {
        free(section.id);
        baca_document_rollback_blocks(document, first_block);
        return false;
    }
    return true;
}

static bool load_spine(BacaDocument *document, EpubBackend *backend, xmlNode *spine, BacaError *error) {
    size_t loaded = 0;
    size_t search_offset = 0;
    for (xmlNode *node = spine->children; node != NULL; node = node->next) {
        if (!xml_node_is_ns(node, "itemref", OPF_NAMESPACE)) {
            continue;
        }
        xmlChar *idref = xml_attribute(node, "idref");
        xmlChar *linear = xml_attribute(node, "linear");
        if (idref == NULL || idref[0] == '\0') {
            xmlFree(idref);
            xmlFree(linear);
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB spine item is missing idref");
            return false;
        }
        EpubManifestItem *item = manifest_by_id(backend, (const char *) idref);
        xmlFree(idref);
        if (item == NULL) {
            xmlFree(linear);
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB spine references a missing manifest item");
            return false;
        }
        bool is_linear = true;
        if (linear != NULL) {
            if (xmlStrcmp(linear, BAD_CAST "no") == 0) {
                is_linear = false;
            } else if (xmlStrcmp(linear, BAD_CAST "yes") != 0) {
                xmlFree(linear);
                baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB spine item has an invalid linear value");
                return false;
            }
        }
        EpubManifestItem *content_item = supported_spine_item(backend, item, error);
        if (content_item == NULL) {
            xmlFree(linear);
            return false;
        }
        if (item_is_svg(content_item)) {
            bool appended = append_svg_section(document, backend, content_item, is_linear, search_offset, error);
            xmlFree(linear);
            if (!appended) {
                return false;
            }
            loaded++;
            continue;
        }
        BacaBuffer markup = {0};
        if (!read_member(backend, content_item->member_path, EPUB_MAX_MARKUP, &markup, error)) {
            xmlFree(linear);
            return false;
        }
        bool appended = baca_html_append_section(document, (const char *) markup.data, markup.length,
                                                  content_item->path, error);
        baca_buffer_free(&markup);
        if (!appended) {
            xmlFree(linear);
            return false;
        }
        BacaSection *section = &document->sections[document->section_count - 1];
        section->linear = is_linear;
        section->search_offset = search_offset;
        for (size_t block_index = section->first_block;
             block_index < section->first_block + section->block_count; block_index++) {
            if (document->blocks[block_index].kind == BACA_BLOCK_TEXT &&
                document->blocks[block_index].value.text.text != NULL) {
                size_t text_length = strlen(document->blocks[block_index].value.text.text);
                if (text_length > SIZE_MAX - search_offset) {
                    xmlFree(linear);
                    baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB searchable text size overflow");
                    return false;
                }
                search_offset += text_length;
            }
        }
        xmlFree(linear);
        loaded++;
    }
    if (loaded == 0) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB spine contains no supported content");
        return false;
    }
    return true;
}

static xmlNode *find_nav_link_before_list(xmlNode *node) {
    for (xmlNode *child = node->children; child != NULL; child = child->next) {
        if (xml_node_is_ns(child, "ol", XHTML_NAMESPACE)) {
            return NULL;
        }
        if (xml_node_is_ns(child, "a", XHTML_NAMESPACE)) {
            return child;
        }
        if (child->type == XML_ELEMENT_NODE) {
            xmlNode *link = find_nav_link_before_list(child);
            if (link != NULL) {
                return link;
            }
        }
    }
    return NULL;
}

static xmlNode *find_nav_link(xmlNode *list_item) {
    return find_nav_link_before_list(list_item);
}

static bool parse_nav_lists(BacaDocument *document, xmlNode *node, const char *nav_path, unsigned depth,
                            BacaError *error);

static bool parse_nested_nav_lists(BacaDocument *document, xmlNode *node, const char *nav_path, unsigned depth,
                                   BacaError *error) {
    for (xmlNode *child = node->children; child != NULL; child = child->next) {
        if (xml_node_is_ns(child, "ol", XHTML_NAMESPACE)) {
            if (!parse_nav_lists(document, child, nav_path, depth, error)) {
                return false;
            }
        } else if (child->type == XML_ELEMENT_NODE &&
                   !parse_nested_nav_lists(document, child, nav_path, depth, error)) {
            return false;
        }
    }
    return true;
}

static bool parse_nav_lists(BacaDocument *document, xmlNode *node, const char *nav_path, unsigned depth,
                             BacaError *error) {
    if (depth > 256U) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB navigation is nested too deeply");
        return false;
    }
    for (xmlNode *item = node->children; item != NULL; item = item->next) {
        if (!xml_node_is_ns(item, "li", XHTML_NAMESPACE)) {
            continue;
        }
        xmlNode *link = find_nav_link(item);
        if (link != NULL) {
            xmlChar *href = xml_attribute(link, "href");
            if (href != NULL && href[0] != '\0') {
                if (baca_is_external_uri((const char *) href) ||
                    strncmp((const char *) href, "//", 2) == 0) {
                    baca_error_set(error, BACA_ERROR_CORRUPT, "external EPUB TOC target is not allowed: %s",
                                   (const char *) href);
                    xmlFree(href);
                    return false;
                }
                char *label = NULL;
                char *target = resolve_reference(nav_path, (const char *) href, false, error);
                bool success = target != NULL && collapse_label(link, &label, error);
                if (success && label[0] != '\0') {
                    success = baca_document_add_toc(document, label, target, depth, error);
                }
                free(label);
                free(target);
                xmlFree(href);
                if (!success) {
                    return false;
                }
            } else {
                xmlFree(href);
            }
        }
        if (!parse_nested_nav_lists(document, item, nav_path, depth + 1, error)) {
            return false;
        }
    }
    return true;
}

static bool nav_is_toc(xmlNode *nav) {
    xmlChar *epub_type = xml_attribute_ns(nav, "type", EPUB_NAMESPACE);
    xmlChar *role = xml_attribute(nav, "role");
    bool matches = (epub_type != NULL && token_contains((const char *) epub_type, "toc")) ||
                   (role != NULL && token_contains((const char *) role, "doc-toc"));
    xmlFree(epub_type);
    xmlFree(role);
    return matches;
}

static xmlNode *find_toc_nav(xmlNode *node) {
    if (xml_node_is_ns(node, "nav", XHTML_NAMESPACE) && nav_is_toc(node)) {
        return node;
    }
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        xmlNode *found = find_toc_nav(child);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static bool parse_epub3_nav(BacaDocument *document, EpubBackend *backend, EpubManifestItem *nav_item,
                             bool *parsed_list, BacaError *error) {
    *parsed_list = false;
    BacaBuffer markup = {0};
    if (!read_member(backend, nav_item->member_path, EPUB_MAX_MARKUP, &markup, error)) {
        return false;
    }
    xmlDocPtr parsed = parse_xml(&markup, nav_item->member_path, error);
    baca_buffer_free(&markup);
    if (parsed == NULL) {
        return false;
    }
    xmlNode *root = xmlDocGetRootElement(parsed);
    if (!xml_node_is_ns(root, "html", XHTML_NAMESPACE)) {
        xmlFreeDoc(parsed);
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB navigation document has an invalid root element");
        return false;
    }
    xmlNode *nav = find_toc_nav(root);
    xmlNode *list = nav == NULL ? NULL : xml_find_descendant_ns(nav, "ol", XHTML_NAMESPACE);
    *parsed_list = list != NULL;
    bool success = list == NULL || parse_nav_lists(document, list, nav_item->path, 0, error);
    xmlFreeDoc(parsed);
    return success;
}

static bool parse_ncx_points(BacaDocument *document, xmlNode *parent, const char *ncx_path, unsigned depth,
                              BacaError *error) {
    if (depth > 256U) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB NCX navigation is nested too deeply");
        return false;
    }
    for (xmlNode *point = parent->children; point != NULL; point = point->next) {
        if (!xml_node_is_ns(point, "navPoint", NCX_NAMESPACE)) {
            continue;
        }
        xmlNode *label_node = find_direct_child_ns(point, "navLabel", NCX_NAMESPACE);
        xmlNode *content_node = find_direct_child_ns(point, "content", NCX_NAMESPACE);
        xmlChar *source = content_node == NULL ? NULL : xml_attribute(content_node, "src");
        if (label_node != NULL && source != NULL && source[0] != '\0') {
            if (baca_is_external_uri((const char *) source) ||
                strncmp((const char *) source, "//", 2) == 0) {
                baca_error_set(error, BACA_ERROR_CORRUPT, "external EPUB TOC target is not allowed: %s",
                               (const char *) source);
                xmlFree(source);
                return false;
            }
            char *label = NULL;
            char *target = resolve_reference(ncx_path, (const char *) source, false, error);
            bool success = target != NULL && collapse_label(label_node, &label, error);
            if (success && label[0] != '\0') {
                success = baca_document_add_toc(document, label, target, depth, error);
            }
            free(label);
            free(target);
            xmlFree(source);
            if (!success) {
                return false;
            }
        } else {
            xmlFree(source);
        }
        if (!parse_ncx_points(document, point, ncx_path, depth + 1, error)) {
            return false;
        }
    }
    return true;
}

static bool parse_ncx(BacaDocument *document, EpubBackend *backend, EpubManifestItem *ncx_item,
                      BacaError *error) {
    BacaBuffer markup = {0};
    if (!read_member(backend, ncx_item->member_path, EPUB_MAX_MARKUP, &markup, error)) {
        return false;
    }
    xmlDocPtr parsed = parse_xml(&markup, ncx_item->member_path, error);
    baca_buffer_free(&markup);
    if (parsed == NULL) {
        return false;
    }
    xmlNode *root = xmlDocGetRootElement(parsed);
    if (!xml_node_is_ns(root, "ncx", NCX_NAMESPACE)) {
        xmlFreeDoc(parsed);
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB NCX document has an invalid root element");
        return false;
    }
    xmlNode *nav_map = find_direct_child_ns(root, "navMap", NCX_NAMESPACE);
    bool success = nav_map != NULL && parse_ncx_points(document, nav_map, ncx_item->path, 0, error);
    if (nav_map == NULL) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB NCX document has no navigation map");
    }
    xmlFreeDoc(parsed);
    return success;
}

static EpubManifestItem *find_nav_item(EpubBackend *backend) {
    for (size_t index = 0; index < backend->manifest_count; index++) {
        if (token_contains(backend->manifest[index].properties, "nav")) {
            return &backend->manifest[index];
        }
    }
    return NULL;
}

static EpubManifestItem *find_ncx_item(EpubBackend *backend, xmlNode *spine) {
    xmlChar *toc_id = spine == NULL ? NULL : xml_attribute(spine, "toc");
    EpubManifestItem *result = toc_id == NULL ? NULL : manifest_by_id(backend, (const char *) toc_id);
    xmlFree(toc_id);
    if (result != NULL) {
        return result;
    }
    for (size_t index = 0; index < backend->manifest_count; index++) {
        if (strcmp(backend->manifest[index].media_type, "application/x-dtbncx+xml") == 0) {
            return &backend->manifest[index];
        }
    }
    return NULL;
}

static bool parse_toc(BacaDocument *document, EpubBackend *backend, xmlNode *spine, BacaError *error) {
    size_t original_count = document->toc_count;
    EpubManifestItem *nav = find_nav_item(backend);
    if (nav != NULL) {
        BacaError nav_error = {0};
        bool parsed_list = false;
        if (!parse_epub3_nav(document, backend, nav, &parsed_list, &nav_error)) {
            baca_document_rollback_toc(document, original_count);
            if (parsed_list || nav_error.code == BACA_ERROR_MEMORY) {
                if (error != NULL) {
                    *error = nav_error;
                }
                return false;
            }
        }
        if (document->toc_count > original_count) {
            return true;
        }
        baca_document_rollback_toc(document, original_count);
    }

    EpubManifestItem *ncx = find_ncx_item(backend, spine);
    if (ncx != NULL) {
        if (!parse_ncx(document, backend, ncx, error)) {
            baca_document_rollback_toc(document, original_count);
            return false;
        }
        if (document->toc_count == original_count) {
            baca_document_rollback_toc(document, original_count);
        }
    }
    return true;
}

static const char *mime_from_extension(const char *path) {
    const char *extension = strrchr(path, '.');
    if (extension == NULL) {
        return "application/octet-stream";
    }
    if (baca_casecmp(extension, ".png") == 0) {
        return "image/png";
    }
    if (baca_casecmp(extension, ".jpg") == 0 || baca_casecmp(extension, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (baca_casecmp(extension, ".gif") == 0) {
        return "image/gif";
    }
    if (baca_casecmp(extension, ".webp") == 0) {
        return "image/webp";
    }
    if (baca_casecmp(extension, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (baca_casecmp(extension, ".avif") == 0) {
        return "image/avif";
    }
    if (baca_casecmp(extension, ".bmp") == 0) {
        return "image/bmp";
    }
    if (baca_casecmp(extension, ".css") == 0) {
        return "text/css";
    }
    if (baca_casecmp(extension, ".ttf") == 0) {
        return "font/ttf";
    }
    if (baca_casecmp(extension, ".otf") == 0) {
        return "font/otf";
    }
    if (baca_casecmp(extension, ".woff") == 0) {
        return "font/woff";
    }
    if (baca_casecmp(extension, ".woff2") == 0) {
        return "font/woff2";
    }
    return "application/octet-stream";
}

static bool epub_resource_paths(EpubBackend *backend, const char *uri, char **path,
                                char **member_path, BacaError *error) {
    if (uri == NULL || baca_is_external_uri(uri) || strncmp(uri, "//", 2) == 0) {
        baca_error_set(error, BACA_ERROR_UNSUPPORTED, "external EPUB resources are not fetched: %s",
                       uri == NULL ? "(null)" : uri);
        return false;
    }
    char *resolved = member_path_from_reference("/", uri, error);
    if (resolved == NULL) {
        return false;
    }
    EpubManifestItem *item = manifest_by_path(backend, resolved);
    char *member = item == NULL ? archive_member_path(resolved, error) :
                                 baca_strdup(item->member_path, error);
    if (member == NULL) {
        free(resolved);
        return false;
    }
    *path = resolved;
    *member_path = member;
    return true;
}

static bool epub_resource_size(const BacaDocument *document, const char *uri, size_t *size) {
    if (document == NULL || size == NULL) {
        return false;
    }
    EpubBackend *backend = document->backend;
    if (backend == NULL || backend->archive == NULL) {
        return false;
    }
    BacaError ignored = {0};
    char *path = NULL;
    char *member_path = NULL;
    if (!epub_resource_paths(backend, uri, &path, &member_path, &ignored)) {
        return false;
    }
    zip_int64_t member_index = zip_name_locate(backend->archive, member_path, ZIP_FL_ENC_GUESS);
    zip_stat_t stat;
    zip_stat_init(&stat);
    const bool found = member_index >= 0 &&
                       zip_stat_index(backend->archive, (zip_uint64_t)member_index,
                                      ZIP_FL_ENC_GUESS, &stat) == 0 &&
                       (stat.valid & ZIP_STAT_SIZE) != 0 && stat.size <= EPUB_MAX_MEMBER &&
                       stat.size <= (zip_uint64_t)SIZE_MAX;
    free(member_path);
    free(path);
    if (!found) {
        return false;
    }
    *size = (size_t)stat.size;
    return true;
}

static bool epub_load_resource(BacaDocument *document, const char *uri, BacaResource *resource,
                                BacaError *error) {
    EpubBackend *backend = document->backend;
    if (backend == NULL || backend->archive == NULL) {
        baca_error_set(error, BACA_ERROR_INTERNAL, "EPUB backend is closed");
        return false;
    }
    char *path = NULL;
    char *member_path = NULL;
    if (!epub_resource_paths(backend, uri, &path, &member_path, error)) {
        return false;
    }
    EpubManifestItem *item = manifest_by_path(backend, path);
    BacaBuffer data = {0};
    if (!read_member(backend, member_path, EPUB_MAX_MEMBER, &data, error)) {
        free(member_path);
        free(path);
        return false;
    }
    free(member_path);

    const char *mime_type = item == NULL ? NULL : item->media_type;
    if (mime_type == NULL) {
        mime_type = mime_from_extension(path);
    }
    char *owned_mime = baca_strdup(mime_type, error);
    free(path);
    if (owned_mime == NULL) {
        baca_buffer_free(&data);
        return false;
    }
    resource->data = data.data;
    resource->length = data.length;
    resource->mime_type = owned_mime;
    return true;
}

static bool inspect_encrypted_data(xmlNode *node, BacaError *error) {
    if (xml_node_is_ns(node, "EncryptedData", XML_ENCRYPTION_NAMESPACE)) {
        xmlNode *method = find_direct_child_ns(node, "EncryptionMethod", XML_ENCRYPTION_NAMESPACE);
        xmlNode *cipher_data = find_direct_child_ns(node, "CipherData", XML_ENCRYPTION_NAMESPACE);
        xmlNode *reference = find_direct_child_ns(cipher_data, "CipherReference", XML_ENCRYPTION_NAMESPACE);
        xmlChar *algorithm = method == NULL ? NULL : xml_attribute(method, "Algorithm");
        xmlChar *uri = reference == NULL ? NULL : xml_attribute(reference, "URI");
        if (algorithm == NULL || algorithm[0] == '\0' || uri == NULL || uri[0] == '\0') {
            xmlFree(algorithm);
            xmlFree(uri);
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB encryption entry is missing required data");
            return false;
        }
        char *target = resolve_reference("META-INF/encryption.xml", (const char *) uri, false, error);
        if (target == NULL) {
            xmlFree(algorithm);
            xmlFree(uri);
            return false;
        }
        bool font_obfuscation = xmlStrcmp(algorithm, BAD_CAST "http://www.idpf.org/2008/embedding") == 0 ||
                                xmlStrcmp(algorithm, BAD_CAST "http://ns.adobe.com/pdf/enc#RC") == 0;
        if (font_obfuscation) {
            baca_error_set(error, BACA_ERROR_UNSUPPORTED, "EPUB font obfuscation is not supported: %s",
                           target);
        } else {
            baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                           "encrypted EPUB content is not supported (algorithm %s, resource %s)",
                           (const char *) algorithm, target);
        }
        free(target);
        xmlFree(algorithm);
        xmlFree(uri);
        return false;
    }
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        if (!inspect_encrypted_data(child, error)) {
            return false;
        }
    }
    return true;
}

static bool validate_encryption(EpubBackend *backend, BacaError *error) {
    if (zip_name_locate(backend->archive, "META-INF/encryption.xml", ZIP_FL_ENC_GUESS) < 0) {
        return true;
    }
    BacaBuffer encryption = {0};
    if (!read_member(backend, "META-INF/encryption.xml", EPUB_MAX_MARKUP, &encryption, error)) {
        return false;
    }
    xmlDocPtr parsed = parse_xml(&encryption, "META-INF/encryption.xml", error);
    baca_buffer_free(&encryption);
    if (parsed == NULL) {
        return false;
    }
    xmlNode *root = xmlDocGetRootElement(parsed);
    if (!xml_node_is_ns(root, "encryption", CONTAINER_NAMESPACE)) {
        xmlFreeDoc(parsed);
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB encryption document has an invalid root element");
        return false;
    }
    bool valid = true;
    for (xmlNode *child = root->children; child != NULL && valid; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) {
            continue;
        }
        if (!xml_node_is_ns(child, "EncryptedData", XML_ENCRYPTION_NAMESPACE)) {
            baca_error_set(error, BACA_ERROR_CORRUPT,
                           "EPUB encryption document contains an unexpected element");
            valid = false;
        } else {
            valid = inspect_encrypted_data(child, error);
        }
    }
    xmlFreeDoc(parsed);
    return valid;
}

static bool validate_mimetype(EpubBackend *backend, BacaError *error) {
    zip_int64_t index = zip_name_locate(backend->archive, "mimetype", ZIP_FL_ENC_GUESS);
    if (index != 0) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB mimetype must be the first archive member");
        return false;
    }
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(backend->archive, 0, ZIP_FL_ENC_GUESS, &stat) != 0 ||
        (stat.valid & ZIP_STAT_COMP_METHOD) == 0 || stat.comp_method != ZIP_CM_STORE) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB mimetype member must be stored without compression");
        return false;
    }
    BacaBuffer mimetype = {0};
    if (!read_member(backend, "mimetype", 64, &mimetype, error)) {
        return false;
    }
    bool valid = mimetype.length == 20 && memcmp(mimetype.data, "application/epub+zip", 20) == 0;
    baca_buffer_free(&mimetype);
    if (!valid) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB mimetype member is invalid");
    }
    return valid;
}

static bool open_archive(EpubBackend *backend, const char *path, BacaError *error) {
    int zip_error_code = 0;
    backend->archive = zip_open(path, ZIP_RDONLY, &zip_error_code);
    if (backend->archive == NULL) {
        zip_error_t zip_error;
        zip_error_init_with_code(&zip_error, zip_error_code);
        baca_error_set(error, BACA_ERROR_CORRUPT, "could not open EPUB archive: %s", zip_error_strerror(&zip_error));
        zip_error_fini(&zip_error);
        return false;
    }
    return validate_archive(backend, error) && validate_mimetype(backend, error);
}

static bool find_opf_path(EpubBackend *backend, BacaError *error) {
    BacaBuffer container = {0};
    if (!read_member(backend, "META-INF/container.xml", EPUB_MAX_MARKUP, &container, error)) {
        return false;
    }
    xmlDocPtr parsed = parse_xml(&container, "META-INF/container.xml", error);
    baca_buffer_free(&container);
    if (parsed == NULL) {
        return false;
    }

    xmlNode *root = xmlDocGetRootElement(parsed);
    xmlChar *version = root == NULL ? NULL : xml_attribute(root, "version");
    if (!xml_node_is_ns(root, "container", CONTAINER_NAMESPACE) || version == NULL ||
        xmlStrcmp(version, BAD_CAST "1.0") != 0) {
        xmlFree(version);
        xmlFreeDoc(parsed);
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB container has an invalid root element or version");
        return false;
    }
    xmlFree(version);
    xmlNode *rootfiles = find_direct_child_ns(root, "rootfiles", CONTAINER_NAMESPACE);
    xmlChar *selected = NULL;
    if (rootfiles != NULL) {
        for (xmlNode *node = rootfiles->children; node != NULL; node = node->next) {
            if (!xml_node_is_ns(node, "rootfile", CONTAINER_NAMESPACE)) {
                continue;
            }
            xmlChar *path = xml_attribute(node, "full-path");
            xmlChar *media_type = xml_attribute(node, "media-type");
            if (path != NULL && media_type != NULL &&
                path[0] != '\0' && strpbrk((const char *) path, "?#") == NULL &&
                xmlStrcmp(media_type, BAD_CAST "application/oebps-package+xml") == 0 && selected == NULL) {
                selected = xmlStrdup(path);
            }
            xmlFree(path);
            xmlFree(media_type);
        }
    }
    xmlFreeDoc(parsed);
    if (selected == NULL || selected[0] == '\0') {
        xmlFree(selected);
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB container has no package rootfile");
        return false;
    }
    backend->opf_path = member_path_from_reference("/", (const char *) selected, error);
    xmlFree(selected);
    if (backend->opf_path == NULL) {
        return false;
    }
    backend->opf_member_path = archive_member_path(backend->opf_path, error);
    return backend->opf_member_path != NULL;
}

static bool package_children(xmlNode *package, xmlNode **metadata, xmlNode **manifest, xmlNode **spine,
                             BacaError *error) {
    for (xmlNode *child = package->children; child != NULL; child = child->next) {
        xmlNode **slot = NULL;
        if (xml_node_is_ns(child, "metadata", OPF_NAMESPACE)) {
            slot = metadata;
        } else if (xml_node_is_ns(child, "manifest", OPF_NAMESPACE)) {
            slot = manifest;
        } else if (xml_node_is_ns(child, "spine", OPF_NAMESPACE)) {
            slot = spine;
        }
        if (slot == NULL) {
            continue;
        }
        if (*slot != NULL) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB package contains duplicate %s elements",
                           (const char *) child->name);
            return false;
        }
        *slot = child;
    }
    if (*metadata == NULL || *manifest == NULL || *spine == NULL) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB package is missing metadata, manifest, or spine");
        return false;
    }
    return true;
}

bool baca_epub_open(BacaDocument *document, const char *path, const char *cleanup_directory, BacaError *error) {
    if (document == NULL || path == NULL || path[0] == '\0') {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document and EPUB path are required");
        return false;
    }
    EpubBackend *backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate EPUB backend");
        return false;
    }
    if (cleanup_directory != NULL) {
        backend->cleanup_directory = baca_strdup(cleanup_directory, error);
        if (backend->cleanup_directory == NULL) {
            backend_destroy(backend);
            return false;
        }
    }
    if (!open_archive(backend, path, error) || !find_opf_path(backend, error) ||
        !validate_encryption(backend, error)) {
        backend_destroy(backend);
        return false;
    }

    BacaBuffer package_data = {0};
    if (!read_member(backend, backend->opf_member_path, EPUB_MAX_MARKUP, &package_data, error)) {
        backend_destroy(backend);
        return false;
    }
    xmlDocPtr package_document = parse_xml(&package_data, backend->opf_member_path, error);
    baca_buffer_free(&package_data);
    if (package_document == NULL) {
        backend_destroy(backend);
        return false;
    }
    xmlNode *package = xmlDocGetRootElement(package_document);
    xmlChar *version = package == NULL ? NULL : xml_attribute(package, "version");
    bool valid_version = version != NULL &&
                         (xmlStrcmp(version, BAD_CAST "2.0") == 0 ||
                          xmlStrcmp(version, BAD_CAST "3.0") == 0);
    xmlFree(version);
    xmlNode *metadata = NULL;
    xmlNode *manifest = NULL;
    xmlNode *spine = NULL;
    if (!xml_node_is_ns(package, "package", OPF_NAMESPACE) || !valid_version ||
        !package_children(package, &metadata, &manifest, &spine, error) ||
        !parse_metadata(document, metadata, error) || !baca_document_account_metadata(document, error) ||
        !parse_manifest(backend, manifest, error) || !index_manifest(backend, error)) {
        if (!baca_error_is_set(error)) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "EPUB OPF has an invalid package root or version");
        }
        xmlFreeDoc(package_document);
        backend_destroy(backend);
        return false;
    }

    document->backend = backend;
    document->ops = &EPUB_OPS;
    document->format = BACA_FORMAT_EPUB;
    if (!load_spine(document, backend, spine, error) || !parse_toc(document, backend, spine, error) ||
        !baca_document_index_toc_sections(document, error)) {
        xmlFreeDoc(package_document);
        document->backend = NULL;
        document->ops = NULL;
        backend_destroy(backend);
        return false;
    }
    xmlFreeDoc(package_document);
    return true;
}
