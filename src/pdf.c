#include "baca/document_backend.h"
#include "baca/graphics.h"

#include <cairo.h>
#include <limits.h>
#include <math.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#include <poppler.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BACA_PDF_MAX_PAGES = 10000,
    BACA_PDF_MAX_TEXT_PER_PAGE = 1024 * 1024,
    BACA_PDF_MAX_TOTAL_TEXT = 16 * 1024 * 1024,
    BACA_PDF_MAX_TOTAL_LINKS = 100000,
    BACA_PDF_MAX_TOTAL_LINK_TARGET_BYTES = 8 * 1024 * 1024,
    BACA_PDF_MAX_OUTLINE_DEPTH = 64,
    BACA_PDF_MAX_OUTLINE_NODES = 10000,
    BACA_PDF_MAX_METADATA_BYTES = 1024 * 1024,
    BACA_PDF_ID_LENGTH = 32,
};

typedef struct BacaPdfBackend {
    PopplerDocument *document;
    size_t page_count;
    size_t text_bytes;
    size_t link_count;
    size_t link_target_bytes;
} BacaPdfBackend;

typedef struct BacaPdfPngBuffer {
    BacaString data;
    BacaError error;
} BacaPdfPngBuffer;

static double pdf_clamp_unit(double value);

static char *pdf_page_target(size_t page_index, BacaError *error) {
    char target[64] = {0};
    const int length = snprintf(target, sizeof(target), "pdf://page/%zu", page_index);
    if (length <= 0 || (size_t)length >= sizeof(target)) {
        baca_error_set(error, BACA_ERROR_INTERNAL, "could not format PDF page target");
        return NULL;
    }
    return baca_strdup(target, error);
}

static char *pdf_copy_link_target(BacaPdfBackend *backend, const char *target, BacaError *error) {
    const size_t length = strnlen(target, BACA_PDF_MAX_METADATA_BYTES + 1U);
    if (length > BACA_PDF_MAX_METADATA_BYTES) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF link target exceeds the supported size limit");
        return NULL;
    }
    const size_t retained = length + 1U;
    if (backend->link_target_bytes > BACA_PDF_MAX_TOTAL_LINK_TARGET_BYTES ||
        retained > BACA_PDF_MAX_TOTAL_LINK_TARGET_BYTES - backend->link_target_bytes) {
        baca_error_set(error, BACA_ERROR_CORRUPT,
                       "PDF retained link target data exceeds the supported size limit");
        return NULL;
    }
    char *copy = baca_strndup(target, length, error);
    if (copy != NULL) {
        backend->link_target_bytes += retained;
    }
    return copy;
}

static char *pdf_copy_uri_target(BacaPdfBackend *backend, const char *target, BacaError *error) {
    const size_t length = strnlen(target, BACA_PDF_MAX_METADATA_BYTES + 1U);
    if (length > BACA_PDF_MAX_METADATA_BYTES) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF link target exceeds the supported size limit");
        return NULL;
    }
    if (g_utf8_validate(target, (gssize)length, NULL)) {
        return pdf_copy_link_target(backend, target, error);
    }
    gchar *valid = g_utf8_make_valid(target, (gssize)length);
    if (valid == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not normalize PDF URI target");
        return NULL;
    }
    char *copy = pdf_copy_link_target(backend, valid, error);
    g_free(valid);
    return copy;
}

static char *pdf_copy_glib_string(gchar *value, BacaError *error) {
    if (value == NULL) {
        return NULL;
    }
    const size_t length = strlen(value);
    if (length == 0U) {
        g_free(value);
        return NULL;
    }
    if (length > BACA_PDF_MAX_METADATA_BYTES) {
        g_free(value);
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF metadata exceeds the supported size limit");
        return NULL;
    }
    gchar *valid = g_utf8_validate(value, (gssize)length, NULL) ? value : g_utf8_make_valid(value, (gssize)length);
    if (valid == NULL) {
        g_free(value);
        baca_error_set(error, BACA_ERROR_MEMORY, "could not normalize PDF metadata");
        return NULL;
    }
    char *copy = baca_strdup(valid, error);
    if (valid != value) {
        g_free(valid);
    }
    g_free(value);
    return copy;
}

static char *pdf_copy_glib_identifier(gchar *value, BacaError *error) {
    if (value == NULL) {
        return NULL;
    }
    char *copy = baca_reallocarray(NULL, BACA_PDF_ID_LENGTH + 1U, sizeof(*copy), error);
    if (copy != NULL) {
        for (size_t index = 0U; index < BACA_PDF_ID_LENGTH; ++index) {
            const unsigned char character = (unsigned char)value[index];
            if (character >= (unsigned char)'0' && character <= (unsigned char)'9') {
                copy[index] = (char)character;
            } else if (character >= (unsigned char)'a' && character <= (unsigned char)'f') {
                copy[index] = (char)character;
            } else if (character >= (unsigned char)'A' && character <= (unsigned char)'F') {
                copy[index] = (char)(character + ((unsigned char)'a' - (unsigned char)'A'));
            } else {
                free(copy);
                copy = NULL;
                baca_error_set(error, BACA_ERROR_CORRUPT, "PDF identifier is not hexadecimal");
                break;
            }
        }
        if (copy != NULL) {
            copy[BACA_PDF_ID_LENGTH] = '\0';
        }
    }
    g_free(value);
    return copy;
}

static char *pdf_datetime(GDateTime *datetime, BacaError *error) {
    if (datetime == NULL) {
        return NULL;
    }
    gchar *formatted = g_date_time_format_iso8601(datetime);
    g_date_time_unref(datetime);
    if (formatted == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not format PDF date");
        return NULL;
    }
    return pdf_copy_glib_string(formatted, error);
}

static bool pdf_load_metadata(BacaDocument *document, PopplerDocument *pdf, BacaError *error) {
    BacaMetadata metadata = {0};
    metadata.title = pdf_copy_glib_string(poppler_document_get_title(pdf), error);
    if (baca_error_is_set(error)) {
        goto fail;
    }
    metadata.author = pdf_copy_glib_string(poppler_document_get_author(pdf), error);
    if (baca_error_is_set(error)) {
        goto fail;
    }
    metadata.description = pdf_copy_glib_string(poppler_document_get_subject(pdf), error);
    if (baca_error_is_set(error)) {
        goto fail;
    }
    metadata.creator = pdf_copy_glib_string(poppler_document_get_creator(pdf), error);
    if (baca_error_is_set(error)) {
        goto fail;
    }
    metadata.producer = pdf_copy_glib_string(poppler_document_get_producer(pdf), error);
    if (baca_error_is_set(error)) {
        goto fail;
    }
    metadata.creation_date = pdf_datetime(poppler_document_get_creation_date_time(pdf), error);
    if (baca_error_is_set(error)) {
        goto fail;
    }
    metadata.modification_date = pdf_datetime(poppler_document_get_modification_date_time(pdf), error);
    if (baca_error_is_set(error)) {
        goto fail;
    }
    const char *generic_date = metadata.creation_date != NULL ? metadata.creation_date : metadata.modification_date;
    metadata.date = generic_date == NULL ? NULL : baca_strdup(generic_date, error);
    metadata.format = baca_strdup("PDF", error);
    if (baca_error_is_set(error) || metadata.format == NULL) {
        goto fail;
    }
    gchar *permanent_id = NULL;
    gchar *update_id = NULL;
    if (poppler_document_get_id(pdf, &permanent_id, &update_id) != FALSE) {
        metadata.identifier = pdf_copy_glib_identifier(permanent_id, error);
        g_free(update_id);
        if (baca_error_is_set(error)) {
            goto fail;
        }
    }
    document->metadata = metadata;
    return baca_document_account_metadata(document, error);

fail:
    free(metadata.title);
    free(metadata.author);
    free(metadata.creator);
    free(metadata.description);
    free(metadata.publisher);
    free(metadata.producer);
    free(metadata.date);
    free(metadata.creation_date);
    free(metadata.modification_date);
    free(metadata.language);
    free(metadata.format);
    free(metadata.identifier);
    free(metadata.source);
    return false;
}

typedef struct BacaPdfDestination {
    int page;
    double y;
    bool has_y;
} BacaPdfDestination;

static bool pdf_dimension_equal(double left, double right) {
    const double scale = fmax(fmax(fabs(left), fabs(right)), 1.0);
    return fabs(left - right) <= scale * 1e-9;
}

static bool pdf_destination(BacaPdfBackend *backend, const PopplerDest *destination,
                            BacaPdfDestination *result) {
    if (destination == NULL) {
        return false;
    }
    PopplerDest *owned = NULL;
    if (destination->type == POPPLER_DEST_NAMED && destination->named_dest != NULL) {
        owned = poppler_document_find_dest(backend->document, destination->named_dest);
        if (owned == NULL) {
            return false;
        }
        destination = owned;
    }

    const int page = destination->page_num - 1;
    if (page < 0 || (size_t)page >= backend->page_count) {
        if (owned != NULL) {
            poppler_dest_free(owned);
        }
        return false;
    }
    *result = (BacaPdfDestination){.page = page};
    const bool supports_top = destination->type == POPPLER_DEST_XYZ ||
                              destination->type == POPPLER_DEST_FITH ||
                              destination->type == POPPLER_DEST_FITBH ||
                              destination->type == POPPLER_DEST_FITR;
    const bool has_top = supports_top &&
                         (destination->type == POPPLER_DEST_FITR || destination->change_top != 0U);
    if (has_top && isfinite(destination->top)) {
        PopplerPage *target_page = poppler_document_get_page(backend->document, page);
        if (target_page != NULL) {
            double width = 0.0;
            double height = 0.0;
            PopplerRectangle crop = {0};
            poppler_page_get_size(target_page, &width, &height);
            poppler_page_get_crop_box(target_page, &crop);
            g_object_unref(target_page);
            const double crop_width = fabs(crop.x2 - crop.x1);
            const double crop_height = fabs(crop.y2 - crop.y1);
            const bool normal = isfinite(width) && isfinite(height) && isfinite(crop_width) &&
                                isfinite(crop_height) && crop_width > 0.0 && crop_height > 0.0 &&
                                pdf_dimension_equal(width, crop_width) &&
                                pdf_dimension_equal(height, crop_height);
            const bool quarter_turned = isfinite(width) && isfinite(height) && isfinite(crop_width) &&
                                        isfinite(crop_height) && crop_width > 0.0 && crop_height > 0.0 &&
                                        pdf_dimension_equal(width, crop_height) &&
                                        pdf_dimension_equal(height, crop_width) && !normal;
            if (normal && !quarter_turned) {
                const double crop_top = fmax(crop.y1, crop.y2);
                result->y = pdf_clamp_unit((crop_top - destination->top) / crop_height);
                result->has_y = true;
            }
        }
    }
    if (owned != NULL) {
        poppler_dest_free(owned);
    }
    return true;
}

static bool pdf_format_destination_target(const BacaPdfDestination *destination, char target[128],
                                          BacaError *error) {
    int length = 0;
    if (destination->has_y) {
        char y[G_ASCII_DTOSTR_BUF_SIZE] = {0};
        (void)g_ascii_dtostr(y, sizeof(y), destination->y);
        length = snprintf(target, 128U, "pdf://page/%d#y=%s", destination->page, y);
    } else {
        length = snprintf(target, 128U, "pdf://page/%d", destination->page);
    }
    if (length <= 0 || length >= 128) {
        baca_error_set(error, BACA_ERROR_INTERNAL, "could not format PDF destination target");
        return false;
    }
    return true;
}

static int pdf_named_action_page(const char *name, int current_page, size_t page_count) {
    if (name == NULL) {
        return -1;
    }
    if (g_ascii_strcasecmp(name, "FirstPage") == 0) {
        return 0;
    }
    if (g_ascii_strcasecmp(name, "LastPage") == 0) {
        return page_count == 0U || page_count > (size_t)INT_MAX ? -1 : (int)page_count - 1;
    }
    if (g_ascii_strcasecmp(name, "NextPage") == 0) {
        return current_page < 0 || (size_t)current_page + 1U >= page_count ? -1 : current_page + 1;
    }
    if (g_ascii_strcasecmp(name, "PrevPage") == 0) {
        return current_page > 0 ? current_page - 1 : -1;
    }
    return -1;
}

static char *pdf_action_target(BacaPdfBackend *backend, const PopplerAction *action, int current_page,
                               BacaError *error) {
    if (action == NULL) {
        return NULL;
    }
    BacaPdfDestination destination = {.page = -1};
    if (action->type == POPPLER_ACTION_GOTO_DEST) {
        if (!pdf_destination(backend, action->goto_dest.dest, &destination)) {
            return NULL;
        }
    } else if (action->type == POPPLER_ACTION_NAMED) {
        destination.page = pdf_named_action_page(action->named.named_dest, current_page, backend->page_count);
    } else if (action->type == POPPLER_ACTION_URI && action->uri.uri != NULL) {
        return pdf_copy_uri_target(backend, action->uri.uri, error);
    }
    if (destination.page < 0 || (size_t)destination.page >= backend->page_count) {
        return NULL;
    }
    char target[128] = {0};
    if (!pdf_format_destination_target(&destination, target, error)) {
        return NULL;
    }
    return pdf_copy_link_target(backend, target, error);
}

static double pdf_clamp_unit(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

static bool pdf_add_link(BacaPdfBackend *backend, BacaImageBlock *image, const PopplerLinkMapping *mapping,
                          double page_width, double page_height, int page_index, BacaError *error) {
    if (image->link_count >= BACA_DOCUMENT_MAX_LINKS_PER_IMAGE ||
        backend->link_count >= BACA_PDF_MAX_TOTAL_LINKS) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF contains too many links");
        return false;
    }

    const double left = fmin(mapping->area.x1, mapping->area.x2);
    const double right = fmax(mapping->area.x1, mapping->area.x2);
    const double bottom = fmin(mapping->area.y1, mapping->area.y2);
    const double top = fmax(mapping->area.y1, mapping->area.y2);
    if (!isfinite(left) || !isfinite(right) || !isfinite(bottom) || !isfinite(top)) {
        return true;
    }
    const double x1 = pdf_clamp_unit(left / page_width);
    const double x2 = pdf_clamp_unit(right / page_width);
    const double y1 = pdf_clamp_unit((page_height - top) / page_height);
    const double y2 = pdf_clamp_unit((page_height - bottom) / page_height);
    if (!(x2 > x1) || !(y2 > y1)) {
        return true;
    }

    char *target = pdf_action_target(backend, mapping->action, page_index, error);
    if (target == NULL) {
        return !baca_error_is_set(error);
    }

    BacaError reserve_error = {0};
    BacaImageLink *links = baca_array_reserve(image->links, &image->link_capacity, sizeof(*image->links),
                                              image->link_count + 1U, &reserve_error);
    if (baca_error_is_set(&reserve_error)) {
        free(target);
        if (error != NULL) {
            *error = reserve_error;
        }
        return false;
    }
    image->links = links;
    image->links[image->link_count++] = (BacaImageLink){
        .x = x1,
        .y = y1,
        .width = x2 - x1,
        .height = y2 - y1,
        .target = target,
    };
    ++backend->link_count;
    return true;
}

static bool pdf_extract_links(BacaPdfBackend *backend, PopplerPage *page, BacaImageBlock *image,
                              double page_width, double page_height, int page_index, BacaError *error) {
    GList *mappings = poppler_page_get_link_mapping(page);
    bool success = true;
    for (GList *item = mappings; item != NULL; item = item->next) {
        const PopplerLinkMapping *mapping = item->data;
        if (mapping != NULL && !pdf_add_link(backend, image, mapping, page_width, page_height, page_index, error)) {
            success = false;
            break;
        }
    }
    poppler_page_free_link_mapping(mappings);
    return success;
}

static bool pdf_intrinsic_size(double width, double height, int *intrinsic_width, int *intrinsic_height,
                               BacaError *error) {
    if (!isfinite(width) || !isfinite(height) || !(width > 0.0) || !(height > 0.0) ||
        width > 1000000.0 || height > 1000000.0) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF page has invalid or unsupported dimensions");
        return false;
    }
    const double longest = fmax(width, height);
    const double scale = longest > (double)BACA_GRAPHICS_MAX_SOURCE_DIMENSION
                             ? (double)BACA_GRAPHICS_MAX_SOURCE_DIMENSION / longest
                             : 1.0;
    const double scaled_width = fmax(1.0, round(width * scale));
    const double scaled_height = fmax(1.0, round(height * scale));
    *intrinsic_width = (int)scaled_width;
    *intrinsic_height = (int)scaled_height;
    return true;
}

static char *pdf_extract_text(BacaPdfBackend *backend, PopplerPage *page, BacaError *error) {
    gchar *raw = poppler_page_get_text(page);
    if (raw == NULL || raw[0] == '\0') {
        g_free(raw);
        return baca_strdup("", error);
    }
    const size_t raw_length = strlen(raw);
    if (raw_length > BACA_PDF_MAX_TEXT_PER_PAGE ||
        raw_length > BACA_PDF_MAX_TOTAL_TEXT - backend->text_bytes) {
        g_free(raw);
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF extracted text exceeds the supported size limit");
        return NULL;
    }
    gchar *valid = g_utf8_validate(raw, (gssize)raw_length, NULL) ? raw : g_utf8_make_valid(raw, (gssize)raw_length);
    if (valid == NULL) {
        g_free(raw);
        baca_error_set(error, BACA_ERROR_MEMORY, "could not normalize PDF text");
        return NULL;
    }
    const size_t valid_length = strlen(valid);
    if (valid_length > BACA_PDF_MAX_TEXT_PER_PAGE ||
        valid_length > BACA_PDF_MAX_TOTAL_TEXT - backend->text_bytes) {
        if (valid != raw) {
            g_free(valid);
        }
        g_free(raw);
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF extracted text exceeds the supported size limit");
        return NULL;
    }
    char *text = baca_strdup(valid, error);
    if (valid != raw) {
        g_free(valid);
    }
    g_free(raw);
    if (text != NULL) {
        backend->text_bytes += valid_length;
    }
    return text;
}

static bool pdf_append_link_line(BacaDocument *document, const char *section_id, const BacaImageBlock *image,
                                 BacaError *error) {
    if (image->link_count == 0U) {
        return true;
    }
    BacaString text = {0};
    BacaTextSpan *spans = calloc(image->link_count, sizeof(*spans));
    if (spans == NULL) {
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate PDF text links");
        return false;
    }
    size_t span_count = 0U;
    for (size_t index = 0U; index < image->link_count; ++index) {
        const BacaImageLink *link = &image->links[index];
        char prefix[48] = {0};
        const int prefix_length = snprintf(prefix, sizeof(prefix), "[%zu] ", index + 1U);
        const char *label = link->target;
        char page_label[64] = {0};
        if (strncmp(link->target, "pdf://page/", 11U) == 0) {
            char *end = NULL;
            const unsigned long page = strtoul(link->target + 11U, &end, 10);
            if (end != link->target + 11U && (*end == '\0' || strncmp(end, "#y=", 3U) == 0)) {
                (void)snprintf(page_label, sizeof(page_label), "Page %lu", page + 1UL);
                label = page_label;
            }
        }
        if (prefix_length <= 0 || (size_t)prefix_length >= sizeof(prefix) ||
            (index > 0U && !baca_string_append_char(&text, '\n', error)) ||
            !baca_string_append_n(&text, prefix, (size_t)prefix_length, error)) {
            goto fail;
        }
        const size_t start = text.length;
        if (!baca_string_append(&text, label, error)) {
            goto fail;
        }
        spans[span_count].start = start;
        spans[span_count].end = text.length;
        spans[span_count].style = BACA_STYLE_UNDERLINE;
        spans[span_count].link = baca_strdup(link->target, error);
        if (spans[span_count].link == NULL) {
            goto fail;
        }
        ++span_count;
    }

    BacaBlock block = {
        .kind = BACA_BLOCK_TEXT,
        .presentation = BACA_PRESENTATION_REFLOW,
        .section_id = baca_strdup(section_id, error),
        .value.text = {
            .text = baca_string_take(&text),
            .spans = spans,
            .span_count = span_count,
            .span_capacity = image->link_count,
        },
    };
    if (block.section_id == NULL || block.value.text.text == NULL ||
        !baca_document_add_text_block(document, &block, error)) {
        baca_document_block_free(&block);
        return false;
    }
    return true;

fail:
    baca_string_free(&text);
    for (size_t index = 0U; index < span_count; ++index) {
        free(spans[index].link);
    }
    free(spans);
    return false;
}

static bool pdf_append_page(BacaDocument *document, BacaPdfBackend *backend, size_t page_index,
                            size_t *search_offset, BacaError *error) {
    PopplerPage *page = poppler_document_get_page(backend->document, (int)page_index);
    if (page == NULL) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF page %zu could not be loaded", page_index + 1U);
        return false;
    }
    double page_width = 0.0;
    double page_height = 0.0;
    poppler_page_get_size(page, &page_width, &page_height);
    int intrinsic_width = 0;
    int intrinsic_height = 0;
    if (!pdf_intrinsic_size(page_width, page_height, &intrinsic_width, &intrinsic_height, error)) {
        g_object_unref(page);
        return false;
    }

    char *target = pdf_page_target(page_index, error);
    char label[64] = {0};
    const int label_length = snprintf(label, sizeof(label), "Page %zu", page_index + 1U);
    BacaBlock image_block = {
        .kind = BACA_BLOCK_IMAGE,
        .presentation = BACA_PRESENTATION_FIXED,
        .section_id = target == NULL ? NULL : baca_strdup(target, error),
        .value.image = {
            .uri = target == NULL ? NULL : baca_strdup(target, error),
            .alt = label_length <= 0 || (size_t)label_length >= sizeof(label) ? NULL : baca_strdup(label, error),
            .anchor = target == NULL ? NULL : baca_strdup(target, error),
            .page_index = (int)page_index,
            .intrinsic_width = intrinsic_width,
            .intrinsic_height = intrinsic_height,
        },
    };
    if (target == NULL || image_block.section_id == NULL || image_block.value.image.uri == NULL ||
        image_block.value.image.alt == NULL || image_block.value.image.anchor == NULL ||
        !pdf_extract_links(backend, page, &image_block.value.image, page_width, page_height, (int)page_index,
                           error)) {
        free(target);
        baca_document_block_free(&image_block);
        g_object_unref(page);
        return false;
    }

    char *text = pdf_extract_text(backend, page, error);
    g_object_unref(page);
    if (text == NULL) {
        free(target);
        baca_document_block_free(&image_block);
        return false;
    }
    const size_t text_length = strlen(text);
    const size_t first_block = document->block_count;
    if (!baca_document_add_image_block(document, &image_block, error)) {
        free(text);
        free(target);
        baca_document_block_free(&image_block);
        return false;
    }
    BacaBlock text_block = {
        .kind = BACA_BLOCK_TEXT,
        .presentation = BACA_PRESENTATION_REFLOW,
        .section_id = baca_strdup(target, error),
        .value.text = {
            .text = text,
        },
    };
    if (text_block.section_id == NULL || !baca_document_add_text_block(document, &text_block, error)) {
        baca_document_block_free(&text_block);
        free(target);
        return false;
    }
    const BacaImageBlock *stored_image = &document->blocks[first_block].value.image;
    if (!pdf_append_link_line(document, target, stored_image, error)) {
        free(target);
        return false;
    }
    BacaSection section = {
        .id = target,
        .first_block = first_block,
        .block_count = document->block_count - first_block,
        .search_offset = *search_offset,
        .source_size = text_length,
        .linear = true,
    };
    if (!baca_document_add_section(document, &section, error)) {
        free(section.id);
        return false;
    }
    if (text_length > SIZE_MAX - *search_offset - 1U) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF search offsets overflow");
        return false;
    }
    *search_offset += text_length + 1U;
    return true;
}

static bool pdf_load_outline_iter(BacaDocument *document, BacaPdfBackend *backend, PopplerIndexIter *iterator,
                                  unsigned depth, size_t *node_count, BacaError *error) {
    if (depth >= BACA_PDF_MAX_OUTLINE_DEPTH) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF outline exceeds the supported depth limit");
        return false;
    }
    do {
        if (*node_count >= BACA_PDF_MAX_OUTLINE_NODES) {
            baca_error_set(error, BACA_ERROR_CORRUPT, "PDF outline exceeds the supported total node limit");
            return false;
        }
        ++*node_count;
        PopplerAction *action = poppler_index_iter_get_action(iterator);
        if (action != NULL) {
            char *target = action->any.title != NULL && action->any.title[0] != '\0'
                               ? pdf_action_target(backend, action, -1, error)
                               : NULL;
            if (target != NULL) {
                if (!baca_document_add_toc(document, action->any.title, target, depth, error)) {
                    free(target);
                    poppler_action_free(action);
                    return false;
                }
            }
            free(target);
            poppler_action_free(action);
            if (baca_error_is_set(error)) {
                return false;
            }
        }
        PopplerIndexIter *child = poppler_index_iter_get_child(iterator);
        if (child != NULL) {
            const bool loaded = pdf_load_outline_iter(document, backend, child, depth + 1U, node_count, error);
            poppler_index_iter_free(child);
            if (!loaded) {
                return false;
            }
        }
    } while (poppler_index_iter_next(iterator) != FALSE);
    return true;
}

static bool pdf_load_outline(BacaDocument *document, BacaPdfBackend *backend, BacaError *error) {
    PopplerIndexIter *iterator = poppler_index_iter_new(backend->document);
    if (iterator == NULL) {
        return true;
    }
    size_t node_count = 0U;
    const bool loaded = pdf_load_outline_iter(document, backend, iterator, 0U, &node_count, error);
    poppler_index_iter_free(iterator);
    return loaded && baca_document_index_toc_sections(document, error);
}

static cairo_status_t pdf_write_png(void *closure, const unsigned char *data, unsigned int length) {
    BacaPdfPngBuffer *buffer = closure;
    if ((size_t)length > BACA_GRAPHICS_MAX_INPUT_BYTES -
                             (buffer->data.length < BACA_GRAPHICS_MAX_INPUT_BYTES
                                  ? buffer->data.length
                                  : BACA_GRAPHICS_MAX_INPUT_BYTES)) {
        baca_error_set(&buffer->error, BACA_ERROR_MEMORY, "rendered PDF page exceeds the PNG size limit");
        return CAIRO_STATUS_WRITE_ERROR;
    }
    if (!baca_string_append_n(&buffer->data, (const char *)data, (size_t)length, &buffer->error)) {
        return CAIRO_STATUS_WRITE_ERROR;
    }
    return CAIRO_STATUS_SUCCESS;
}

static bool pdf_render_page(BacaDocument *document, int page_index, int width, int height, uint32_t background,
                             BacaResource *resource, BacaError *error) {
    (void)background;
    BacaPdfBackend *backend = document->backend;
    if (backend == NULL || page_index < 0 || (size_t)page_index >= backend->page_count ||
        width > BACA_GRAPHICS_MAX_SOURCE_DIMENSION || height > BACA_GRAPHICS_MAX_SOURCE_DIMENSION ||
        (size_t)width > BACA_GRAPHICS_MAX_RENDER_PIXELS / (size_t)height) {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "invalid or oversized PDF render request");
        return false;
    }
    PopplerPage *page = poppler_document_get_page(backend->document, page_index);
    if (page == NULL) {
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF page %d could not be loaded", page_index + 1);
        return false;
    }
    double page_width = 0.0;
    double page_height = 0.0;
    poppler_page_get_size(page, &page_width, &page_height);
    if (!isfinite(page_width) || !isfinite(page_height) || !(page_width > 0.0) || !(page_height > 0.0)) {
        g_object_unref(page);
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF page has invalid dimensions");
        return false;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *context = cairo_create(surface);
    cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    cairo_paint(context);
    cairo_scale(context, (double)width / page_width, (double)height / page_height);
    poppler_page_render(page, context);
    const cairo_status_t context_status = cairo_status(context);
    const cairo_status_t surface_status = cairo_surface_status(surface);
    cairo_destroy(context);
    g_object_unref(page);
    if (context_status != CAIRO_STATUS_SUCCESS || surface_status != CAIRO_STATUS_SUCCESS) {
        const cairo_status_t status = context_status != CAIRO_STATUS_SUCCESS ? context_status : surface_status;
        cairo_surface_destroy(surface);
        baca_error_set(error, BACA_ERROR_EXTERNAL, "could not render PDF page: %s", cairo_status_to_string(status));
        return false;
    }

    BacaPdfPngBuffer png = {0};
    const cairo_status_t status = cairo_surface_write_to_png_stream(surface, pdf_write_png, &png);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS || baca_error_is_set(&png.error)) {
        baca_string_free(&png.data);
        if (baca_error_is_set(&png.error)) {
            if (error != NULL) {
                *error = png.error;
            }
        } else {
            baca_error_set(error, BACA_ERROR_EXTERNAL, "could not encode PDF page: %s", cairo_status_to_string(status));
        }
        return false;
    }
    resource->length = png.data.length;
    resource->data = (unsigned char *)baca_string_take(&png.data);
    resource->mime_type = baca_strdup("image/png", error);
    if (resource->data == NULL || resource->mime_type == NULL) {
        baca_resource_free(resource);
        return false;
    }
    return true;
}

static void pdf_close(BacaDocument *document) {
    BacaPdfBackend *backend = document->backend;
    if (backend == NULL) {
        return;
    }
    if (backend->document != NULL) {
        g_object_unref(backend->document);
    }
    free(backend);
    document->backend = NULL;
}

static const BacaDocumentOps pdf_document_ops = {
    .render_page = pdf_render_page,
    .close = pdf_close,
};

bool baca_pdf_open(BacaDocument *document, const char *path, BacaError *error) {
    if (document == NULL || path == NULL || path[0] == '\0') {
        baca_error_set(error, BACA_ERROR_ARGUMENT, "document and PDF path are required");
        return false;
    }
    GError *gerror = NULL;
    gchar *uri = g_filename_to_uri(path, NULL, &gerror);
    if (uri == NULL) {
        baca_error_set(error, BACA_ERROR_IO, "could not create PDF file URI: %s",
                       gerror == NULL ? "invalid path" : gerror->message);
        g_clear_error(&gerror);
        return false;
    }
    PopplerDocument *pdf = poppler_document_new_from_file(uri, NULL, &gerror);
    g_free(uri);
    if (pdf == NULL) {
        if (gerror != NULL && gerror->domain == POPPLER_ERROR && gerror->code == POPPLER_ERROR_ENCRYPTED) {
            baca_error_set(error, BACA_ERROR_UNSUPPORTED,
                           "PDF password required; password-protected PDFs are not supported");
        } else {
            baca_error_set(error, BACA_ERROR_CORRUPT, "could not open PDF: %s",
                           gerror == NULL ? "invalid PDF data" : gerror->message);
        }
        g_clear_error(&gerror);
        return false;
    }
    g_clear_error(&gerror);

    const int page_count = poppler_document_get_n_pages(pdf);
    if (page_count <= 0) {
        g_object_unref(pdf);
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF contains no pages");
        return false;
    }
    if (page_count > BACA_PDF_MAX_PAGES) {
        g_object_unref(pdf);
        baca_error_set(error, BACA_ERROR_CORRUPT, "PDF page count exceeds the supported limit of %d",
                       BACA_PDF_MAX_PAGES);
        return false;
    }
    BacaPdfBackend *backend = calloc(1U, sizeof(*backend));
    if (backend == NULL) {
        g_object_unref(pdf);
        baca_error_set(error, BACA_ERROR_MEMORY, "could not allocate PDF backend");
        return false;
    }
    backend->document = pdf;
    backend->page_count = (size_t)page_count;
    document->backend = backend;
    document->ops = &pdf_document_ops;
    document->default_presentation = BACA_PRESENTATION_FIXED;

    if (!pdf_load_metadata(document, pdf, error)) {
        return false;
    }
    size_t search_offset = 0U;
    for (size_t page_index = 0U; page_index < backend->page_count; ++page_index) {
        if (!pdf_append_page(document, backend, page_index, &search_offset, error)) {
            return false;
        }
    }
    return pdf_load_outline(document, backend, error);
}
