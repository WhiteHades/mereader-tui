#pragma once

#include "mereader-tui/search.h"
#include "text_input.h"

typedef struct MereaderTuiLibraryDirectoryPicker {
  char *path;
  char **names;
  size_t length;
  size_t capacity;
  MereaderTuiTextInput query;
  MereaderTuiSearchIndex search;
  MereaderTuiSearchDirectories matches;
  size_t selected;
  size_t top;
  bool scanning;
} MereaderTuiLibraryDirectoryPicker;

typedef enum MereaderTuiLibraryDirectoryPickerEntryKind {
  MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_USE = 0,
  MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_PARENT,
  MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_DIRECTORY,
  MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_MATCH,
} MereaderTuiLibraryDirectoryPickerEntryKind;

typedef struct MereaderTuiLibraryDirectoryPickerEntry {
  MereaderTuiLibraryDirectoryPickerEntryKind kind;
  const char *name;
} MereaderTuiLibraryDirectoryPickerEntry;

typedef enum MereaderTuiLibraryDirectoryPickerActivationKind {
  MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ACTIVATION_NONE = 0,
  MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ACTIVATION_BROWSED,
  MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ACTIVATION_USE,
} MereaderTuiLibraryDirectoryPickerActivationKind;

typedef struct MereaderTuiLibraryDirectoryPickerActivation {
  MereaderTuiLibraryDirectoryPickerActivationKind kind;
  /* Borrowed from the picker until its next mutation or release. */
  const char *path;
} MereaderTuiLibraryDirectoryPickerActivation;

void mereader_tui_library_directory_picker_free(
    MereaderTuiLibraryDirectoryPicker *picker);
[[nodiscard]] bool mereader_tui_library_directory_picker_open(
    MereaderTuiLibraryDirectoryPicker *picker, const char *path,
    MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_library_directory_picker_searching(
    const MereaderTuiLibraryDirectoryPicker *picker);
[[nodiscard]] bool mereader_tui_library_directory_picker_has_parent(
    const MereaderTuiLibraryDirectoryPicker *picker);
[[nodiscard]] size_t mereader_tui_library_directory_picker_entry_count(
    const MereaderTuiLibraryDirectoryPicker *picker);
[[nodiscard]] bool mereader_tui_library_directory_picker_entry(
    const MereaderTuiLibraryDirectoryPicker *picker, size_t index,
    MereaderTuiLibraryDirectoryPickerEntry *entry);
void mereader_tui_library_directory_picker_ensure_selection_visible(
    MereaderTuiLibraryDirectoryPicker *picker, size_t visible);
void mereader_tui_library_directory_picker_move_selection(
    MereaderTuiLibraryDirectoryPicker *picker, int amount, size_t visible);
[[nodiscard]] bool mereader_tui_library_directory_picker_parent(
    MereaderTuiLibraryDirectoryPicker *picker, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_library_directory_picker_refresh_search(
    MereaderTuiLibraryDirectoryPicker *picker,
    MereaderTuiSearchIndex *active_index, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_library_directory_picker_set_query(
    MereaderTuiLibraryDirectoryPicker *picker, const char *query,
    MereaderTuiSearchIndex *active_index, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_library_directory_picker_poll(
    MereaderTuiLibraryDirectoryPicker *picker,
    MereaderTuiSearchIndex *active_index, bool *changed,
    MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_library_directory_picker_activate(
    MereaderTuiLibraryDirectoryPicker *picker,
    MereaderTuiSearchIndex *active_index,
    MereaderTuiLibraryDirectoryPickerActivation *activation,
    MereaderTuiError *error);
