#include "baca/document_backend.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#define BACA_HTML_MAX (64U * 1024U * 1024U)

typedef struct HtmlContext {
    BacaDocument *document;
    const char *section_id;
    BacaString text;
    BacaTextSpan *spans;
    size_t span_count;
    size_t span_capacity;
    BacaAnchor *anchors;
    size_t anchor_count;
    size_t anchor_capacity;
    unsigned heading_level;
    unsigned list_depth;
    bool preformatted;
    bool pending_space;
    BacaError *error;
} HtmlContext;

static bool walk_node(HtmlContext *context, xmlNode *node, BacaTextStyle style, const char *link,
                      bool suppress_blocks);

static bool node_is(const xmlNode *node, const char *name) {
    return node != NULL && node->type == XML_ELEMENT_NODE && xmlStrcasecmp(node->name, BAD_CAST name) == 0;
}

static void context_text_free(HtmlContext *context) {
    baca_string_free(&context->text);
    for (size_t index = 0; index < context->span_count; index++) {
        free(context->spans[index].link);
    }
    free(context->spans);
    for (size_t index = 0; index < context->anchor_count; index++) {
        free(context->anchors[index].target);
    }
    free(context->anchors);
    context->spans = NULL;
    context->span_count = 0;
    context->span_capacity = 0;
    context->anchors = NULL;
    context->anchor_count = 0;
    context->anchor_capacity = 0;
    context->pending_space = false;
}

static bool links_equal(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return strcmp(left, right) == 0;
}

static bool add_span(HtmlContext *context, size_t start, size_t end, BacaTextStyle style, const char *link) {
    if (start == end || (style == BACA_STYLE_NONE && link == NULL)) {
        return true;
    }
    if (context->span_count > 0) {
        BacaTextSpan *last = &context->spans[context->span_count - 1];
        if (last->end == start && last->style == style && links_equal(last->link, link)) {
            last->end = end;
            return true;
        }
    }
    if (context->span_count >= BACA_DOCUMENT_MAX_SPANS_PER_BLOCK) {
        baca_error_set(context->error, BACA_ERROR_CORRUPT, "HTML text block contains too many spans");
        return false;
    }

    char *owned_link = NULL;
    if (link != NULL) {
        owned_link = baca_strdup(link, context->error);
        if (owned_link == NULL) {
            return false;
        }
    }
    BacaError reserve_error = {0};
    BacaTextSpan *spans = baca_array_reserve(context->spans, &context->span_capacity, sizeof(*context->spans),
                                             context->span_count + 1, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (context->error != NULL) {
            *context->error = reserve_error;
        }
        free(owned_link);
        return false;
    }
    context->spans = spans;
    context->spans[context->span_count++] = (BacaTextSpan) {
        .start = start,
        .end = end,
        .style = style,
        .link = owned_link,
    };
    return true;
}

static bool append_chunk(HtmlContext *context, const char *value, size_t length, BacaTextStyle style,
                          const char *link) {
    if (length == 0) {
        return true;
    }
    if (context->text.length > BACA_DOCUMENT_MAX_RETAINED_BYTES ||
        length > BACA_DOCUMENT_MAX_RETAINED_BYTES - context->text.length) {
        baca_error_set(context->error, BACA_ERROR_CORRUPT, "HTML text block exceeds the supported size limit");
        return false;
    }
    size_t start = context->text.length;
    if (!baca_string_append_n(&context->text, value, length, context->error)) {
        return false;
    }
    return add_span(context, start, context->text.length, style, link);
}

static bool is_html_space(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f';
}

static bool append_text(HtmlContext *context, const char *value, size_t length, BacaTextStyle style,
                         const char *link) {
    if (context->heading_level > 0) {
        style = (BacaTextStyle) (style | BACA_STYLE_HEADING);
    }
    if (context->preformatted) {
        context->pending_space = false;
        size_t segment_start = 0;
        for (size_t index = 0; index < length; index++) {
            if (value[index] != '\r') {
                continue;
            }
            if (!append_chunk(context, value + segment_start, index - segment_start, style, link) ||
                !append_chunk(context, "\n", 1, style, link)) {
                return false;
            }
            if (index + 1 < length && value[index + 1] == '\n') {
                index++;
            }
            segment_start = index + 1;
        }
        return append_chunk(context, value + segment_start, length - segment_start, style, link);
    }

    size_t index = 0;
    while (index < length) {
        bool non_breaking_space = index + 1 < length && (unsigned char) value[index] == 0xc2U &&
                                  (unsigned char) value[index + 1] == 0xa0U;
        if (is_html_space((unsigned char) value[index]) || non_breaking_space) {
            context->pending_space = true;
            index += non_breaking_space ? 2U : 1U;
            continue;
        }
        if (context->pending_space && context->text.length > 0 &&
            context->text.data[context->text.length - 1] != '\n') {
            if (!append_chunk(context, " ", 1, style, link)) {
                return false;
            }
        }
        context->pending_space = false;
        size_t start = index;
        while (index < length) {
            non_breaking_space = index + 1 < length && (unsigned char) value[index] == 0xc2U &&
                                 (unsigned char) value[index + 1] == 0xa0U;
            if (is_html_space((unsigned char) value[index]) || non_breaking_space) {
                break;
            }
            index++;
        }
        if (!append_chunk(context, value + start, index - start, style, link)) {
            return false;
        }
    }
    return true;
}

static bool append_break(HtmlContext *context, BacaTextStyle style, const char *link) {
    context->pending_space = false;
    while (!context->preformatted && context->text.length > 0 &&
           context->text.data[context->text.length - 1] == ' ') {
        context->text.data[--context->text.length] = '\0';
    }
    return append_chunk(context, "\n", 1, style, link);
}

static char *anchor_target(const HtmlContext *context, const char *value) {
    return baca_document_fragment_target(context->section_id, value, context->error);
}

static bool add_anchor(HtmlContext *context, const char *value) {
    if (value == NULL || value[0] == '\0') {
        return true;
    }
    char *target = anchor_target(context, value);
    if (target == NULL) {
        return false;
    }
    if (context->anchor_count > 0 &&
        strcmp(context->anchors[context->anchor_count - 1U].target, target) == 0) {
        free(target);
        return true;
    }
    if (context->anchor_count >= BACA_DOCUMENT_MAX_ANCHORS_PER_BLOCK) {
        free(target);
        baca_error_set(context->error, BACA_ERROR_CORRUPT, "HTML text block contains too many anchors");
        return false;
    }
    BacaError reserve_error = {0};
    BacaAnchor *anchors = baca_array_reserve(context->anchors, &context->anchor_capacity,
                                             sizeof(*context->anchors), context->anchor_count + 1,
                                             &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        if (context->error != NULL) {
            *context->error = reserve_error;
        }
        free(target);
        return false;
    }
    context->anchors = anchors;
    context->anchors[context->anchor_count++] = (BacaAnchor) {
        .target = target,
        .offset = context->text.length,
    };
    return true;
}

static xmlChar *node_attribute(const xmlNode *node, const char *name) {
    for (const xmlAttr *attribute = node->properties; attribute != NULL; attribute = attribute->next) {
        const xmlChar *local_name = attribute->name;
        const xmlChar *colon = xmlStrchr(attribute->name, ':');
        if (colon != NULL) {
            local_name = colon + 1;
        }
        if (xmlStrcasecmp(local_name, BAD_CAST name) == 0) {
            return xmlNodeListGetString(node->doc, attribute->children, 1);
        }
    }
    return NULL;
}

static bool record_node_anchors(HtmlContext *context, const xmlNode *node) {
    xmlChar *id = node_attribute(node, "id");
    xmlChar *name = node_is(node, "a") ? node_attribute(node, "name") : NULL;
    bool success = id == NULL || add_anchor(context, (const char *) id);
    if (success && name != NULL && (id == NULL || xmlStrcmp(id, name) != 0)) {
        success = add_anchor(context, (const char *) name);
    }
    xmlFree(id);
    xmlFree(name);
    return success;
}

static void reset_text_context(HtmlContext *context) {
    memset(&context->text, 0, sizeof(context->text));
    context->spans = NULL;
    context->span_count = 0;
    context->span_capacity = 0;
    context->anchors = NULL;
    context->anchor_count = 0;
    context->anchor_capacity = 0;
    context->pending_space = false;
}

static void trim_text_metadata(HtmlContext *context) {
    while (context->span_count > 0 && context->spans[context->span_count - 1].start >= context->text.length) {
        free(context->spans[context->span_count - 1].link);
        context->span_count--;
    }
    if (context->span_count > 0 && context->spans[context->span_count - 1].end > context->text.length) {
        context->spans[context->span_count - 1].end = context->text.length;
    }
    for (size_t index = 0; index < context->anchor_count; index++) {
        if (context->anchors[index].offset > context->text.length) {
            context->anchors[index].offset = context->text.length;
        }
    }
}

static bool flush_text(HtmlContext *context) {
    if (!context->preformatted) {
        context->pending_space = false;
        while (context->text.length > 0 &&
               (context->text.data[context->text.length - 1] == ' ' ||
                context->text.data[context->text.length - 1] == '\n')) {
            context->text.data[--context->text.length] = '\0';
        }
        trim_text_metadata(context);
    }
    if (context->text.length == 0 && context->anchor_count == 0) {
        context_text_free(context);
        reset_text_context(context);
        return true;
    }

    char *text = baca_string_take(&context->text);
    if (text == NULL) {
        text = baca_strdup("", context->error);
        if (text == NULL) {
            return false;
        }
    }
    char *section_id = baca_strdup(context->section_id, context->error);
    if (section_id == NULL) {
        free(text);
        return false;
    }

    BacaBlock block = {
        .kind = BACA_BLOCK_TEXT,
        .section_id = section_id,
        .value.text = {
            .text = text,
            .spans = context->spans,
            .span_count = context->span_count,
            .span_capacity = context->span_capacity,
            .anchors = context->anchors,
            .anchor_count = context->anchor_count,
            .anchor_capacity = context->anchor_capacity,
            .heading_level = context->heading_level,
            .preformatted = context->preformatted,
        },
    };
    reset_text_context(context);
    if (!baca_document_add_text_block(context->document, &block, context->error)) {
        baca_document_block_free(&block);
        return false;
    }
    return true;
}

static char *resolve_uri(HtmlContext *context, const char *uri) {
    return baca_document_resolve_uri(context->section_id, uri, true, context->error);
}

static bool srcset_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f';
}

static bool valid_srcset_integer(const char *start, const char *end) {
    if (start == end || *start == '0') {
        return false;
    }
    for (const char *cursor = start; cursor < end; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }
    return true;
}

static bool valid_srcset_density(const char *start, const char *end) {
    bool digit = false;
    bool dot = false;
    bool nonzero = false;
    if (start == end) {
        return false;
    }
    for (const char *cursor = start; cursor < end; cursor++) {
        if (*cursor >= '0' && *cursor <= '9') {
            digit = true;
            nonzero = nonzero || *cursor != '0';
        } else if (*cursor == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    return digit && nonzero;
}

static bool valid_srcset_descriptors(const char *start, const char *end) {
    bool width = false;
    bool height = false;
    bool density = false;
    while (start < end) {
        while (start < end && srcset_space(*start)) {
            start++;
        }
        if (start == end) {
            break;
        }
        const char *token_end = start;
        while (token_end < end && !srcset_space(*token_end)) {
            token_end++;
        }
        if (token_end - start < 2) {
            return false;
        }
        char suffix = token_end[-1];
        const char *number_end = token_end - 1;
        if (suffix == 'w' && !width && valid_srcset_integer(start, number_end)) {
            width = true;
        } else if (suffix == 'h' && !height && valid_srcset_integer(start, number_end)) {
            height = true;
        } else if (suffix == 'x' && !density && valid_srcset_density(start, number_end)) {
            density = true;
        } else {
            return false;
        }
        start = token_end;
    }
    return !(density && (width || height)) && (!height || width);
}

static char *srcset_first(HtmlContext *context, const xmlChar *srcset) {
    const char *cursor = (const char *) srcset;
    while (*cursor != '\0') {
        while (srcset_space(*cursor) || *cursor == ',') {
            cursor++;
        }
        const char *url_start = cursor;
        while (*cursor != '\0' && !srcset_space(*cursor)) {
            cursor++;
        }
        const char *url_end = cursor;
        bool ended_by_comma = false;
        while (url_end > url_start && url_end[-1] == ',') {
            url_end--;
            ended_by_comma = true;
        }
        if (url_end == url_start) {
            continue;
        }
        if (ended_by_comma) {
            return baca_strndup(url_start, (size_t) (url_end - url_start), context->error);
        }

        while (srcset_space(*cursor)) {
            cursor++;
        }
        const char *descriptors = cursor;
        unsigned parentheses = 0;
        while (*cursor != '\0') {
            if (*cursor == '(') {
                parentheses++;
            } else if (*cursor == ')' && parentheses > 0) {
                parentheses--;
            } else if (*cursor == ',' && parentheses == 0) {
                break;
            }
            cursor++;
        }
        const char *descriptor_end = cursor;
        while (descriptor_end > descriptors && srcset_space(descriptor_end[-1])) {
            descriptor_end--;
        }
        if (parentheses == 0 && valid_srcset_descriptors(descriptors, descriptor_end)) {
            return baca_strndup(url_start, (size_t) (url_end - url_start), context->error);
        }
        if (*cursor == ',') {
            cursor++;
        }
    }
    return NULL;
}

static xmlNode *find_descendant(xmlNode *node, const char *name) {
    for (xmlNode *child = node->children; child != NULL; child = child->next) {
        if (node_is(child, name)) {
            return child;
        }
        xmlNode *found = find_descendant(child, name);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static char *candidate_image_source(HtmlContext *context, xmlNode *candidate) {
    xmlChar *source = NULL;
    if (node_is(candidate, "img")) {
        source = node_attribute(candidate, "src");
    } else if (node_is(candidate, "image")) {
        source = node_attribute(candidate, "href");
    }
    char *value = NULL;
    if (source != NULL) {
        value = baca_strdup((const char *) source, context->error);
    } else {
        xmlChar *srcset = node_attribute(candidate, "srcset");
        if (srcset != NULL) {
            value = srcset_first(context, srcset);
            xmlFree(srcset);
        }
    }
    xmlFree(source);
    return value;
}

static char *image_source(HtmlContext *context, xmlNode *node, xmlNode **source_node) {
    xmlNode *candidate = node;
    char *value = NULL;
    if (node_is(node, "picture")) {
        candidate = find_descendant(node, "img");
        if (candidate != NULL) {
            value = candidate_image_source(context, candidate);
        }
        if (value == NULL || value[0] == '\0') {
            free(value);
            candidate = find_descendant(node, "source");
            value = candidate == NULL ? NULL : candidate_image_source(context, candidate);
        }
    } else {
        value = candidate_image_source(context, candidate);
    }
    if (source_node != NULL) {
        *source_node = candidate;
    }
    return value;
}

static bool add_descendant_image_anchors(HtmlContext *context, xmlNode *node, const xmlChar *primary,
                                         bool *added) {
    for (xmlNode *child = node->children; child != NULL; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) {
            continue;
        }
        xmlChar *id = node_attribute(child, "id");
        xmlChar *name = node_is(child, "a") ? node_attribute(child, "name") : NULL;
        xmlChar *values[] = {id, name};
        for (size_t index = 0; index < BACA_ARRAY_LEN(values); index++) {
            if (values[index] != NULL && values[index][0] != '\0' &&
                (primary == NULL || xmlStrcmp(values[index], primary) != 0)) {
                if (!add_anchor(context, (const char *) values[index])) {
                    xmlFree(id);
                    xmlFree(name);
                    return false;
                }
                *added = true;
            }
        }
        xmlFree(id);
        xmlFree(name);
        if (!add_descendant_image_anchors(context, child, primary, added)) {
            return false;
        }
    }
    return true;
}

static bool emit_image(HtmlContext *context, xmlNode *node, const char *link) {
    xmlNode *source_node = node;
    char *source = image_source(context, node, &source_node);
    if (source == NULL || source[0] == '\0') {
        free(source);
        if (baca_error_is_set(context->error)) {
            return false;
        }
        return record_node_anchors(context, node);
    }
    if (!flush_text(context)) {
        free(source);
        return false;
    }

    char *resolved = resolve_uri(context, source);
    free(source);
    if (resolved == NULL) {
        return false;
    }
    xmlChar *alt_value = node_attribute(source_node, "alt");
    if (alt_value == NULL) {
        alt_value = node_attribute(source_node, "aria-label");
    }
    if (alt_value == NULL) {
        alt_value = node_attribute(source_node, "title");
    }
    if (alt_value == NULL && node_is(node, "picture")) {
        xmlNode *fallback = find_descendant(node, "img");
        if (fallback != NULL) {
            alt_value = node_attribute(fallback, "alt");
        }
    }
    bool had_alt_value = alt_value != NULL;
    char *alt = alt_value == NULL ? NULL : baca_strdup((const char *) alt_value, context->error);
    xmlFree(alt_value);
    if (had_alt_value && alt == NULL) {
        free(resolved);
        return false;
    }

    xmlChar *node_id = node_attribute(node, "id");
    xmlChar *node_name = node_attribute(node, "name");
    xmlChar *source_id = source_node == node ? NULL : node_attribute(source_node, "id");
    xmlChar *source_name = source_node == node ? NULL : node_attribute(source_node, "name");
    xmlChar *anchor_values[] = {node_id, node_name, source_id, source_name};
    const xmlChar *primary = NULL;
    for (size_t index = 0; index < BACA_ARRAY_LEN(anchor_values); index++) {
        if (anchor_values[index] != NULL && anchor_values[index][0] != '\0') {
            primary = anchor_values[index];
            break;
        }
    }
    char *anchor = NULL;
    if (primary != NULL) {
        anchor = anchor_target(context, (const char *) primary);
        if (anchor == NULL) {
            xmlFree(node_id);
            xmlFree(node_name);
            xmlFree(source_id);
            xmlFree(source_name);
            free(resolved);
            free(alt);
            return false;
        }
    }
    bool added_secondary = false;
    for (size_t index = 0; index < BACA_ARRAY_LEN(anchor_values); index++) {
        if (anchor_values[index] != NULL && anchor_values[index][0] != '\0' &&
            (primary == NULL || xmlStrcmp(anchor_values[index], primary) != 0)) {
            if (!add_anchor(context, (const char *) anchor_values[index])) {
                xmlFree(node_id);
                xmlFree(node_name);
                xmlFree(source_id);
                xmlFree(source_name);
                free(resolved);
                free(alt);
                free(anchor);
                return false;
            }
            added_secondary = true;
        }
    }
    if (!add_descendant_image_anchors(context, node, primary, &added_secondary)) {
        xmlFree(node_id);
        xmlFree(node_name);
        xmlFree(source_id);
        xmlFree(source_name);
        free(resolved);
        free(alt);
        free(anchor);
        return false;
    }
    if (added_secondary && !flush_text(context)) {
        xmlFree(node_id);
        xmlFree(node_name);
        xmlFree(source_id);
        xmlFree(source_name);
        free(resolved);
        free(alt);
        free(anchor);
        return false;
    }
    xmlFree(node_id);
    xmlFree(node_name);
    xmlFree(source_id);
    xmlFree(source_name);

    char *owned_link = link == NULL ? NULL : baca_strdup(link, context->error);
    if (link != NULL && owned_link == NULL) {
        free(resolved);
        free(alt);
        free(anchor);
        return false;
    }
    char *section_id = baca_strdup(context->section_id, context->error);
    if (section_id == NULL) {
        free(resolved);
        free(alt);
        free(anchor);
        free(owned_link);
        return false;
    }
    BacaBlock block = {
        .kind = BACA_BLOCK_IMAGE,
        .section_id = section_id,
        .value.image = {
            .uri = resolved,
            .alt = alt,
            .anchor = anchor,
            .link = owned_link,
            .page_index = -1,
        },
    };
    if (!baca_document_add_image_block(context->document, &block, context->error)) {
        baca_document_block_free(&block);
        return false;
    }
    return true;
}

static bool walk_children(HtmlContext *context, xmlNode *node, BacaTextStyle style, const char *link,
                          bool suppress_blocks) {
    for (xmlNode *child = node->children; child != NULL; child = child->next) {
        if (!walk_node(context, child, style, link, suppress_blocks)) {
            return false;
        }
    }
    return true;
}

static unsigned heading_level(const xmlNode *node) {
    if (node->type != XML_ELEMENT_NODE || xmlStrlen(node->name) != 2 || node->name[0] != 'h' ||
        node->name[1] < '1' || node->name[1] > '6') {
        return 0;
    }
    return (unsigned) (node->name[1] - '0');
}

static bool is_boundary_element(const xmlNode *node) {
    static const char *const names[] = {
        "p",       "div",     "section", "article", "aside",   "header", "footer",  "main",
        "nav",     "address", "blockquote", "figure", "figcaption", "details", "summary", "dt",
        "dd",
    };
    for (size_t index = 0; index < BACA_ARRAY_LEN(names); index++) {
        if (node_is(node, names[index])) {
            return true;
        }
    }
    return false;
}

static BacaTextStyle element_style(const xmlNode *node, BacaTextStyle style) {
    if (node_is(node, "strong") || node_is(node, "b")) {
        style = (BacaTextStyle) (style | BACA_STYLE_BOLD);
    }
    if (node_is(node, "em") || node_is(node, "i") || node_is(node, "cite") || node_is(node, "dfn")) {
        style = (BacaTextStyle) (style | BACA_STYLE_ITALIC);
    }
    if (node_is(node, "u") || node_is(node, "ins")) {
        style = (BacaTextStyle) (style | BACA_STYLE_UNDERLINE);
    }
    if (node_is(node, "code") || node_is(node, "samp") || node_is(node, "kbd") || node_is(node, "tt") ||
        node_is(node, "pre")) {
        style = (BacaTextStyle) (style | BACA_STYLE_CODE);
    }
    return style;
}

static bool walk_list(HtmlContext *context, xmlNode *node, BacaTextStyle style, const char *link) {
    if (!flush_text(context) || !record_node_anchors(context, node)) {
        return false;
    }
    bool ordered = node_is(node, "ol");
    long ordinal = 1;
    bool ordinal_exhausted = false;
    xmlChar *start = ordered ? node_attribute(node, "start") : NULL;
    if (start != NULL) {
        char *end = NULL;
        errno = 0;
        long parsed = strtol((const char *) start, &end, 10);
        if (errno == ERANGE) {
            xmlFree(start);
            baca_error_set(context->error, BACA_ERROR_CORRUPT, "ordered-list start is out of range");
            return false;
        }
        if (end != (char *) start && *end == '\0') {
            ordinal = parsed;
        }
    }
    xmlFree(start);

    if (context->list_depth == 256U) {
        baca_error_set(context->error, BACA_ERROR_CORRUPT, "HTML lists are nested too deeply");
        return false;
    }
    context->list_depth++;
    for (xmlNode *item = node->children; item != NULL; item = item->next) {
        if (!node_is(item, "li")) {
            continue;
        }
        if (ordered && ordinal_exhausted) {
            context->list_depth--;
            baca_error_set(context->error, BACA_ERROR_CORRUPT, "ordered-list ordinal exceeds LONG_MAX");
            return false;
        }
        if (!flush_text(context) || !record_node_anchors(context, item)) {
            context->list_depth--;
            return false;
        }
        for (unsigned depth = 1; depth < context->list_depth; depth++) {
            if (!append_chunk(context, "  ", 2, BACA_STYLE_NONE, NULL)) {
                context->list_depth--;
                return false;
            }
        }
        char marker[48];
        int marker_length = ordered ? snprintf(marker, sizeof(marker), "%ld. ", ordinal)
                                    : snprintf(marker, sizeof(marker), "- ");
        if (ordered) {
            if (ordinal == LONG_MAX) {
                ordinal_exhausted = true;
            } else {
                ordinal++;
            }
        }
        if (marker_length < 0 || (size_t) marker_length >= sizeof(marker) ||
            !append_chunk(context, marker, (size_t) marker_length, BACA_STYLE_NONE, NULL)) {
            context->list_depth--;
            return false;
        }
        for (xmlNode *child = item->children; child != NULL; child = child->next) {
            if (node_is(child, "ul") || node_is(child, "ol")) {
                if (!flush_text(context) || !walk_list(context, child, style, link)) {
                    context->list_depth--;
                    return false;
                }
            } else if (!walk_node(context, child, style, link, true)) {
                context->list_depth--;
                return false;
            }
        }
        if (!flush_text(context)) {
            context->list_depth--;
            return false;
        }
    }
    context->list_depth--;
    return true;
}

static bool walk_table_rows(HtmlContext *context, xmlNode *node, BacaTextStyle style, const char *link) {
    for (xmlNode *row = node->children; row != NULL; row = row->next) {
        if (node_is(row, "tr")) {
            if (!flush_text(context) || !record_node_anchors(context, row)) {
                return false;
            }
            bool first_cell = true;
            for (xmlNode *cell = row->children; cell != NULL; cell = cell->next) {
                if (!node_is(cell, "td") && !node_is(cell, "th")) {
                    continue;
                }
                if (!first_cell && !append_chunk(context, " | ", 3, BACA_STYLE_NONE, NULL)) {
                    return false;
                }
                first_cell = false;
                if (!record_node_anchors(context, cell)) {
                    return false;
                }
                BacaTextStyle cell_style = node_is(cell, "th") ?
                                               (BacaTextStyle) (style | BACA_STYLE_BOLD) :
                                               style;
                if (!walk_children(context, cell, cell_style, link, true)) {
                    return false;
                }
            }
            if (!flush_text(context)) {
                return false;
            }
        } else if (row->type == XML_ELEMENT_NODE && !walk_table_rows(context, row, style, link)) {
            return false;
        }
    }
    return true;
}

static bool walk_table(HtmlContext *context, xmlNode *node, BacaTextStyle style, const char *link) {
    if (!flush_text(context) || !record_node_anchors(context, node)) {
        return false;
    }
    for (xmlNode *child = node->children; child != NULL; child = child->next) {
        if (node_is(child, "caption")) {
            if (!walk_node(context, child, style, link, false)) {
                return false;
            }
        }
    }
    return walk_table_rows(context, node, style, link) && flush_text(context);
}

static bool walk_node(HtmlContext *context, xmlNode *node, BacaTextStyle style, const char *link,
                      bool suppress_blocks) {
    if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
        const char *content = (const char *) node->content;
        return content == NULL || append_text(context, content, strlen(content), style, link);
    }
    if (node->type != XML_ELEMENT_NODE) {
        return true;
    }
    if (node_is(node, "script") || node_is(node, "style") || node_is(node, "noscript") ||
        node_is(node, "template") || node_is(node, "head")) {
        return true;
    }

    if (node_is(node, "picture") || node_is(node, "img") || node_is(node, "image")) {
        return emit_image(context, node, link);
    }
    if (node_is(node, "source")) {
        return true;
    }
    if (node_is(node, "br")) {
        return record_node_anchors(context, node) && append_break(context, style, link);
    }
    if (node_is(node, "hr")) {
        return record_node_anchors(context, node) && flush_text(context);
    }
    if (node_is(node, "ul") || node_is(node, "ol")) {
        return walk_list(context, node, style, link);
    }
    if (node_is(node, "table")) {
        return walk_table(context, node, style, link);
    }

    unsigned level = heading_level(node);
    bool pre = node_is(node, "pre");
    bool boundary = !suppress_blocks && (level > 0 || pre || is_boundary_element(node));
    if (boundary && !flush_text(context)) {
        return false;
    }
    if (!record_node_anchors(context, node)) {
        return false;
    }

    BacaTextStyle child_style = element_style(node, style);
    char *resolved_link = NULL;
    const char *child_link = link;
    if (node_is(node, "a")) {
        xmlChar *href = node_attribute(node, "href");
        if (href != NULL && href[0] != '\0') {
            resolved_link = resolve_uri(context, (const char *) href);
            xmlFree(href);
            if (resolved_link == NULL) {
                return false;
            }
            child_link = resolved_link;
        } else {
            xmlFree(href);
        }
    }

    unsigned previous_heading = context->heading_level;
    bool previous_preformatted = context->preformatted;
    if (level > 0) {
        context->heading_level = level;
    }
    if (pre) {
        context->preformatted = true;
        context->pending_space = false;
    }
    bool success = walk_children(context, node, child_style, child_link, suppress_blocks);
    if (boundary && success) {
        success = flush_text(context);
    }
    context->heading_level = previous_heading;
    context->preformatted = previous_preformatted;
    free(resolved_link);
    return success;
}

static xmlNode *find_body(xmlNode *node) {
    if (node_is(node, "body")) {
        return node;
    }
    for (xmlNode *child = node == NULL ? NULL : node->children; child != NULL; child = child->next) {
        xmlNode *body = find_body(child);
        if (body != NULL) {
            return body;
        }
    }
    return NULL;
}

bool baca_html_append_section(BacaDocument *document, const char *html, size_t length, const char *section_id,
                               BacaError *error) {
    if (error != NULL) {
        baca_error_clear(error);
    }
    if (document == NULL || html == NULL || section_id == NULL || section_id[0] == '\0') {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document, HTML, and section id are required");
        return false;
    }
    if (length > BACA_HTML_MAX || length > (size_t) INT_MAX) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "HTML section exceeds the supported size limit");
        return false;
    }

    size_t first_block = document->block_count;
    HtmlContext context = {
        .document = document,
        .section_id = section_id,
        .error = error,
    };
    htmlDocPtr parsed = NULL;
    bool success = true;
    if (length > 0) {
        int options = HTML_PARSE_RECOVER | HTML_PARSE_NODEFDTD | HTML_PARSE_NONET | HTML_PARSE_NOERROR |
                      HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT | HTML_PARSE_BIG_LINES;
#if LIBXML_VERSION >= 21300
        options |= XML_PARSE_NO_XXE;
#endif
#if LIBXML_VERSION >= 21400
        options |= XML_PARSE_NO_SYS_CATALOG;
#endif
        parsed = htmlReadMemory(html, (int) length, section_id, NULL, options);
        if (parsed == NULL) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "could not parse HTML section %s", section_id);
            success = false;
        } else {
            xmlNode *root = xmlDocGetRootElement(parsed);
            xmlNode *body = find_body(root);
            xmlNode *content = body != NULL ? body : root;
            if (content != NULL) {
                success = record_node_anchors(&context, content) &&
                          walk_children(&context, content, BACA_STYLE_NONE, NULL, false) && flush_text(&context);
            }
        }
    }

    if (success) {
        BacaSection section = {
            .id = baca_strdup(section_id, error),
            .first_block = first_block,
            .block_count = document->block_count - first_block,
            .search_offset = 0,
            .source_size = length,
            .linear = true,
        };
        if (section.id == NULL || !baca_document_add_section(document, &section, error)) {
            free(section.id);
            success = false;
        }
    }

    xmlFreeDoc(parsed);
    context_text_free(&context);
    if (!success) {
        baca_document_rollback_blocks(document, first_block);
    }
    return success;
}
