#include "baca/app.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (setlocale(LC_ALL, "") == NULL) {
    fprintf(stderr, "%s: cannot initialize locale from the environment\n",
            BACA_NAME);
    return EXIT_FAILURE;
  }
  return baca_cli_main(argc, argv);
}
