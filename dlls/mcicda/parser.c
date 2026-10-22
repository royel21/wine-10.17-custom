#include "parser.h"
#include "stdio.h"

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

static bool finalizeTracks(VirtualCD* cd) {
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

    for (i = 0; i < cd->trackCount - 1; i++) {
        cd->tracks[i].endLBA = cd->tracks[i + 1].startLBA;
    }

    cd->tracks[cd->trackCount - 1].endLBA = totalSectors;

    return true;
}

bool LoadCurrentDisc(VirtualCD* cd) {
    wchar_t cuePath[MAX_PATH];

    if (!getCurrentCuePath(cuePath, MAX_PATH)) {
        return false;
    }

    WARN("cuePath=%ls\n", cuePath);
    TRACE("cuePath=%ls\n", cuePath);

    if (!parseCue(cuePath, cd)) {
        return false;
    }

    if (!finalizeTracks(cd)) {
        return false;
    }

    return true;
}
