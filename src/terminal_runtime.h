#pragma once

#include "mereader-tui/common.h"

#include <stdbool.h>
#include <stddef.h>

/* Begin before curses setup, activate when setup is complete, and finish after
 * terminal cleanup. */
bool mereader_tui_terminal_runtime_begin(MereaderTuiError *error);
bool mereader_tui_terminal_runtime_activate(MereaderTuiError *error);
bool mereader_tui_terminal_runtime_interrupted(void);
int mereader_tui_terminal_runtime_finish(int result, MereaderTuiError *error);

double mereader_tui_terminal_monotonic_seconds(void);
bool mereader_tui_terminal_graphics_write(void *user_data, const void *data,
                                  size_t length);
void mereader_tui_terminal_probe_cell_pixels(bool pixel_mode, int *width, int *height);
