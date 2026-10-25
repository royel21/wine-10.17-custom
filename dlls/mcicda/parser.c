#include "parser.h"
#include "stdio.h"
#include <stdlib.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mcicda);

#define CDIMAGE_REGKEY L"SOFTWARE\\Winlator\\WFM\\CurrentCDImagePath"

static long msfToLBA(const char* msf) {
    int m, s, f;

    if (sscanf(msf, "%d:%d:%d", &m, &s, &f) != 3) {
        return -1;
    }

    return ((m * 60) + s) * 75 + f;
}

static bool getCurrentCuePath(wchar_t* buffer, DWORD bufferChars) {
    HKEY hKey;

    DWORD type;
    LONG result;
    DWORD size = bufferChars * sizeof(wchar_t);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, CDIMAGE_REGKEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    result = RegQueryValueExW(hKey, L"", NULL, &type, (BYTE*)buffer, &size);

    RegCloseKey(hKey);


    if (result != ERROR_SUCCESS)
        return false;

    if (type != REG_SZ)
        return false;

    return true;
}

static void buildBinPath(const wchar_t* cuePath, const char* binFile, wchar_t* outPath) {
    wchar_t* slash;
    wchar_t wBin[MAX_PATH];

    wcscpy(outPath, cuePath);

    slash = wcsrchr(outPath, L'\\');

    if (!slash)
        slash = wcsrchr(outPath, L'/');


    MultiByteToWideChar(CP_ACP, 0, binFile, -1, wBin, MAX_PATH);

    if (slash)
        wcscpy(slash + 1, wBin);
    else
        wcscpy(outPath, wBin);
}

static bool parseCue(const wchar_t* cuePath, VirtualCD* cd) {
    FILE* fp;

    char line[512];

    CDTrack* current = NULL;

    memset(cd, 0, sizeof(*cd));

    wcscpy(cd->cuePath, cuePath);

    fp = _wfopen(cuePath, L"rt");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp)) {
        char fileName[MAX_PATH];

        if (sscanf(line, " FILE \"%259[^\"]\" BINARY", fileName) == 1) {
            buildBinPath(cuePath, fileName, cd->binPath);
            continue;
        }

        {
            int trackNum;
            char mode[32];

            if (sscanf(line, " TRACK %d %31s", &trackNum, mode) == 2) {
                if (cd->trackCount >= MAX_CD_TRACKS)
                    break;

                current = &cd->tracks[cd->trackCount++];

                memset(current, 0, sizeof(*current));

                current->trackNumber = trackNum;

                strcpy(current->mode, mode);

                current->isAudio = !_stricmp(mode, "AUDIO");

                continue;
            }
        }

        if (current) {
            int index;
            char msf[32];

            if (sscanf(line, " INDEX %d %31s", &index, msf) == 2) {
                long lba = msfToLBA(msf);

                if (index == 0)
                    current->index0 = lba;

                if (index == 1) {
                    current->index1 = lba;
                    current->startLBA = lba;
                }
            }
        }
    }

    fclose(fp);

    return cd->trackCount > 0;
}

static bool parseCCD(const wchar_t* ccdPath, VirtualCD* cd)
{
    wchar_t value[64];
    DWORD leadoutLBA = 0;
    wchar_t* ext = NULL;
    int i;

    memset(cd, 0, sizeof(*cd));

    wcscpy(cd->cuePath, ccdPath);
    wcscpy(cd->binPath, ccdPath);

    ext = wcsrchr(cd->binPath, L'.');
    if (!ext)
        return false;

    wcscpy(ext, L".img");

    {
        DWORD attr = GetFileAttributesW(cd->binPath);

        if (attr == INVALID_FILE_ATTRIBUTES ||
            (attr & FILE_ATTRIBUTE_DIRECTORY))
            return false;
    }

    /* Parse TRACK sections */
    for (i = 1; i <= MAX_CD_TRACKS; i++)
    {
        wchar_t section[32];
        int mode;
        CDTrack* trk;

        swprintf(section, ARRAY_SIZE(section), L"TRACK %d", i);

        mode = GetPrivateProfileIntW(
            section,
            L"MODE",
            -1,
            ccdPath);

        if (mode == -1)
            break;

        trk = &cd->tracks[cd->trackCount++];

        memset(trk, 0, sizeof(*trk));

        trk->trackNumber = i;
        trk->sectorSize = 2352;

        if (mode == 0)
        {
            trk->isAudio = TRUE;
            strcpy(trk->mode, "AUDIO");
        }
        else if (mode == 1)
        {
            trk->isAudio = FALSE;
            strcpy(trk->mode, "MODE1/2352");
        }
        else if (mode == 2)
        {
            trk->isAudio = FALSE;
            strcpy(trk->mode, "MODE2/2352");
        }
        else
        {
            trk->isAudio = FALSE;
            strcpy(trk->mode, "UNKNOWN");
        }

        trk->index0 = GetPrivateProfileIntW(
            section,
            L"INDEX 0",
            -1,
            ccdPath);

        trk->index1 = GetPrivateProfileIntW(
            section,
            L"INDEX 1",
            0,
            ccdPath);
    }

    if (!cd->trackCount)
        return false;

    /* Parse TOC entries */
    for (i = 0; i < 256; i++)
    {
        wchar_t section[32];
        unsigned point;
        long plba;

        swprintf(section, ARRAY_SIZE(section), L"Entry %d", i);

        GetPrivateProfileStringW(
            section,
            L"Point",
            L"",
            value,
            ARRAY_SIZE(value),
            ccdPath);

        if (!value[0])
            continue;

        if (swscanf(value, L"%x", &point) != 1)
            continue;

        plba = GetPrivateProfileIntW(
            section,
            L"PLBA",
            0,
            ccdPath);

        if (point >= 1 && point <= (unsigned)cd->trackCount)
        {
            cd->tracks[point - 1].startLBA = plba;
        }
        else if (point == 0xA2)
        {
            leadoutLBA = plba;
        }
    }

    if (leadoutLBA <= (DWORD)cd->tracks[0].startLBA)
        return false;

    /* Calculate end LBAs */
    for (i = 0; i < cd->trackCount - 1; i++)
    {
        cd->tracks[i].endLBA =
            cd->tracks[i + 1].startLBA;
    }

    cd->tracks[cd->trackCount - 1].endLBA = leadoutLBA;

    /* Calculate IMG offsets */
    for (i = 0; i < cd->trackCount; i++)
    {
        CDTrack* trk = &cd->tracks[i];

        trk->fileOffset = (ULONGLONG)trk->startLBA * 2352;

        TRACE(
            "CCD Track %02d mode=%s start=%ld end=%ld offset=%I64u\n",
            trk->trackNumber,
            trk->mode,
            trk->startLBA,
            trk->endLBA,
            trk->fileOffset);
    }

    TRACE(
        "parseCCD Loaded %d tracks leadout=%lu img=%s\n",
        cd->trackCount,
        leadoutLBA,
        wine_dbgstr_w(cd->binPath));

    return true;
}

static bool finalizeTracks(VirtualCD* cd)
{
    FILE* fp;
    long fileSize;
    long totalSectors;
    int i;

    fp = _wfopen(cd->binPath, L"rb");
    if (!fp)
        return false;

    fseek(fp, 0, SEEK_END);
    fileSize = ftell(fp);
    fclose(fp);

    totalSectors = fileSize / 2352;

    TRACE("BIN size=%ld bytes\n", fileSize);
    TRACE("totalSectors=%ld\n", totalSectors);

    /* build track ranges */
    for (i = 0; i < cd->trackCount - 1; i++)
    {
        cd->tracks[i].endLBA =
            cd->tracks[i + 1].startLBA;
    }

    /* last track ends at image end */
    cd->tracks[cd->trackCount - 1].endLBA =
        totalSectors;

    /* calculate physical file offsets */
    {
        ULONGLONG offset = 0;

        for (i = 0; i < cd->trackCount; i++)
        {
            CDTrack* trk = &cd->tracks[i];

            trk->fileOffset = offset;

            if (!_stricmp(trk->mode, "MODE1/2048"))
                trk->sectorSize = 2048;
            else if (!_stricmp(trk->mode, "AUDIO"))
                trk->sectorSize = 2352;
            else
                trk->sectorSize = 2352;

            if (i < cd->trackCount - 1)
            {
                DWORD sectors =
                    trk->endLBA - trk->startLBA;

                offset +=
                    (ULONGLONG)sectors *
                    trk->sectorSize;
            }
        }
    }

    return true;
}

static inline wchar_t* getFileExtension(wchar_t* path) {
    wchar_t* ext = wcsrchr(path, L'.');
    return ext && *ext++ != L'\0' ? ext : NULL;
}

static inline bool hasEx(wchar_t* path, const wchar_t* targetExt) {
    wchar_t* ext = getFileExtension(path);
    return ext && wcsicmp(ext, targetExt) == 0;
}

bool LoadCurrentDisc(VirtualCD* cd) {
    wchar_t path[MAX_PATH];

    if (!getCurrentCuePath(path, MAX_PATH)) {
        return false;
    }

    TRACE("CDImagePath=%ls\n", path);

    if (hasEx(path, L"cue"))
    {
        parseCue(path, cd);
    }
    else  if (hasEx(path, L"ccd"))
    {
        parseCCD(path, cd);
    }

    if (!finalizeTracks(cd)) {
        return false;
    }

    return true;
}
