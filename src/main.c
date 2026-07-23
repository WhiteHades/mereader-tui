#include "mereader-tui/app.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (setlocale(LC_ALL, "") == NULL) {
    fprintf(stderr, "%s: cannot initialize locale from the environment\n",
            MEREADER_TUI_NAME);
    return EXIT_FAILURE;
  }
  return mereader_tui_cli_main(argc, argv);
}
