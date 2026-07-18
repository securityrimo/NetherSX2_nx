/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"

void fatal_error(const char *fmt, ...) {
  // format once so we can both log it and show it on screen
  char msg[1024];
  va_list list;
  va_start(list, fmt);
  vsnprintf(msg, sizeof(msg), fmt, list);
  va_end(list);

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  printf("NetherSX2 - fatal error\n\n%s", msg);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
