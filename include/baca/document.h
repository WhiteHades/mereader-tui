#pragma once

#include "baca/common.h"

#define BACA_DOCUMENT_MAX_BLOCKS 100000U
#define BACA_DOCUMENT_MAX_SECTIONS 100000U
#define BACA_DOCUMENT_MAX_TOC_ENTRIES 100000U
#define BACA_DOCUMENT_MAX_SPANS_PER_BLOCK 100000U
#define BACA_DOCUMENT_MAX_ANCHORS_PER_BLOCK 100000U
#define BACA_DOCUMENT_MAX_RETAINED_BYTES (256U * 1024U * 1024U)

typedef enum BacaDocumentFormat : uint8_t {
    BACA_FORMAT_UNKNOWN = 0,
    BACA_FORMAT_EPUB,
    BACA_FORMAT_MOBI,
    BACA_FORMAT_AZW,
    BACA_FORMAT_PDF,
    BACA_FORMAT_IMAGE,
    BACA_FORMAT_COMIC,
    BACA_FORMAT_TEXT,
    BACA_FORMAT_FB2,
} BacaDocumentFormat;

typedef struct BacaMetadata {
    char *title;
    char *creator;
    char *description;
    char *publisher;
    char *date;
    char *language;
    char *format;
    char *identifier;
    char *source;
} BacaMetadata;

typedef enum BacaTextStyle : uint16_t {
    BACA_STYLE_NONE = 0,
    BACA_STYLE_BOLD = 1U << 0U,
    BACA_STYLE_ITALIC = 1U << 1U,
    BACA_STYLE_UNDERLINE = 1U << 2U,
    BACA_STYLE_CODE = 1U << 3U,
    BACA_STYLE_HEADING = 1U << 4U,
} BacaTextStyle;

typedef struct BacaTextSpan {
    size_t start;
    size_t end;
    BacaTextStyle style;
    char *link;
} BacaTextSpan;

typedef struct BacaAnchor {
    char *target;
    size_t offset;
} BacaAnchor;

typedef struct BacaTextBlock {
    char *text;
    BacaTextSpan *spans;
    size_t span_count;
    size_t span_capacity;
    BacaAnchor *anchors;
    size_t anchor_count;
    size_t anchor_capacity;
    unsigned heading_level;
    bool preformatted;
} BacaTextBlock;

typedef struct BacaImageBlock {
    char *uri;
    char *alt;
    char *anchor;
    char *link;
    int page_index;
    int intrinsic_width;
    int intrinsic_height;
    bool broken;
} BacaImageBlock;

typedef enum BacaBlockKind : uint8_t {
    BACA_BLOCK_TEXT = 0,
    BACA_BLOCK_IMAGE,
    BACA_BLOCK_PAGE_BREAK,
} BacaBlockKind;

typedef struct BacaBlock {
    BacaBlockKind kind;
    char *section_id;
    union {
        BacaTextBlock text;
        BacaImageBlock image;
    } value;
} BacaBlock;

typedef struct BacaTocEntry {
    char *label;
    char *target;
    unsigned depth;
    size_t section_index;
} BacaTocEntry;

typedef struct BacaSection {
    char *id;
    size_t first_block;
    size_t block_count;
    size_t search_offset;
    size_t source_size;
    bool linear;
} BacaSection;

typedef struct BacaResource {
    unsigned char *data;
    size_t length;
    char *mime_type;
} BacaResource;

struct BacaDocument;

typedef struct BacaDocumentOps {
    bool (*load_resource)(struct BacaDocument *document, const char *uri, BacaResource *resource,
                          BacaError *error);
    bool (*render_page)(struct BacaDocument *document, int page_index, double scale, BacaResource *resource,
                        BacaError *error);
    void (*close)(struct BacaDocument *document);
} BacaDocumentOps;

typedef struct BacaDocument {
    char *path;
    BacaDocumentFormat format;
    BacaMetadata metadata;
    BacaTocEntry *toc;
    size_t toc_count;
    size_t toc_capacity;
    BacaSection *sections;
    size_t section_count;
    size_t section_capacity;
    BacaBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    size_t retained_bytes;
    void *backend;
    const BacaDocumentOps *ops;
} BacaDocument;

/* document must point to {0}; close it before passing it here again. */
[[nodiscard]] bool baca_document_open(BacaDocument *document, const char *path, BacaError *error);
void baca_document_close(BacaDocument *document);
void baca_resource_free(BacaResource *resource);
/* resource must point to {0}; free it before passing it here again. */
[[nodiscard]] bool baca_document_load_resource(BacaDocument *document, const char *uri, BacaResource *resource,
                                               BacaError *error);
/* Disabled probing performs no resource loads and leaves image blocks as placeholders. */
void baca_document_probe_images(BacaDocument *document, bool enabled);
[[nodiscard]] const char *baca_document_format_name(BacaDocumentFormat format);
[[nodiscard]] const BacaTocEntry *baca_document_current_toc(const BacaDocument *document, const char *target);
[[nodiscard]] bool baca_document_add_text_block(BacaDocument *document, BacaBlock *block, BacaError *error);
[[nodiscard]] bool baca_document_add_image_block(BacaDocument *document, BacaBlock *block, BacaError *error);
[[nodiscard]] bool baca_document_add_toc(BacaDocument *document, const char *label, const char *target,
                                         unsigned depth, BacaError *error);
[[nodiscard]] bool baca_document_add_section(BacaDocument *document, BacaSection *section, BacaError *error);
