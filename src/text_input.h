#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

enum { MEREADER_TUI_TEXT_INPUT_CAPACITY = PATH_MAX };

typedef struct MereaderTuiTextInput {
  char value[MEREADER_TUI_TEXT_INPUT_CAPACITY];
  size_t length;
  size_t cursor;
} MereaderTuiTextInput;

/* Cursor-only operations return false; content insertions and deletions return
 * true. */
bool mereader_tui_text_input_apply(MereaderTuiTextInput *input, bool key_code, int code,
                           wchar_t character);
