#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool switchStorageInitializeForPath(const char* iniPath, char* gamePath, size_t gamePathSize,
                                    char* error, size_t errorSize);
bool switchStorageSocketReady(void);
void switchStorageShutdown(void);

#ifdef __cplusplus
}
#endif
