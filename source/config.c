/* config.c -- runtime display globals
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "config.h"

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;
// Physical panel size (720p handheld / 1080p docked). The surface we hand the core
// IS the full panel; the core aspect-fits and centres its draw rect within it.
int panel_width = 1280;
int panel_height = 720;
