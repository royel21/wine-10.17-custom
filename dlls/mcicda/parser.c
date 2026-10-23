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


static inline wchar_t* getFileExtension(wchar_t* path) {
    wchar_t* ext = wcsrchr(path, L'.');
    return ext && *ext++ != L'\0' ? ext : NULL;
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

    if (GetFileAttributesW(cd->binPath) == INVALID_FILE_ATTRIBUTES)
        return false;

    /* Parse TRACK sections */
    for (i = 1; i <= MAX_CD_TRACKS; i++)
    {
        wchar_t section[32];
        int mode;
        CDTrack* trk = NULL;

        swprintf(section, 32, L"TRACK %d", i);

        GetPrivateProfileStringW(section, L"MODE", L"", value, ARRAYSIZE(value), ccdPath);

        if (!value[0])
            break;

        mode = _wtoi(value);

        trk = &cd->tracks[cd->trackCount++];

        memset(trk, 0, sizeof(*trk));

        trk->trackNumber = i;
        trk->sectorSize = 2352;

        if (mode == 0)
        {
            trk->isAudio = TRUE;
            strcpy(trk->mode, "AUDIO");
        }
        else
        {
            trk->isAudio = FALSE;

            if (mode == 1)
                strcpy(trk->mode, "MODE1/2352");
            else if (mode == 2)
                strcpy(trk->mode, "MODE2/2352");
            else
                strcpy(trk->mode, "UNKNOWN");
        }

        GetPrivateProfileStringW(section, L"INDEX 1", L"0", value, ARRAYSIZE(value), ccdPath);

        trk->index1 = _wtol(value);
    }

    if (!cd->trackCount)
        return false;

    /* Parse TOC entries */
    for (i = 0; i < 256; i++)
    {
        wchar_t section[32];
        long point;
        long plba;

        swprintf(section, 32, L"Entry %d", i);

        GetPrivateProfileStringW(section, L"Point", L"", value, ARRAYSIZE(value), ccdPath);

        if (!value[0])
            continue;

        point = wcstol(value, NULL, 0);

        GetPrivateProfileStringW(section, L"PLBA", L"", value, ARRAYSIZE(value), ccdPath);

        if (!value[0])
            continue;

        plba = _wtol(value);

        if (point >= 1 && point <= cd->trackCount)
        {
            cd->tracks[point - 1].startLBA = plba;
        }
        else if (point == 0xA2)
        {
            leadoutLBA = plba;
        }
    }

    if (!leadoutLBA)
        return false;

    /* Calculate end LBAs */
    for (i = 0; i < cd->trackCount - 1; i++)
    {
        cd->tracks[i].endLBA =
            cd->tracks[i + 1].startLBA;
    }

    cd->tracks[cd->trackCount - 1].endLBA =
        leadoutLBA;

    /* Calculate IMG offsets */
    ULONGLONG offset = 0;

    for (i = 0; i < cd->trackCount; i++)
    {
        CDTrack* trk = &cd->tracks[i];

        trk->fileOffset = offset;

        offset +=
            (ULONGLONG)(trk->endLBA - trk->startLBA)
            * 2352;

        TRACE(
            "parseCCD Track %02d mode=%s start=%ld end=%ld offset=%I64u\n",
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


bool LoadCurrentDisc(VirtualCD* cd) {
    wchar_t path[MAX_PATH];
    wchar_t* ext;

    if (!getCurrentCuePath(path, MAX_PATH)) {
        return false;
    }

    ext = getFileExtension(path);

    TRACE("CDImagePath=%ls\n", path);

    if (!_wcsicmp(ext, L".cue"))
    {
        parseCue(path, cd);
    }
    else if (!_wcsicmp(ext, L".ccd"))
    {
        parseCCD(path, cd);
    }

    if (!finalizeTracks(cd)) {
        return false;
    }

    return true;
}
