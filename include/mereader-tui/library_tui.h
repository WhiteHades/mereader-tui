#pragma once

#include "mereader-tui/catalog.h"
#include "mereader-tui/config.h"
#include "mereader-tui/library.h"

typedef enum MereaderTuiLibraryCommand : uint8_t {
    MEREADER_TUI_LIBRARY_COMMAND_NONE = 0,
    MEREADER_TUI_LIBRARY_COMMAND_QUIT,
    MEREADER_TUI_LIBRARY_COMMAND_OPEN,
    MEREADER_TUI_LIBRARY_COMMAND_REFRESH,
    MEREADER_TUI_LIBRARY_COMMAND_SET_ROOT,
} MereaderTuiLibraryCommand;

typedef struct MereaderTuiLibraryAction {
    MereaderTuiLibraryCommand command;
    char *path;
    MereaderTuiLibrarySort sort;
} MereaderTuiLibraryAction;

[[nodiscard]] int mereader_tui_library_tui_run(const MereaderTuiConfig *config, const MereaderTuiHistory *history, MereaderTuiCatalog *catalog,
                                        bool setup_required, MereaderTuiLibrarySort sort, const char *selected_filepath,
                                        const char *context, MereaderTuiLibraryAction *action, MereaderTuiError *error);
