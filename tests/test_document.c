#include "test_support.h"

#include "baca/document.h"
#include "baca/document_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zip.h>

typedef struct FixtureMember {
    const char *name;
    const void *data;
    size_t length;
} FixtureMember;

static const char container_xml[] =
    "<?xml version=\"1.0\"?>"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
    "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

static const unsigned char pixel_png[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
    0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x1fU, 0x15U, 0xc4U,
    0x89U, 0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x44U, 0x41U,
    0x54U, 0x08U, 0xd7U, 0x63U, 0xf8U, 0xcfU, 0xc0U, 0xf0U,
    0x1fU, 0x00U, 0x05U, 0x00U, 0x01U, 0xffU, 0x89U, 0x99U,
    0x3dU, 0x1dU, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U,
    0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
};

static bool buffer_contains(const unsigned char *data, size_t length, const void *needle, size_t needle_length) {
    if (needle_length == 0U) {
        return true;
    }
    if (data == NULL || needle == NULL || needle_length > length) {
        return false;
    }
    for (size_t offset = 0U; offset <= length - needle_length; offset++) {
        if (memcmp(data + offset, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool zip_add_bytes(zip_t *archive, const char *name, const void *data, size_t length, bool stored,
                          zip_int64_t *added_index) {
    void *owned = malloc(length == 0U ? 1U : length);
    if (owned == NULL) {
        return false;
    }
    if (length != 0U) {
        memcpy(owned, data, length);
    }
    zip_source_t *source = zip_source_buffer(archive, owned, length, 1);
    if (source == NULL) {
        free(owned);
        return false;
    }
    zip_int64_t index = zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
    if (index < 0) {
        zip_source_free(source);
        return false;
    }
    if (stored && zip_set_file_compression(archive, (zip_uint64_t)index, ZIP_CM_STORE, 0U) != 0) {
        return false;
    }
    if (added_index != NULL) {
        *added_index = index;
    }
    return true;
}

static bool close_archive(zip_t *archive) {
    if (zip_close(archive) == 0) {
        return true;
    }
    fprintf(stderr, "EPUB fixture close: %s\n", zip_strerror(archive));
    zip_discard(archive);
    return false;
}

static bool create_epub(const char *relative, const char *opf, const FixtureMember *members, size_t member_count,
                        char **output_path) {
    char *path = baca_test_path(relative);
    if (path == NULL) {
        return false;
    }
    BacaError error = {0};
    char *directory = baca_path_dirname(path, &error);
    if (directory == NULL || !baca_mkdirs(directory, &error)) {
        fprintf(stderr, "EPUB fixture directory: %s\n", error.message);
        free(directory);
        free(path);
        return false;
    }
    free(directory);

    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    if (archive == NULL) {
        fprintf(stderr, "EPUB fixture open failed with libzip error %d\n", zip_error);
        free(path);
        return false;
    }
    bool built = zip_add_bytes(archive, "mimetype", "application/epub+zip", 20U, true, NULL) &&
                 zip_add_bytes(archive, "META-INF/container.xml", container_xml, strlen(container_xml), false,
                               NULL) &&
                 zip_add_bytes(archive, "OEBPS/content.opf", opf, strlen(opf), false, NULL);
    for (size_t index = 0U; index < member_count && built; index++) {
        built = zip_add_bytes(archive, members[index].name, members[index].data, members[index].length, false, NULL);
    }
    if (!built) {
        fprintf(stderr, "EPUB fixture add: %s\n", zip_strerror(archive));
        zip_discard(archive);
        free(path);
        return false;
    }
    if (!close_archive(archive)) {
        free(path);
        return false;
    }
    *output_path = path;
    return true;
}

static bool document_has_anchor(const BacaDocument *document, const char *target) {
    for (size_t block_index = 0U; block_index < document->block_count; block_index++) {
        const BacaBlock *block = &document->blocks[block_index];
        if (block->kind == BACA_BLOCK_TEXT) {
            for (size_t anchor_index = 0U; anchor_index < block->value.text.anchor_count; anchor_index++) {
                if (strcmp(block->value.text.anchors[anchor_index].target, target) == 0) {
                    return true;
                }
            }
        } else if (block->kind == BACA_BLOCK_IMAGE && block->value.image.anchor != NULL &&
                   strcmp(block->value.image.anchor, target) == 0) {
            return true;
        }
    }
    return false;
}

static const BacaTextBlock *find_text(const BacaDocument *document, const char *text) {
    for (size_t index = 0U; index < document->block_count; index++) {
        if (document->blocks[index].kind == BACA_BLOCK_TEXT && document->blocks[index].value.text.text != NULL &&
            strcmp(document->blocks[index].value.text.text, text) == 0) {
            return &document->blocks[index].value.text;
        }
    }
    return NULL;
}

static const BacaImageBlock *find_image(const BacaDocument *document, const char *alt) {
    for (size_t index = 0U; index < document->block_count; index++) {
        if (document->blocks[index].kind == BACA_BLOCK_IMAGE && document->blocks[index].value.image.alt != NULL &&
            strcmp(document->blocks[index].value.image.alt, alt) == 0) {
            return &document->blocks[index].value.image;
        }
    }
    return NULL;
}

static bool text_has_span(const BacaTextBlock *text, BacaTextStyle style, const char *link) {
    if (text == NULL) {
        return false;
    }
    for (size_t index = 0U; index < text->span_count; index++) {
        const BacaTextSpan *span = &text->spans[index];
        bool style_matches = (span->style & style) == style;
        bool link_matches = link == NULL ? span->link == NULL : span->link != NULL && strcmp(span->link, link) == 0;
        if (style_matches && link_matches && span->start < span->end && span->end <= strlen(text->text)) {
            return true;
        }
    }
    return false;
}

static bool build_epub2(char **path) {
    static const char opf[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"book-id\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>Harness Book</dc:title><dc:creator>Test Author</dc:creator>"
        "<dc:description>Generated locally</dc:description><dc:publisher>Baca Tests</dc:publisher>"
        "<dc:date>2026-07-18</dc:date><dc:language>en</dc:language><dc:format>EPUB</dc:format>"
        "<dc:identifier id=\"book-id\">urn:test:epub2</dc:identifier><dc:source>fixture</dc:source>"
        "</metadata><manifest>"
        "<item id=\"c1\" href=\"Text/chapter%23one%3F.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c2\" href=\"Text/nested/chapter2.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
        "<item id=\"pixel\" href=\"Images/pixel.png\" media-type=\"image/png\"/>"
        "<item id=\"high\" href=\"Images/high.png\" media-type=\"image/png\"/>"
        "<item id=\"vector\" href=\"Images/vector.svg\" media-type=\"image/svg+xml\"/>"
        "</manifest><spine toc=\"ncx\"><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>";
    static const char chapter1[] =
        "<!doctype html><html><head><title>ignored</title></head><body id=\"root-body\">"
        "<h1 id=\"top\"> Chapter <em>One</em> </h1>"
        "<p id=\"intro\"> Hello   <strong>bold</strong> "
        "<a id=\"next\" name=\"legacy-next\" href=\"nested/chapter2.xhtml#deep\">next</a> "
        "<a href=\"https://example.test/reference\">outside</a>. </p>"
        "<img id=\"raster\" src=\"../Images/pixel.png\" alt=\"Pixel\"/>"
        "<a id=\"image-link\" href=\"nested/chapter2.xhtml#deep\">"
        "<img id=\"linked\" src=\"../Images/pixel.png\" alt=\"Linked\"/></a>"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
        "<image id=\"svg-image\" xlink:href=\"../Images/vector.svg\" aria-label=\"Vector\"/>"
        "<image id=\"svg-href-image\" href=\"../Images/pixel.png\" aria-label=\"SVG Href\"/></svg>"
        "<picture id=\"picture\"><source id=\"picture-source\" "
        "srcset=\"../Images/high.png 2x, ../Images/pixel.png 1x\"/>"
        "<img id=\"picture-fallback\" alt=\"Responsive\"/></picture>"
        "<a id=\"tail\" name=\"tail-name\"></a></body></html>";
    static const char chapter2[] =
        "<html><body><h2 id=\"deep\">Second Section</h2>"
        "<pre id=\"code\">a\tb\n界é</pre><p><a href=\"../chapter%23one%3F.xhtml#top\">back</a></p>"
        "</body></html>";
    static const char ncx[] =
        "<?xml version=\"1.0\"?>"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
        "<navMap><navPoint id=\"n1\"><navLabel><text> Chapter   One </text></navLabel>"
        "<content src=\"Text/chapter%23one%3F.xhtml#top\"/>"
        "<navPoint id=\"n2\"><navLabel><text>Second Section</text></navLabel>"
        "<content src=\"Text/nested/chapter2.xhtml#deep\"/></navPoint></navPoint></navMap></ncx>";
    static const char vector[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\">"
        "<rect width=\"1\" height=\"1\"/></svg>";
    const FixtureMember members[] = {
        {.name = "OEBPS/Text/chapter#one?.xhtml", .data = chapter1, .length = sizeof(chapter1) - 1U},
        {.name = "OEBPS/Text/nested/chapter2.xhtml", .data = chapter2, .length = sizeof(chapter2) - 1U},
        {.name = "OEBPS/toc.ncx", .data = ncx, .length = sizeof(ncx) - 1U},
        {.name = "OEBPS/Images/pixel.png", .data = pixel_png, .length = sizeof(pixel_png)},
        {.name = "OEBPS/Images/high.png", .data = pixel_png, .length = sizeof(pixel_png)},
        {.name = "OEBPS/Images/vector.svg", .data = vector, .length = sizeof(vector) - 1U},
    };
    return create_epub("document/epub2.epub", opf, members, BACA_ARRAY_LEN(members), path);
}

static BacaTestResult test_epub2_normalization_toc_resources_and_cleanup(void) {
    char *path = NULL;
    TEST_ASSERT(build_epub2(&path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_INT(document.format, BACA_FORMAT_EPUB);
    TEST_ASSERT_STR(document.metadata.title, "Harness Book");
    TEST_ASSERT_STR(document.metadata.creator, "Test Author");
    TEST_ASSERT_STR(document.metadata.description, "Generated locally");
    TEST_ASSERT_STR(document.metadata.publisher, "Baca Tests");
    TEST_ASSERT_STR(document.metadata.date, "2026-07-18");
    TEST_ASSERT_STR(document.metadata.language, "en");
    TEST_ASSERT_STR(document.metadata.format, "EPUB");
    TEST_ASSERT_STR(document.metadata.identifier, "urn:test:epub2");
    TEST_ASSERT_STR(document.metadata.source, "fixture");
    TEST_ASSERT_SIZE(document.section_count, 2U);
    TEST_ASSERT_STR(document.sections[0].id, "OEBPS/Text/chapter%23one%3F.xhtml");
    TEST_ASSERT_STR(document.sections[1].id, "OEBPS/Text/nested/chapter2.xhtml");
    TEST_ASSERT(document.sections[0].linear && document.sections[1].linear);
    TEST_ASSERT(document.sections[1].search_offset > document.sections[0].search_offset);

    const BacaTextBlock *heading = find_text(&document, "Chapter One");
    const BacaTextBlock *paragraph = find_text(&document, "Hello bold next outside.");
    TEST_ASSERT(heading != NULL && paragraph != NULL);
    TEST_ASSERT_INT((int)heading->heading_level, 1);
    TEST_ASSERT(text_has_span(heading, BACA_STYLE_HEADING, NULL));
    TEST_ASSERT(text_has_span(heading, (BacaTextStyle)(BACA_STYLE_HEADING | BACA_STYLE_ITALIC), NULL));
    TEST_ASSERT(text_has_span(paragraph, BACA_STYLE_BOLD, NULL));
    TEST_ASSERT(text_has_span(paragraph, BACA_STYLE_NONE, "OEBPS/Text/nested/chapter2.xhtml#deep"));
    TEST_ASSERT(text_has_span(paragraph, BACA_STYLE_NONE, "https://example.test/reference"));

    const BacaImageBlock *pixel = find_image(&document, "Pixel");
    const BacaImageBlock *linked = find_image(&document, "Linked");
    const BacaImageBlock *vector = find_image(&document, "Vector");
    const BacaImageBlock *svg_href = find_image(&document, "SVG Href");
    const BacaImageBlock *responsive = find_image(&document, "Responsive");
    TEST_ASSERT(pixel != NULL && linked != NULL && vector != NULL && svg_href != NULL && responsive != NULL);
    TEST_ASSERT_STR(pixel->uri, "OEBPS/Images/pixel.png");
    TEST_ASSERT_STR(linked->link, "OEBPS/Text/nested/chapter2.xhtml#deep");
    TEST_ASSERT_STR(vector->uri, "OEBPS/Images/vector.svg");
    TEST_ASSERT_STR(svg_href->uri, "OEBPS/Images/pixel.png");
    TEST_ASSERT_STR(responsive->uri, "OEBPS/Images/high.png");

    static const char *const anchors[] = {
        "root-body", "top", "intro", "next", "legacy-next", "raster", "image-link", "linked",
        "svg-image", "svg-href-image", "picture", "picture-source", "picture-fallback", "tail", "tail-name",
    };
    for (size_t index = 0U; index < BACA_ARRAY_LEN(anchors); index++) {
        BacaString target = {0};
        TEST_ASSERT(baca_string_append(&target, "OEBPS/Text/chapter%23one%3F.xhtml#", &error));
        TEST_ASSERT(baca_string_append(&target, anchors[index], &error));
        TEST_ASSERT_MSG(document_has_anchor(&document, target.data), "missing anchor %s", target.data);
        baca_string_free(&target);
    }
    TEST_ASSERT(document_has_anchor(&document, "OEBPS/Text/nested/chapter2.xhtml#deep"));
    TEST_ASSERT_SIZE(document.toc_count, 2U);
    TEST_ASSERT_STR(document.toc[0].label, "Chapter One");
    TEST_ASSERT_STR(document.toc[0].target, "OEBPS/Text/chapter%23one%3F.xhtml#top");
    TEST_ASSERT_INT((int)document.toc[0].depth, 0);
    TEST_ASSERT_SIZE(document.toc[0].section_index, 0U);
    TEST_ASSERT_STR(document.toc[1].target, "OEBPS/Text/nested/chapter2.xhtml#deep");
    TEST_ASSERT_INT((int)document.toc[1].depth, 1);
    TEST_ASSERT_SIZE(document.toc[1].section_index, 1U);
    TEST_ASSERT(baca_document_current_toc(&document, document.sections[1].id) == &document.toc[1]);

    BacaResource resource = {0};
    TEST_ASSERT_MSG(baca_document_load_resource(&document, "OEBPS/Images/pixel.png", &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_SIZE(resource.length, sizeof(pixel_png));
    TEST_ASSERT(memcmp(resource.data, pixel_png, sizeof(pixel_png)) == 0);
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    baca_resource_free(&resource);
    TEST_ASSERT_MSG(baca_document_load_resource(&document, "OEBPS/Images/vector.svg", &resource, &error), "%s",
                    error.message);
    TEST_ASSERT_STR(resource.mime_type, "image/svg+xml");
    TEST_ASSERT(buffer_contains(resource.data, resource.length, "<svg", 4U));
    baca_resource_free(&resource);
    baca_document_close(&document);
    TEST_ASSERT(document.path == NULL && document.backend == NULL && document.blocks == NULL);

    TEST_ASSERT(baca_test_write_text("document/owned-cleanup/marker", "owned"));
    char *cleanup = baca_test_path("document/owned-cleanup");
    TEST_ASSERT(cleanup != NULL);
    TEST_ASSERT_MSG(baca_epub_open(&document, path, cleanup, &error), "%s", error.message);
    TEST_ASSERT(baca_directory_exists(cleanup));
    baca_document_close(&document);
    TEST_ASSERT(!baca_directory_exists(cleanup));
    free(cleanup);
    free(path);
    return BACA_TEST_PASS;
}

static bool build_epub3(char **path) {
    static const char opf[] =
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>EPUB Three</dc:title>"
        "<dc:identifier>urn:test:epub3</dc:identifier><dc:language>en</dc:language></metadata>"
        "<manifest><item id=\"one\" href=\"one.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"page\" href=\"Images/page.svg\" media-type=\"image/svg+xml\"/>"
        "<item id=\"two\" href=\"two.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
        "</manifest><spine><itemref idref=\"one\"/><itemref idref=\"page\" linear=\"no\"/>"
        "<itemref idref=\"two\"/></spine></package>";
    static const char one[] = "<html><body><h1 id=\"start\">EPUB Three</h1><p>First section.</p></body></html>";
    static const char two[] = "<html><body><h2 id=\"second\">Second</h2><p>Last section.</p></body></html>";
    static const char page[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"2\"><rect width=\"2\" "
        "height=\"2\"/></svg>";
    static const char nav[] =
        "<?xml version=\"1.0\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
        "<body><nav epub:type=\"toc\"><ol><li><a href=\"one.xhtml#start\">One</a><ol>"
        "<li><a href=\"Images/page.svg\">Picture</a></li></ol></li>"
        "<li><a href=\"two.xhtml#second\">Two</a></li></ol></nav></body></html>";
    const FixtureMember members[] = {
        {.name = "OEBPS/one.xhtml", .data = one, .length = sizeof(one) - 1U},
        {.name = "OEBPS/two.xhtml", .data = two, .length = sizeof(two) - 1U},
        {.name = "OEBPS/Images/page.svg", .data = page, .length = sizeof(page) - 1U},
        {.name = "OEBPS/nav.xhtml", .data = nav, .length = sizeof(nav) - 1U},
    };
    return create_epub("document/epub3.epub", opf, members, BACA_ARRAY_LEN(members), path);
}

static BacaTestResult test_epub3_nav_and_svg_spine(void) {
    char *path = NULL;
    TEST_ASSERT(build_epub3(&path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.section_count, 3U);
    TEST_ASSERT_STR(document.sections[1].id, "OEBPS/Images/page.svg");
    TEST_ASSERT(!document.sections[1].linear);
    TEST_ASSERT_SIZE(document.sections[1].block_count, 1U);
    const BacaBlock *svg_block = &document.blocks[document.sections[1].first_block];
    TEST_ASSERT_INT(svg_block->kind, BACA_BLOCK_IMAGE);
    TEST_ASSERT_STR(svg_block->value.image.uri, "OEBPS/Images/page.svg");
    TEST_ASSERT_SIZE(document.toc_count, 3U);
    TEST_ASSERT_STR(document.toc[0].label, "One");
    TEST_ASSERT_INT((int)document.toc[1].depth, 1);
    TEST_ASSERT_STR(document.toc[1].target, "OEBPS/Images/page.svg");
    TEST_ASSERT_STR(document.toc[2].target, "OEBPS/two.xhtml#second");
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static bool build_minimal_epub(const char *relative, const char *extra_manifest, const char *extra_spine,
                               const FixtureMember *extra_members, size_t extra_count, char **path) {
    static const char prefix[] =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Minimal</dc:title>"
        "<dc:identifier>urn:test:minimal</dc:identifier><dc:language>en</dc:language></metadata><manifest>"
        "<item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>";
    static const char middle[] = "</manifest><spine><itemref idref=\"chapter\"/>";
    static const char suffix[] = "</spine></package>";
    BacaString opf = {0};
    BacaError error = {0};
    bool built = baca_string_append(&opf, prefix, &error) && baca_string_append(&opf, extra_manifest, &error) &&
                 baca_string_append(&opf, middle, &error) && baca_string_append(&opf, extra_spine, &error) &&
                 baca_string_append(&opf, suffix, &error);
    static const char chapter[] = "<html><body><p id=\"start\">Minimal body</p></body></html>";
    FixtureMember *members = calloc(extra_count + 1U, sizeof(*members));
    if (!built || members == NULL) {
        baca_string_free(&opf);
        free(members);
        return false;
    }
    members[0] = (FixtureMember){
        .name = "OEBPS/chapter.xhtml",
        .data = chapter,
        .length = sizeof(chapter) - 1U,
    };
    for (size_t index = 0U; index < extra_count; index++) {
        members[index + 1U] = extra_members[index];
    }
    bool created = create_epub(relative, opf.data, members, extra_count + 1U, path);
    free(members);
    baca_string_free(&opf);
    return created;
}

static BacaTestResult test_missing_toc_fallback(void) {
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/no-toc.epub", "", "", NULL, 0U, &path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, path, &error), "%s", error.message);
    TEST_ASSERT_SIZE(document.toc_count, 0U);
    TEST_ASSERT_SIZE(document.section_count, 1U);
    TEST_ASSERT(find_text(&document, "Minimal body") != NULL);
    baca_document_close(&document);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_data_resources(void) {
    BacaDocument document = {0};
    BacaResource resource = {0};
    BacaError error = {0};
    TEST_ASSERT(baca_document_load_resource(&document, "data:image/png;base64,iVBORw0KGgo=", &resource, &error));
    TEST_ASSERT_STR(resource.mime_type, "image/png");
    TEST_ASSERT_SIZE(resource.length, 8U);
    TEST_ASSERT(memcmp(resource.data, "\x89PNG\r\n\x1a\n", 8U) == 0);
    baca_resource_free(&resource);
    TEST_ASSERT(baca_document_load_resource(&document, "data:text/plain,a%20b%00c", &resource, &error));
    TEST_ASSERT_SIZE(resource.length, 5U);
    TEST_ASSERT(memcmp(resource.data, "a b\0c", 5U) == 0);
    baca_resource_free(&resource);
    TEST_ASSERT(!baca_document_load_resource(&document, "data:text/plain;base64,%%%", &resource, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    return BACA_TEST_PASS;
}

static BacaTestResult test_traversal_errors(void) {
    char *path = baca_test_path("document/archive-traversal.epub");
    TEST_ASSERT(path != NULL);
    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    TEST_ASSERT(archive != NULL);
    TEST_ASSERT(zip_add_bytes(archive, "mimetype", "application/epub+zip", 20U, true, NULL));
    TEST_ASSERT(zip_add_bytes(archive, "../escape", "x", 1U, false, NULL));
    TEST_ASSERT(close_archive(archive));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "unsafe EPUB archive path") != NULL);
    free(path);

    static const char unsafe_opf[] =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>x</dc:title></metadata>"
        "<manifest><item id=\"bad\" href=\"../../escape.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine><itemref idref=\"bad\"/></spine></package>";
    path = NULL;
    TEST_ASSERT(create_epub("document/reference-traversal.epub", unsafe_opf, NULL, 0U, &path));
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "escapes the document root") != NULL);
    free(path);
    return BACA_TEST_PASS;
}

static bool patch_member_size(const char *path, const char *member_name, uint32_t size) {
    BacaError error = {0};
    BacaBuffer archive = {0};
    if (!baca_read_file(path, &archive, &error)) {
        return false;
    }
    bool patched = false;
    size_t name_length = strlen(member_name);
    for (size_t offset = 0U; offset + 46U <= archive.length; offset++) {
        unsigned char *entry = archive.data + offset;
        if (memcmp(entry, "PK\001\002", 4U) != 0) {
            continue;
        }
        size_t filename_length = (size_t)entry[28] | ((size_t)entry[29] << 8U);
        size_t extra_length = (size_t)entry[30] | ((size_t)entry[31] << 8U);
        size_t comment_length = (size_t)entry[32] | ((size_t)entry[33] << 8U);
        if (offset + 46U + filename_length + extra_length + comment_length > archive.length) {
            break;
        }
        if (filename_length == name_length && memcmp(entry + 46U, member_name, name_length) == 0) {
            entry[24] = (unsigned char)(size & 0xffU);
            entry[25] = (unsigned char)((size >> 8U) & 0xffU);
            entry[26] = (unsigned char)((size >> 16U) & 0xffU);
            entry[27] = (unsigned char)((size >> 24U) & 0xffU);
            patched = true;
            break;
        }
        offset += 45U + filename_length + extra_length + comment_length;
    }
    bool written = patched && baca_write_file(path, archive.data, archive.length, &error);
    baca_buffer_free(&archive);
    return written;
}

static BacaTestResult test_oversized_member_error(void) {
    static const FixtureMember extra[] = {
        {.name = "OEBPS/huge.bin", .data = "x", .length = 1U},
    };
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/oversized.epub",
                                   "<item id=\"huge\" href=\"huge.bin\" media-type=\"application/octet-stream\"/>",
                                   "", extra, BACA_ARRAY_LEN(extra), &path));
    TEST_ASSERT(patch_member_size(path, "OEBPS/huge.bin", 64U * 1024U * 1024U + 1U));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_CORRUPT);
    TEST_ASSERT(strstr(error.message, "size limit") != NULL);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_encryption_document_error(void) {
    static const char encryption[] =
        "<?xml version=\"1.0\"?>"
        "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
        "<EncryptionMethod Algorithm=\"urn:test:drm\"/><CipherData>"
        "<CipherReference URI=\"../OEBPS/chapter.xhtml\"/></CipherData></EncryptedData></encryption>";
    const FixtureMember extra[] = {
        {.name = "META-INF/encryption.xml", .data = encryption, .length = sizeof(encryption) - 1U},
    };
    char *path = NULL;
    TEST_ASSERT(build_minimal_epub("document/encrypted-metadata.epub", "", "", extra, BACA_ARRAY_LEN(extra),
                                   &path));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
    TEST_ASSERT(strstr(error.message, "encrypted EPUB content") != NULL);
    free(path);
    return BACA_TEST_PASS;
}

static BacaTestResult test_encrypted_zip_member_error(void) {
    char *path = NULL;
    static const char opf[] =
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>x</dc:title></metadata>"
        "<manifest><item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"c\"/></spine></package>";
    TEST_ASSERT(create_epub("document/encrypted-member.epub", opf, NULL, 0U, &path));
    int zip_error = 0;
    zip_t *archive = zip_open(path, ZIP_CREATE, &zip_error);
    TEST_ASSERT(archive != NULL);
    zip_int64_t index = -1;
    TEST_ASSERT(zip_add_bytes(archive, "OEBPS/secret.bin", "secret", 6U, false, &index));
    if (zip_file_set_encryption(archive, (zip_uint64_t)index, ZIP_EM_AES_256, "test-password") != 0) {
        zip_discard(archive);
        free(path);
        return baca_test_skip("libzip cannot create AES-encrypted fixtures");
    }
    TEST_ASSERT(close_archive(archive));
    BacaDocument document = {0};
    BacaError error = {0};
    TEST_ASSERT(!baca_document_open(&document, path, &error));
    TEST_ASSERT_ERROR(error, BACA_ERROR_UNSUPPORTED);
    TEST_ASSERT(strstr(error.message, "encrypted EPUB members") != NULL);
    free(path);
    return BACA_TEST_PASS;
}

static char *configured_mobitool_path(const char *configured) {
    if (configured == NULL || configured[0] == '\0' || strchr(configured, '/') == NULL) {
        return NULL;
    }
    BacaError error = {0};
    return baca_realpath(configured, &error);
}

static BacaTestResult test_optional_real_mobi(void) {
    const char *configured_tool = getenv("BACA_TEST_MOBITOOL");
    const char *configured_sample = getenv("BACA_TEST_MOBI_SAMPLE");
    if (configured_tool == NULL || configured_tool[0] == '\0' || configured_sample == NULL ||
        configured_sample[0] == '\0') {
        return baca_test_skip("set BACA_TEST_MOBITOOL and BACA_TEST_MOBI_SAMPLE for real conversion");
    }
    char *tool = configured_mobitool_path(configured_tool);
    BacaError error = {0};
    char *sample = baca_realpath(configured_sample, &error);
    if (tool == NULL || access(tool, X_OK) != 0 || sample == NULL) {
        free(tool);
        free(sample);
        return baca_test_fail_at(__FILE__, __LINE__, "configured MOBI tool/sample is not accessible");
    }
    TEST_ASSERT(baca_test_mkdir("mobi-bin"));
    char *shim = baca_test_path("mobi-bin/mobitool");
    char *bin = baca_test_path("mobi-bin");
    TEST_ASSERT(shim != NULL && bin != NULL);
    TEST_ASSERT(symlink(tool, shim) == 0);
    const char *old_path_value = getenv("PATH");
    char *old_path = baca_strdup(old_path_value == NULL ? "" : old_path_value, &error);
    TEST_ASSERT(old_path != NULL);
    BacaString path_value = {0};
    TEST_ASSERT(baca_string_append(&path_value, bin, &error));
    TEST_ASSERT(baca_string_append_char(&path_value, ':', &error));
    TEST_ASSERT(baca_string_append(&path_value, old_path, &error));
    TEST_ASSERT(setenv("PATH", path_value.data, 1) == 0);

    size_t before = baca_test_count_directories("tmp", "baca-mobi-");
    TEST_ASSERT(before != SIZE_MAX);
    BacaDocument document = {0};
    TEST_ASSERT_MSG(baca_document_open(&document, sample, &error), "%s", error.message);
    size_t during = baca_test_count_directories("tmp", "baca-mobi-");
    TEST_ASSERT_SIZE(during, before + 1U);
    TEST_ASSERT(document.format == BACA_FORMAT_MOBI || document.format == BACA_FORMAT_AZW);
    TEST_ASSERT(document.block_count > 0U && document.section_count > 0U);
    TEST_ASSERT(document.metadata.title != NULL || document.metadata.creator != NULL);
    baca_document_close(&document);
    TEST_ASSERT_SIZE(baca_test_count_directories("tmp", "baca-mobi-"), before);
    TEST_ASSERT(setenv("PATH", old_path, 1) == 0);
    baca_string_free(&path_value);
    free(old_path);
    free(shim);
    free(bin);
    free(tool);
    free(sample);
    return BACA_TEST_PASS;
}

const BacaTestCase *baca_document_test_cases(size_t *count) {
    static const BacaTestCase cases[] = {
        {.name = "epub2_normalization_toc_resources_and_cleanup",
         .function = test_epub2_normalization_toc_resources_and_cleanup},
        {.name = "epub3_nav_and_svg_spine", .function = test_epub3_nav_and_svg_spine},
        {.name = "missing_toc_fallback", .function = test_missing_toc_fallback},
        {.name = "data_resources", .function = test_data_resources},
        {.name = "traversal_errors", .function = test_traversal_errors},
        {.name = "oversized_member_error", .function = test_oversized_member_error},
        {.name = "encryption_document_error", .function = test_encryption_document_error},
        {.name = "encrypted_zip_member_error", .function = test_encrypted_zip_member_error},
        {.name = "optional_real_mobi", .function = test_optional_real_mobi},
    };
    *count = BACA_ARRAY_LEN(cases);
    return cases;
}
