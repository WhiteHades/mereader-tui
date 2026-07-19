#pragma once

#include "baca/document.h"

[[nodiscard]] bool baca_epub_open(BacaDocument *document, const char *path, const char *cleanup_directory,
                                   BacaError *error);
[[nodiscard]] bool baca_mobi_open(BacaDocument *document, const char *path, BacaError *error);
[[nodiscard]] bool baca_html_append_section(BacaDocument *document, const char *html, size_t length,
                                            const char *section_id, BacaError *error);
[[nodiscard]] bool baca_pdf_open(BacaDocument *document, const char *path, BacaError *error);
[[nodiscard]] bool baca_image_open(BacaDocument *document, const char *path, BacaError *error);
[[nodiscard]] bool baca_comic_open(BacaDocument *document, const char *path, BacaError *error);

[[nodiscard]] char *baca_document_resolve_uri(const char *base, const char *reference, bool allow_external,
                                              BacaError *error);
[[nodiscard]] char *baca_document_uri_path(const char *uri, BacaError *error);
[[nodiscard]] char *baca_document_fragment_target(const char *base, const char *fragment, BacaError *error);
[[nodiscard]] bool baca_document_account_metadata(BacaDocument *document, BacaError *error);
[[nodiscard]] bool baca_document_index_toc_sections(BacaDocument *document, BacaError *error);
void baca_document_block_free(BacaBlock *block);
void baca_document_rollback_blocks(BacaDocument *document, size_t first_block);
void baca_document_rollback_toc(BacaDocument *document, size_t first_entry);
