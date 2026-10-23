#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "winreg.h"

#define MAX_CD_TRACKS 99

typedef struct {
  int trackNumber;

  bool isAudio;

  char mode[32];

  long index0;
  long index1;

  long startLBA;
  long endLBA;

  ULONGLONG fileOffset;
  DWORD sectorSize;

} CDTrack;

typedef struct {
  wchar_t cuePath[MAX_PATH];
  wchar_t binPath[MAX_PATH];

  int trackCount;

  CDTrack tracks[MAX_CD_TRACKS];

} VirtualCD;

bool LoadCurrentDisc(VirtualCD *cd);