#ifndef OF_ECWOLF_CD_H
#define OF_ECWOLF_CD_H

#include "filesys.h"

#include <stdio.h>

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)
bool OF_ECWolfCD_GetInfo(const char *path, bool *is_dir);
void OF_ECWolfCD_List(const char *path, TArray<FString> &files);
FILE *OF_ECWolfCD_Open(const char *path, const char *mode);
#endif

#endif
