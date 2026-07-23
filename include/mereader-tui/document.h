#pragma once

#include "mereader-tui/common.h"

#define MEREADER_TUI_DOCUMENT_MAX_BLOCKS 100000U
#define MEREADER_TUI_DOCUMENT_MAX_SECTIONS 100000U
#define MEREADER_TUI_DOCUMENT_MAX_TOC_ENTRIES 100000U
#define MEREADER_TUI_DOCUMENT_MAX_SPANS_PER_BLOCK 100000U
#define MEREADER_TUI_DOCUMENT_MAX_ANCHORS_PER_BLOCK 100000U
#define MEREADER_TUI_DOCUMENT_MAX_LINKS_PER_IMAGE 4096U
#define MEREADER_TUI_DOCUMENT_MAX_RETAINED_BYTES (256U * 1024U * 1024U)

typedef enum MereaderTuiDocumentFormat : uint8_t {
    MEREADER_TUI_FORMAT_UNKNOWN = 0,
    MEREADER_TUI_FORMAT_EPUB,
    MEREADER_TUI_FORMAT_MOBI,
    MEREADER_TUI_FORMAT_AZW,
    MEREADER_TUI_FORMAT_PDF,
    MEREADER_TUI_FORMAT_IMAGE,
    MEREADER_TUI_FORMAT_COMIC,
    MEREADER_TUI_FORMAT_TEXT,
    MEREADER_TUI_FORMAT_FB2,
} MereaderTuiDocumentFormat;

typedef struct MereaderTuiMetadata {
    char *title;
    char *author;
    char *creator;
    char *description;
    char *publisher;
    char *producer;
    char *date;
    char *creation_date;
    char *modification_date;
    char *language;
    char *format;
    char *identifier;
    char *source;
} MereaderTuiMetadata;

typedef enum MereaderTuiTextStyle : uint16_t {
    MEREADER_TUI_STYLE_NONE = 0,
    MEREADER_TUI_STYLE_BOLD = 1U << 0U,
    MEREADER_TUI_STYLE_ITALIC = 1U << 1U,
    MEREADER_TUI_STYLE_UNDERLINE = 1U << 2U,
    MEREADER_TUI_STYLE_CODE = 1U << 3U,
    MEREADER_TUI_STYLE_HEADING = 1U << 4U,
} MereaderTuiTextStyle;

typedef struct MereaderTuiTextSpan {
    size_t start;
    size_t end;
    MereaderTuiTextStyle style;
    char *link;
} MereaderTuiTextSpan;

typedef struct MereaderTuiAnchor {
    char *target;
    size_t offset;
} MereaderTuiAnchor;

typedef struct MereaderTuiTextBlock {
    char *text;
    MereaderTuiTextSpan *spans;
    size_t span_count;
    size_t span_capacity;
    MereaderTuiAnchor *anchors;
    size_t anchor_count;
    size_t anchor_capacity;
    unsigned heading_level;
    bool preformatted;
} MereaderTuiTextBlock;

typedef struct MereaderTuiImageLink {
    double x;
    double y;
    double width;
    double height;
    char *target;
} MereaderTuiImageLink;

typedef struct MereaderTuiImageBlock {
    char *uri;
    char *alt;
    char *anchor;
    char *link;
    int page_index;
    int intrinsic_width;
    int intrinsic_height;
    MereaderTuiImageLink *links;
    size_t link_count;
    size_t link_capacity;
    bool broken;
} MereaderTuiImageBlock;

typedef enum MereaderTuiPresentation : uint8_t {
    MEREADER_TUI_PRESENTATION_DEFAULT = 0,
    MEREADER_TUI_PRESENTATION_FIXED,
    MEREADER_TUI_PRESENTATION_REFLOW,
} MereaderTuiPresentation;

typedef enum MereaderTuiBlockKind : uint8_t {
    MEREADER_TUI_BLOCK_TEXT = 0,
    MEREADER_TUI_BLOCK_IMAGE,
    MEREADER_TUI_BLOCK_PAGE_BREAK,
} MereaderTuiBlockKind;

typedef struct MereaderTuiBlock {
    MereaderTuiBlockKind kind;
    MereaderTuiPresentation presentation;
    char *section_id;
    union {
        MereaderTuiTextBlock text;
        MereaderTuiImageBlock image;
    } value;
} MereaderTuiBlock;

typedef struct MereaderTuiTocEntry {
    char *label;
    char *target;
    unsigned depth;
    size_t section_index;
} MereaderTuiTocEntry;

typedef struct MereaderTuiSection {
    char *id;
    size_t first_block;
    size_t block_count;
    size_t search_offset;
    size_t source_size;
    bool linear;
} MereaderTuiSection;

typedef struct MereaderTuiResource {
    unsigned char *data;
    size_t length;
    char *mime_type;
} MereaderTuiResource;

struct MereaderTuiDocument;

typedef struct MereaderTuiDocumentOps {
    bool (*load_resource)(struct MereaderTuiDocument *document, const char *uri, MereaderTuiResource *resource,
                          MereaderTuiError *error);
    bool (*resource_size)(const struct MereaderTuiDocument *document, const char *uri, size_t *size);
    bool (*render_page)(struct MereaderTuiDocument *document, int page_index, int width, int height,
                        uint32_t background, MereaderTuiResource *resource, MereaderTuiError *error);
    void (*close)(struct MereaderTuiDocument *document);
} MereaderTuiDocumentOps;

typedef struct MereaderTuiDocument {
    char *path;
    uint64_t instance_id; /* Zero unless assigned by mereader_tui_document_open. */
    MereaderTuiDocumentFormat format;
    MereaderTuiMetadata metadata;
    MereaderTuiTocEntry *toc;
    size_t toc_count;
    size_t toc_capacity;
    MereaderTuiSection *sections;
    size_t section_count;
    size_t section_capacity;
    MereaderTuiBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    size_t retained_bytes;
    MereaderTuiPresentation default_presentation;
    void *backend;
    const MereaderTuiDocumentOps *ops;
} MereaderTuiDocument;

/* document must point to {0}; close it before passing it here again. */
[[nodiscard]] bool mereader_tui_document_open(MereaderTuiDocument *document, const char *path, MereaderTuiError *error);
void mereader_tui_document_close(MereaderTuiDocument *document);
void mereader_tui_resource_free(MereaderTuiResource *resource);
/* resource must point to {0}; free it before passing it here again. */
[[nodiscard]] bool mereader_tui_document_load_resource(MereaderTuiDocument *document, const char *uri, MereaderTuiResource *resource,
                                               MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_document_render_page(MereaderTuiDocument *document, int page_index, int width, int height,
                                             uint32_t background, MereaderTuiResource *resource, MereaderTuiError *error);
/* Disabled probing performs no resource loads and leaves image blocks as placeholders. */
void mereader_tui_document_probe_images(MereaderTuiDocument *document, bool enabled);
[[nodiscard]] const char *mereader_tui_document_format_name(MereaderTuiDocumentFormat format);
[[nodiscard]] const MereaderTuiTocEntry *mereader_tui_document_current_toc(const MereaderTuiDocument *document, const char *target);
[[nodiscard]] bool mereader_tui_document_add_text_block(MereaderTuiDocument *document, MereaderTuiBlock *block, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_document_add_image_block(MereaderTuiDocument *document, MereaderTuiBlock *block, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_document_add_toc(MereaderTuiDocument *document, const char *label, const char *target,
                                         unsigned depth, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_document_add_section(MereaderTuiDocument *document, MereaderTuiSection *section, MereaderTuiError *error);
