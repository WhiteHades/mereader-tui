#include "library_directory_picker.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void mereader_tui_library_directory_picker_free(
    MereaderTuiLibraryDirectoryPicker *picker) {
  if (picker == NULL) {
    return;
  }
  for (size_t index = 0U; index < picker->length; ++index) {
    free(picker->names[index]);
  }
  free(picker->names);
  free(picker->path);
  mereader_tui_search_index_close(&picker->search);
  mereader_tui_search_directories_free(&picker->matches);
  *picker = (MereaderTuiLibraryDirectoryPicker){0};
}

static int directory_picker_compare_names(const void *left,
                                          const void *right) {
  const char *left_name = *(char *const *)left;
  const char *right_name = *(char *const *)right;
  const int folded = mereader_tui_casecmp(left_name, right_name);
  return folded == 0 ? strcmp(left_name, right_name) : folded;
}

static bool directory_picker_load(MereaderTuiLibraryDirectoryPicker *picker,
                                  const char *path,
                                  MereaderTuiError *error) {
  DIR *directory = opendir(path);
  if (directory == NULL) {
    const int saved_errno = errno;
    mereader_tui_error_set(
        error,
        saved_errno == ENOENT || saved_errno == ENOTDIR
            ? MEREADER_TUI_ERROR_NOT_FOUND
            : MEREADER_TUI_ERROR_IO,
        "cannot browse '%s': %s", path, strerror(saved_errno));
    return false;
  }

  MereaderTuiLibraryDirectoryPicker replacement = {0};
  replacement.path = mereader_tui_strdup(path, error);
  bool loaded = replacement.path != NULL;
  int read_error = 0;
  while (loaded) {
    errno = 0;
    struct dirent *entry = readdir(directory);
    if (entry == NULL) {
      read_error = errno;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 ||
        strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    struct stat status = {0};
    if (fstatat(dirfd(directory), entry->d_name, &status, 0) != 0 ||
        !S_ISDIR(status.st_mode)) {
      continue;
    }
    char **names = mereader_tui_array_reserve(
        replacement.names, &replacement.capacity, sizeof(*replacement.names),
        replacement.length + 1U, error);
    if (names == NULL) {
      loaded = false;
      break;
    }
    replacement.names = names;
    replacement.names[replacement.length] =
        mereader_tui_strdup(entry->d_name, error);
    if (replacement.names[replacement.length] == NULL) {
      loaded = false;
      break;
    }
    ++replacement.length;
  }
  const int close_status = closedir(directory);
  if (loaded && read_error != 0) {
    mereader_tui_error_set(
        error,
        read_error == ENOENT || read_error == ENOTDIR
            ? MEREADER_TUI_ERROR_NOT_FOUND
            : MEREADER_TUI_ERROR_IO,
        "cannot read '%s': %s", path, strerror(read_error));
    loaded = false;
  } else if (loaded && close_status != 0) {
    const int saved_errno = errno;
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_IO,
                           "cannot close '%s': %s", path,
                           strerror(saved_errno));
    loaded = false;
  }
  if (!loaded) {
    mereader_tui_library_directory_picker_free(&replacement);
    return false;
  }
  if (replacement.length > 1U) {
    qsort(replacement.names, replacement.length, sizeof(*replacement.names),
          directory_picker_compare_names);
  }
  mereader_tui_library_directory_picker_free(picker);
  *picker = replacement;
  return true;
}

bool mereader_tui_library_directory_picker_open(
    MereaderTuiLibraryDirectoryPicker *picker, const char *path,
    MereaderTuiError *error) {
  if (picker == NULL || path == NULL || path[0] == '\0') {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                           "invalid library directory picker request");
    return false;
  }
  char *resolved = mereader_tui_realpath(path, error);
  if (resolved == NULL) {
    return false;
  }
  if (!mereader_tui_directory_exists(resolved)) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_NOT_FOUND,
                           "library path is not a directory: %s", path);
    free(resolved);
    return false;
  }
  const bool loaded = directory_picker_load(picker, resolved, error);
  free(resolved);
  return loaded;
}

bool mereader_tui_library_directory_picker_searching(
    const MereaderTuiLibraryDirectoryPicker *picker) {
  return picker != NULL && picker->query.length > 0U;
}

bool mereader_tui_library_directory_picker_has_parent(
    const MereaderTuiLibraryDirectoryPicker *picker) {
  return picker != NULL && picker->path != NULL &&
         strcmp(picker->path, "/") != 0;
}

size_t mereader_tui_library_directory_picker_entry_count(
    const MereaderTuiLibraryDirectoryPicker *picker) {
  if (picker == NULL || picker->path == NULL) {
    return 0U;
  }
  if (mereader_tui_library_directory_picker_searching(picker)) {
    return picker->matches.length;
  }
  return 1U +
         (mereader_tui_library_directory_picker_has_parent(picker) ? 1U : 0U) +
         picker->length;
}

bool mereader_tui_library_directory_picker_entry(
    const MereaderTuiLibraryDirectoryPicker *picker, size_t index,
    MereaderTuiLibraryDirectoryPickerEntry *entry) {
  if (picker == NULL || entry == NULL) {
    return false;
  }
  if (mereader_tui_library_directory_picker_searching(picker)) {
    if (index >= picker->matches.length) {
      return false;
    }
    *entry = (MereaderTuiLibraryDirectoryPickerEntry){
        .kind = MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_MATCH,
        .name = picker->matches.items[index].relative_path,
    };
    return true;
  }
  if (index == 0U && picker->path != NULL) {
    *entry = (MereaderTuiLibraryDirectoryPickerEntry){
        .kind = MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_USE,
        .name = "[use this folder]",
    };
    return true;
  }
  const bool parent = mereader_tui_library_directory_picker_has_parent(picker);
  if (parent && index == 1U) {
    *entry = (MereaderTuiLibraryDirectoryPickerEntry){
        .kind = MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_PARENT,
        .name = "..",
    };
    return true;
  }
  const size_t offset = 1U + (parent ? 1U : 0U);
  if (index < offset || index - offset >= picker->length) {
    return false;
  }
  *entry = (MereaderTuiLibraryDirectoryPickerEntry){
      .kind = MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ENTRY_DIRECTORY,
      .name = picker->names[index - offset],
  };
  return true;
}

void mereader_tui_library_directory_picker_ensure_selection_visible(
    MereaderTuiLibraryDirectoryPicker *picker, size_t visible) {
  if (picker == NULL) {
    return;
  }
  const size_t count =
      mereader_tui_library_directory_picker_entry_count(picker);
  if (count == 0U) {
    picker->selected = 0U;
    picker->top = 0U;
    return;
  }
  if (picker->selected >= count) {
    picker->selected = count - 1U;
  }
  if (visible == 0U) {
    visible = 1U;
  }
  if (picker->selected < picker->top) {
    picker->top = picker->selected;
  } else if (picker->selected >= picker->top + visible) {
    picker->top = picker->selected - visible + 1U;
  }
  const size_t maximum_top = count > visible ? count - visible : 0U;
  if (picker->top > maximum_top) {
    picker->top = maximum_top;
  }
}

void mereader_tui_library_directory_picker_move_selection(
    MereaderTuiLibraryDirectoryPicker *picker, int amount, size_t visible) {
  if (picker == NULL) {
    return;
  }
  const size_t count =
      mereader_tui_library_directory_picker_entry_count(picker);
  if (count == 0U) {
    picker->selected = 0U;
    picker->top = 0U;
    return;
  }
  mereader_tui_library_directory_picker_ensure_selection_visible(picker,
                                                                 visible);
  if (amount < 0) {
    const size_t distance = (size_t)(-(long)amount);
    picker->selected =
        distance > picker->selected ? 0U : picker->selected - distance;
  } else {
    const size_t maximum = count - 1U;
    const size_t distance = (size_t)amount;
    picker->selected = distance > maximum - picker->selected
                           ? maximum
                           : picker->selected + distance;
  }
  mereader_tui_library_directory_picker_ensure_selection_visible(picker,
                                                                 visible);
}

bool mereader_tui_library_directory_picker_parent(
    MereaderTuiLibraryDirectoryPicker *picker, MereaderTuiError *error) {
  if (picker == NULL || picker->path == NULL) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                           "invalid library directory picker request");
    return false;
  }
  if (!mereader_tui_library_directory_picker_has_parent(picker)) {
    return true;
  }
  char *child = mereader_tui_path_basename(picker->path, error);
  char *parent =
      child == NULL ? NULL : mereader_tui_path_dirname(picker->path, error);
  if (parent == NULL ||
      !mereader_tui_library_directory_picker_open(picker, parent, error)) {
    free(parent);
    free(child);
    return false;
  }
  const size_t offset =
      1U + (mereader_tui_library_directory_picker_has_parent(picker) ? 1U
                                                                    : 0U);
  for (size_t index = 0U; child != NULL && index < picker->length; ++index) {
    if (strcmp(picker->names[index], child) == 0) {
      picker->selected = offset + index;
      break;
    }
  }
  picker->top = 0U;
  free(parent);
  free(child);
  return true;
}

static MereaderTuiSearchIndex *directory_picker_search_index(
    MereaderTuiLibraryDirectoryPicker *picker,
    MereaderTuiSearchIndex *active_index, MereaderTuiError *error) {
  if (active_index != NULL && active_index->handle != NULL &&
      active_index->root != NULL &&
      strcmp(picker->path, active_index->root) == 0) {
    return active_index;
  }
  if (picker->search.handle == NULL &&
      !mereader_tui_search_index_start(&picker->search, picker->path, error)) {
    return NULL;
  }
  return &picker->search;
}

bool mereader_tui_library_directory_picker_refresh_search(
    MereaderTuiLibraryDirectoryPicker *picker,
    MereaderTuiSearchIndex *active_index, MereaderTuiError *error) {
  if (picker == NULL || picker->path == NULL) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                           "invalid library directory picker request");
    return false;
  }
  if (!mereader_tui_library_directory_picker_searching(picker)) {
    mereader_tui_search_directories_free(&picker->matches);
    mereader_tui_search_index_close(&picker->search);
    picker->scanning = false;
    picker->selected = 0U;
    picker->top = 0U;
    return true;
  }

  MereaderTuiSearchIndex *index =
      directory_picker_search_index(picker, active_index, error);
  if (index == NULL) {
    return false;
  }
  MereaderTuiSearchDirectories matches = {0};
  if (!mereader_tui_search_directories(index, picker->query.value, 0U, 512U,
                                       &matches, error)) {
    return false;
  }
  size_t retained = 0U;
  for (size_t result = 0U; result < matches.length; ++result) {
    if (strcmp(matches.items[result].relative_path, "/") == 0 ||
        strcmp(matches.items[result].relative_path, ".") == 0) {
      free(matches.items[result].relative_path);
      continue;
    }
    matches.items[retained++] = matches.items[result];
  }
  const size_t removed = matches.length - retained;
  matches.total = removed > matches.total ? 0U : matches.total - removed;
  matches.length = retained;

  bool scanning = false;
  if (index == &picker->search &&
      !mereader_tui_search_index_scanning(index, &scanning, error)) {
    mereader_tui_search_directories_free(&matches);
    return false;
  }
  mereader_tui_search_directories_free(&picker->matches);
  picker->matches = matches;
  picker->scanning = scanning;
  picker->selected = 0U;
  picker->top = 0U;
  return true;
}

bool mereader_tui_library_directory_picker_set_query(
    MereaderTuiLibraryDirectoryPicker *picker, const char *query,
    MereaderTuiSearchIndex *active_index, MereaderTuiError *error) {
  if (picker == NULL || query == NULL) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                           "invalid library directory picker query");
    return false;
  }
  const size_t length = strlen(query);
  if (length >= sizeof(picker->query.value)) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                           "library directory picker query is too long");
    return false;
  }
  (void)memcpy(picker->query.value, query, length + 1U);
  picker->query.length = length;
  picker->query.cursor = length;
  return mereader_tui_library_directory_picker_refresh_search(
      picker, active_index, error);
}

bool mereader_tui_library_directory_picker_poll(
    MereaderTuiLibraryDirectoryPicker *picker,
    MereaderTuiSearchIndex *active_index, bool *changed,
    MereaderTuiError *error) {
  if (picker == NULL || changed == NULL) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                           "invalid library directory picker poll request");
    return false;
  }
  *changed = false;
  if (!picker->scanning || picker->search.handle == NULL) {
    return true;
  }
  bool scanning = true;
  if (!mereader_tui_search_index_scanning(&picker->search, &scanning,
                                          error)) {
    picker->scanning = false;
    return false;
  }
  if (scanning) {
    return true;
  }
  picker->scanning = false;
  *changed = true;
  if (!mereader_tui_library_directory_picker_refresh_search(
          picker, active_index, error)) {
    return false;
  }
  return true;
}

bool mereader_tui_library_directory_picker_activate(
    MereaderTuiLibraryDirectoryPicker *picker,
    MereaderTuiSearchIndex *active_index,
    MereaderTuiLibraryDirectoryPickerActivation *activation,
    MereaderTuiError *error) {
  if (picker == NULL || activation == NULL || picker->path == NULL) {
    mereader_tui_error_set(error, MEREADER_TUI_ERROR_ARGUMENT,
                           "invalid library directory picker activation");
    return false;
  }
  *activation = (MereaderTuiLibraryDirectoryPickerActivation){0};
  if (mereader_tui_library_directory_picker_searching(picker)) {
    if (picker->selected >= picker->matches.length) {
      return true;
    }
    const char *base = picker->search.root;
    if (base == NULL && active_index != NULL &&
        active_index->handle != NULL && active_index->root != NULL &&
        strcmp(picker->path, active_index->root) == 0) {
      base = active_index->root;
    }
    if (base == NULL) {
      mereader_tui_error_set(error, MEREADER_TUI_ERROR_INTERNAL,
                             "folder search has no base path");
      return false;
    }
    char *path = mereader_tui_path_join(
        base, picker->matches.items[picker->selected].relative_path, error);
    if (path == NULL) {
      return false;
    }
    const bool opened =
        mereader_tui_library_directory_picker_open(picker, path, error);
    free(path);
    if (!opened) {
      return false;
    }
    activation->kind =
        MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ACTIVATION_BROWSED;
    return true;
  }
  if (picker->selected == 0U) {
    activation->kind =
        MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ACTIVATION_USE;
    activation->path = picker->path;
    return true;
  }
  const bool parent = mereader_tui_library_directory_picker_has_parent(picker);
  if (parent && picker->selected == 1U) {
    if (!mereader_tui_library_directory_picker_parent(picker, error)) {
      return false;
    }
    activation->kind =
        MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ACTIVATION_BROWSED;
    return true;
  }
  const size_t offset = 1U + (parent ? 1U : 0U);
  if (picker->selected < offset ||
      picker->selected - offset >= picker->length) {
    return true;
  }
  char *path = mereader_tui_path_join(
      picker->path, picker->names[picker->selected - offset], error);
  if (path == NULL) {
    return false;
  }
  const bool opened =
      mereader_tui_library_directory_picker_open(picker, path, error);
  free(path);
  if (!opened) {
    return false;
  }
  activation->kind =
      MEREADER_TUI_LIBRARY_DIRECTORY_PICKER_ACTIVATION_BROWSED;
  return true;
}
