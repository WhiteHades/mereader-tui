#pragma once

#include "mereader-tui/config.h"
#include "mereader-tui/database.h"
#include "mereader-tui/document.h"
#include "mereader-tui/layout.h"

typedef enum MereaderTuiCommand : uint8_t {
    MEREADER_TUI_COMMAND_NONE = 0,
    MEREADER_TUI_COMMAND_QUIT,
    MEREADER_TUI_COMMAND_SCROLL_DOWN,
    MEREADER_TUI_COMMAND_SCROLL_UP,
    MEREADER_TUI_COMMAND_PAGE_DOWN,
    MEREADER_TUI_COMMAND_PAGE_UP,
    MEREADER_TUI_COMMAND_HOME,
    MEREADER_TUI_COMMAND_END,
    MEREADER_TUI_COMMAND_TOC,
    MEREADER_TUI_COMMAND_ADD_BOOKMARK,
    MEREADER_TUI_COMMAND_BOOKMARKS,
    MEREADER_TUI_COMMAND_METADATA,
    MEREADER_TUI_COMMAND_HELP,
    MEREADER_TUI_COMMAND_TOGGLE_THEME,
    MEREADER_TUI_COMMAND_SEARCH_FORWARD,
    MEREADER_TUI_COMMAND_SEARCH_BACKWARD,
    MEREADER_TUI_COMMAND_NEXT_MATCH,
    MEREADER_TUI_COMMAND_PREVIOUS_MATCH,
    MEREADER_TUI_COMMAND_TOGGLE_PDF_VIEW,
    MEREADER_TUI_COMMAND_CONFIRM,
    MEREADER_TUI_COMMAND_SCREENSHOT,
} MereaderTuiCommand;

typedef bool (*MereaderTuiExternalOpener)(void *user_data, const char *target, const char *preferred,
                                   MereaderTuiError *error);

typedef struct MereaderTuiApp {
    MereaderTuiConfig config;
    MereaderTuiDatabase database;
    MereaderTuiDocument document;
    MereaderTuiLayout layout;
    char *source;
    size_t scroll_line;
    double saved_progress;
    bool dark_mode;
    bool direct_open;
    MereaderTuiExternalOpener external_opener;
    void *external_opener_data;
} MereaderTuiApp;

[[nodiscard]] bool mereader_tui_app_init(MereaderTuiApp *app, const char *path, bool direct_open, MereaderTuiError *error);
[[nodiscard]] bool mereader_tui_app_free(MereaderTuiApp *app, MereaderTuiError *error);
[[nodiscard]] int mereader_tui_app_run(MereaderTuiApp *app, MereaderTuiError *error);
[[nodiscard]] int mereader_tui_cli_main(int argc, char **argv);
[[nodiscard]] int mereader_tui_tui_run(MereaderTuiApp *app, MereaderTuiError *error);
