#pragma once

#include "baca/config.h"
#include "baca/database.h"
#include "baca/document.h"
#include "baca/layout.h"
#include "baca/library.h"

typedef enum BacaCommand : uint8_t {
    BACA_COMMAND_NONE = 0,
    BACA_COMMAND_QUIT,
    BACA_COMMAND_SCROLL_DOWN,
    BACA_COMMAND_SCROLL_UP,
    BACA_COMMAND_PAGE_DOWN,
    BACA_COMMAND_PAGE_UP,
    BACA_COMMAND_HOME,
    BACA_COMMAND_END,
    BACA_COMMAND_TOC,
    BACA_COMMAND_ADD_BOOKMARK,
    BACA_COMMAND_BOOKMARKS,
    BACA_COMMAND_METADATA,
    BACA_COMMAND_HELP,
    BACA_COMMAND_TOGGLE_THEME,
    BACA_COMMAND_SEARCH_FORWARD,
    BACA_COMMAND_SEARCH_BACKWARD,
    BACA_COMMAND_NEXT_MATCH,
    BACA_COMMAND_PREVIOUS_MATCH,
    BACA_COMMAND_TOGGLE_PDF_VIEW,
    BACA_COMMAND_CONFIRM,
    BACA_COMMAND_SCREENSHOT,
} BacaCommand;

typedef bool (*BacaExternalOpener)(void *user_data, const char *target, const char *preferred,
                                   BacaError *error);

typedef struct BacaApp {
    BacaConfig config;
    BacaDatabase database;
    BacaDocument document;
    BacaLayout layout;
    size_t scroll_line;
    double saved_progress;
    bool dark_mode;
    bool direct_open;
    BacaExternalOpener external_opener;
    void *external_opener_data;
} BacaApp;

typedef enum BacaLibraryCommand : uint8_t {
    BACA_LIBRARY_COMMAND_NONE = 0,
    BACA_LIBRARY_COMMAND_QUIT,
    BACA_LIBRARY_COMMAND_OPEN,
    BACA_LIBRARY_COMMAND_REFRESH,
} BacaLibraryCommand;

typedef struct BacaLibraryAction {
    BacaLibraryCommand command;
    char *path;
    BacaLibrarySort sort;
} BacaLibraryAction;

[[nodiscard]] bool baca_app_init(BacaApp *app, const char *path, bool direct_open, BacaError *error);
[[nodiscard]] bool baca_app_free(BacaApp *app, BacaError *error);
[[nodiscard]] int baca_app_run(BacaApp *app, BacaError *error);
[[nodiscard]] int baca_cli_main(int argc, char **argv);
[[nodiscard]] int baca_tui_run(BacaApp *app, BacaError *error);
[[nodiscard]] int baca_library_tui_run(const BacaConfig *config, const BacaHistory *history, BacaLibrarySort sort,
                                       const char *selected_filepath, const char *context, BacaLibraryAction *action,
                                       BacaError *error);
