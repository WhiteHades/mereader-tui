#pragma once

#include "mereader-tui/document.h"

#include <sys/stat.h>

[[nodiscard]] bool mereader_tui_epub_open(MereaderTuiDocument *document, const char *path, const char *cleanup_directory,
                                   MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_mobi_open(MereaderTuiDocument *document, const char *path, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_html_append_section(MereaderTuiDocument *document, const char *html, size_t length,
                                             const char *section_id, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_pdf_open(MereaderTuiDocument *document, const char *path, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_image_open(MereaderTuiDocument *document, const char *path,
                                   const struct stat *expected_identity, MereaderTuiError *error);
/* destination must not exist and must be inside a caller-owned temporary directory. */
[[nodiscard]] bool mereader_tui_image_export_original(const MereaderTuiDocument *document, const char *destination,
                                              MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_comic_open(MereaderTuiDocument *document, const char *path,
                                    const struct stat *expected_identity, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_text_open(MereaderTuiDocument *document, const char *path,
                                  const struct stat *expected_identity, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_fb2_open(MereaderTuiDocument *document, const char *path,
                                 const struct stat *expected_identity, MereaderTuiError *error);

[[nodiscard]] char *mereader_tui_document_resolve_uri(const char *base, const char *reference, bool allow_external,
                                               MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_document_uri_path(const char *uri, MereaderTuiError *error);
[[nodiscard]] char *mereader_tui_document_fragment_target(const char *base, const char *fragment, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_document_account_metadata(MereaderTuiDocument *document, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_document_index_toc_sections(MereaderTuiDocument *document, MereaderTuiError *error);
void mereader_tui_document_block_free(MereaderTuiBlock *block);
void mereader_tui_document_rollback_blocks(MereaderTuiDocument *document, size_t first_block);
void mereader_tui_document_rollback_toc(MereaderTuiDocument *document, size_t first_entry);
[[nodiscard]] bool mereader_tui_document_read_snapshot(const char *path, const struct stat *expected_identity,
                                                size_t maximum, MereaderTuiBuffer *output, MereaderTuiError *error);
