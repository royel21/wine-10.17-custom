#include "parser.h"
#include "stdio.h"
#include <stdlib.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mcicda);

#define CDIMAGE_REGKEY L"SOFTWARE\\Winlator\\WFM\\CurrentCDImagePath"

/* Alcohol 120% / 52% MDS version 1 layout, as documented by CDemu's libmirage */
#define MDS_HEADER_SIZE 0x58
#define MDS_SESSION_BLOCK_SIZE 24
#define MDS_TRACK_BLOCK_SIZE 80
#define MDS_FOOTER_SIZE 16
#define MDS_MAX_SIZE (16 * 1024 * 1024)

static long msfToLBA(const char* msf) {
    int m, s, f;

    if (sscanf(msf, "%d:%d:%d", &m, &s, &f) != 3) {
        return -1;
    }

    return ((m * 60) + s) * 75 + f;
}

bool GetCurrentDiscPath(wchar_t* buffer, DWORD bufferChars) {
    HKEY hKey;
    DWORD type;
    LONG result;
    DWORD size = bufferChars * sizeof(wchar_t);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, CDIMAGE_REGKEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    result = RegQueryValueExW(hKey, L"", NULL, &type, (BYTE*)buffer, &size);
    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS || type != REG_SZ)
        return false;

    return true;
}

static ULONGLONG getFileSize64(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    return ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

/* a file name relative to the descriptor's folder */
static void buildPathW(const wchar_t* basePath, const wchar_t* fileName, wchar_t* outPath) {
    wchar_t* slash;

    lstrcpynW(outPath, basePath, MAX_PATH);
    slash = wcsrchr(outPath, L'\\');
    if (!slash)
        slash = wcsrchr(outPath, L'/');

    if (slash)
        lstrcpynW(slash + 1, fileName, MAX_PATH - (int)(slash + 1 - outPath));
    else
        lstrcpynW(outPath, fileName, MAX_PATH);
}

static void buildBinPath(const wchar_t* cuePath, const char* binFile, wchar_t* outPath) {
    wchar_t wBin[MAX_PATH];

    MultiByteToWideChar(CP_ACP, 0, binFile, -1, wBin, MAX_PATH);
    buildPathW(cuePath, wBin, outPath);
}

static bool replaceExtension(wchar_t* path, const wchar_t* ext) {
    wchar_t* dot = wcsrchr(path, L'.');
    wchar_t* slash = wcsrchr(path, L'\\');

    if (!dot || (slash && dot < slash))
        return false;
    lstrcpynW(dot, ext, MAX_PATH - (int)(dot - path));
    return true;
}

static DWORD cueSectorSize(const char* mode) {
    if (!_stricmp(mode, "MODE1/2048") || !_stricmp(mode, "MODE2/2048"))
        return 2048;
    if (!_stricmp(mode, "MODE2/2336"))
        return 2336;
    if (!_stricmp(mode, "CDG"))
        return 2448;
    return 2352; /* AUDIO, MODE1/2352, MODE2/2352, CDI/2352 */
}

/* FILE "name" TYPE or FILE name TYPE (unquoted names have no spaces) */
static bool parseCueFileLine(const char* line, char* fileName) {
    const char* p = line;
    const char* end;
    size_t len;

    while (*p == ' ' || *p == '\t') p++;
    if (_strnicmp(p, "FILE", 4) || (p[4] != ' ' && p[4] != '\t'))
        return false;
    p += 4;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '"') {
        end = strchr(++p, '"');
    }
    else {
        end = p;
        while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') end++;
    }

    if (!end || end == p)
        return false;

    len = min((size_t)(end - p), (size_t)MAX_PATH - 1);
    memcpy(fileName, p, len);
    fileName[len] = 0;
    return true;
}

/* INDEX times in a CUE are positions inside the track's own FILE, so a CUE with one file per
 * track (common for games with CD audio) needs every file's length to place the next ones.
 * PREGAP sectors are part of the disc layout but are not stored in any file. */
static bool parseCue(const wchar_t* cuePath, VirtualCD* cd) {
    FILE* fp;
    char line[512];
    wchar_t currentFile[MAX_PATH] = { 0 };
    long relStart[MAX_CD_TRACKS] = { 0 };
    long pregap[MAX_CD_TRACKS] = { 0 };
    long absGap[MAX_CD_TRACKS] = { 0 };
    CDTrack* current = NULL;
    long base = 0, gaps = 0;
    int i, j, k;

    memset(cd, 0, sizeof(*cd));
    wcscpy(cd->cuePath, cuePath);

    fp = _wfopen(cuePath, L"rt");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp)) {
        char fileName[MAX_PATH];
        char mode[32], msf[32];
        int trackNum, index;

        if (parseCueFileLine(line, fileName)) {
            buildBinPath(cuePath, fileName, currentFile);
            if (!cd->binPath[0])
                wcscpy(cd->binPath, currentFile);
            continue;
        }

        if (sscanf(line, " TRACK %d %31s", &trackNum, mode) == 2) {
            if (cd->trackCount >= MAX_CD_TRACKS || !currentFile[0])
                break;

            current = &cd->tracks[cd->trackCount++];
            memset(current, 0, sizeof(*current));

            current->trackNumber = trackNum;
            strcpy(current->mode, mode);
            current->isAudio = !_stricmp(mode, "AUDIO");
            current->sectorSize = cueSectorSize(mode);
            wcscpy(current->filePath, currentFile);
            continue;
        }

        if (!current)
            continue;

        if (sscanf(line, " INDEX %d %31s", &index, msf) == 2) {
            long lba = msfToLBA(msf);

            if (lba < 0)
                continue;
            if (index == 0)
                current->index0 = lba;
            if (index == 1) {
                current->index1 = lba;
                relStart[cd->trackCount - 1] = lba;
            }
        }
        else if (sscanf(line, " PREGAP %31s", msf) == 1) {
            long lba = msfToLBA(msf);
            if (lba > 0)
                pregap[cd->trackCount - 1] = lba;
        }
    }

    fclose(fp);
    if (!cd->trackCount)
        return false;

    /* tracks i..j-1 share one file */
    for (i = 0; i < cd->trackCount; i = j) {
        ULONGLONG fileSize, pos = 0;
        long prevRel = 0, lastSectors, fileSectors;
        CDTrack* last;

        for (j = i; j < cd->trackCount && !wcscmp(cd->tracks[j].filePath, cd->tracks[i].filePath); j++)
            ;

        fileSize = getFileSize64(cd->tracks[i].filePath);
        if (!fileSize) {
            WARN("missing image file %s\n", debugstr_w(cd->tracks[i].filePath));
            return false;
        }

        for (k = i; k < j; k++) {
            CDTrack* trk = &cd->tracks[k];
            /* the sectors before this track's INDEX 01 belong to the previous track's layout */
            DWORD spanSize = k > i ? cd->tracks[k - 1].sectorSize : trk->sectorSize;

            if (relStart[k] < prevRel)
                relStart[k] = prevRel;
            pos += (ULONGLONG)(relStart[k] - prevRel) * spanSize;
            prevRel = relStart[k];

            gaps += pregap[k];
            absGap[k] = gaps;
            trk->fileOffset = pos;
            trk->startLBA = base + gaps + relStart[k];
        }

        /* the file's last track runs to the end of the file */
        last = &cd->tracks[j - 1];
        lastSectors = fileSize > last->fileOffset ? (long)((fileSize - last->fileOffset) / last->sectorSize) : 0;
        fileSectors = relStart[j - 1] + lastSectors;

        for (k = i; k < j - 1; k++)
            cd->tracks[k].endLBA = base + absGap[k] + relStart[k + 1];
        last->endLBA = base + absGap[j - 1] + fileSectors;

        base += fileSectors;
    }

    return true;
}

static bool parseCCD(const wchar_t* ccdPath, VirtualCD* cd) {
    wchar_t value[64];
    DWORD leadoutLBA = 0;
    ULONGLONG imgSize;
    int i;

    memset(cd, 0, sizeof(*cd));
    wcscpy(cd->cuePath, ccdPath);
    wcscpy(cd->binPath, ccdPath);

    if (!replaceExtension(cd->binPath, L".img"))
        return false;

    imgSize = getFileSize64(cd->binPath);
    if (!imgSize)
        return false;

    for (i = 1; i <= MAX_CD_TRACKS; i++) {
        wchar_t section[32];
        int mode;
        CDTrack* trk;

        swprintf(section, ARRAY_SIZE(section), L"TRACK %d", i);
        mode = GetPrivateProfileIntW(section, L"MODE", -1, ccdPath);

        if (mode == -1)
            break;

        trk = &cd->tracks[cd->trackCount++];
        memset(trk, 0, sizeof(*trk));

        trk->trackNumber = i;
        trk->sectorSize = 2352;
        wcscpy(trk->filePath, cd->binPath);

        if (mode == 0) {
            trk->isAudio = TRUE;
            strcpy(trk->mode, "AUDIO");
        }
        else if (mode == 1) {
            trk->isAudio = FALSE;
            strcpy(trk->mode, "MODE1/2352");
        }
        else if (mode == 2) {
            trk->isAudio = FALSE;
            strcpy(trk->mode, "MODE2/2352");
        }
        else {
            trk->isAudio = FALSE;
            strcpy(trk->mode, "UNKNOWN");
        }

        trk->index0 = GetPrivateProfileIntW(section, L"INDEX 0", -1, ccdPath);
        trk->index1 = GetPrivateProfileIntW(section, L"INDEX 1", 0, ccdPath);
    }

    if (!cd->trackCount)
        return false;

    for (i = 0; i < 256; i++) {
        wchar_t section[32];
        unsigned point;
        long plba;

        swprintf(section, ARRAY_SIZE(section), L"Entry %d", i);
        GetPrivateProfileStringW(section, L"Point", L"", value, ARRAY_SIZE(value), ccdPath);

        if (!value[0])
            continue;

        if (swscanf(value, L"%x", &point) != 1)
            continue;

        plba = GetPrivateProfileIntW(section, L"PLBA", 0, ccdPath);

        if (point >= 1 && point <= (unsigned)cd->trackCount) {
            cd->tracks[point - 1].startLBA = plba;
        }
        else if (point == 0xA2) {
            leadoutLBA = plba;
        }
    }

    /* the image stores every sector from LBA 0 on, 2352 bytes each */
    if (!leadoutLBA || leadoutLBA > imgSize / 2352)
        leadoutLBA = (DWORD)(imgSize / 2352);

    if (leadoutLBA <= (DWORD)cd->tracks[0].startLBA)
        return false;

    for (i = 0; i < cd->trackCount - 1; i++) {
        cd->tracks[i].endLBA = cd->tracks[i + 1].startLBA;
    }
    cd->tracks[cd->trackCount - 1].endLBA = leadoutLBA;

    for (i = 0; i < cd->trackCount; i++) {
        CDTrack* trk = &cd->tracks[i];
        trk->fileOffset = (ULONGLONG)trk->startLBA * 2352;
    }

    return true;
}

static uint16_t readU16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t readU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t readU64(const uint8_t* p) {
    return (uint64_t)readU32(p) | ((uint64_t)readU32(p + 4) << 32);
}

/* the image file is named in the track's footer; "*.mdf" means "the MDS name with this extension" */
static void getMdfPath(const wchar_t* mdsPath, const uint8_t* mds, size_t size, const uint8_t* track, wchar_t* mdfPath) {
    uint32_t footerOffset = readU32(track + 52);
    wchar_t name[MAX_PATH] = { 0 };

    if (readU32(track + 48) > 0 && footerOffset && footerOffset + MDS_FOOTER_SIZE <= size) {
        uint32_t nameOffset = readU32(mds + footerOffset);
        bool wide = readU32(mds + footerOffset + 4) != 0;
        size_t i;

        if (nameOffset < size) {
            if (wide) {
                for (i = 0; i < MAX_PATH - 1 && nameOffset + i * 2 + 1 < size; i++) {
                    name[i] = (wchar_t)readU16(mds + nameOffset + i * 2);
                    if (!name[i])
                        break;
                }
            }
            else {
                char narrow[MAX_PATH] = { 0 };
                for (i = 0; i < MAX_PATH - 1 && nameOffset + i < size && mds[nameOffset + i]; i++)
                    narrow[i] = (char)mds[nameOffset + i];
                MultiByteToWideChar(CP_ACP, 0, narrow, -1, name, MAX_PATH);
            }
        }
    }

    if (!name[0] || name[0] == L'*') {
        lstrcpynW(mdfPath, mdsPath, MAX_PATH);
        replaceExtension(mdfPath, name[0] == L'*' ? name + 1 : L".mdf");
    }
    else buildPathW(mdsPath, name, mdfPath);
}

static bool parseMDS(const wchar_t* mdsPath, VirtualCD* cd) {
    ULONGLONG fileSize = getFileSize64(mdsPath);
    uint8_t* mds;
    DWORD readSize = 0;
    HANDLE file;
    uint16_t sessionCount, s;
    uint32_t sessionsOffset;
    bool isDvd;

    memset(cd, 0, sizeof(*cd));
    wcscpy(cd->cuePath, mdsPath);

    if (fileSize < MDS_HEADER_SIZE || fileSize > MDS_MAX_SIZE)
        return false;

    file = CreateFileW(mdsPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    mds = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)fileSize);
    if (!mds || !ReadFile(file, mds, (DWORD)fileSize, &readSize, NULL) || readSize != fileSize) {
        CloseHandle(file);
        HeapFree(GetProcessHeap(), 0, mds);
        return false;
    }
    CloseHandle(file);

    if (memcmp(mds, "MEDIA DESCRIPTOR", 16) || mds[16] != 1) {
        /* version 2 descriptors (newer DAEMON Tools) are encrypted */
        WARN("unsupported MDS file %s (version %u.%u)\n", debugstr_w(mdsPath), mds[16], mds[17]);
        HeapFree(GetProcessHeap(), 0, mds);
        return false;
    }

    isDvd = readU16(mds + 0x12) >= 0x10;
    sessionCount = readU16(mds + 0x14);
    sessionsOffset = readU32(mds + 0x50);

    for (s = 0; s < sessionCount; s++) {
        uint32_t sessionOffset = sessionsOffset + s * MDS_SESSION_BLOCK_SIZE;
        const uint8_t* session;
        uint32_t tracksOffset;
        long sessionEnd;
        int firstInSession = cd->trackCount;
        uint8_t blockCount, b;

        if (sessionOffset + MDS_SESSION_BLOCK_SIZE > fileSize)
            break;

        session = mds + sessionOffset;
        sessionEnd = (long)readU32(session + 4);
        blockCount = session[10];
        tracksOffset = readU32(session + 20);

        for (b = 0; b < blockCount && cd->trackCount < MAX_CD_TRACKS; b++) {
            uint32_t trackOffset = tracksOffset + b * MDS_TRACK_BLOCK_SIZE;
            const uint8_t* block;
            uint8_t point, mode;
            CDTrack* trk;

            if (trackOffset + MDS_TRACK_BLOCK_SIZE > fileSize)
                break;

            block = mds + trackOffset;
            point = block[4];
            if (point == 0 || point > 99)
                continue; /* lead-in entries, not tracks */

            mode = block[0] & 0x0F;
            trk = &cd->tracks[cd->trackCount++];
            memset(trk, 0, sizeof(*trk));

            trk->trackNumber = point;
            trk->isAudio = mode == 0x09;
            trk->sectorSize = readU16(block + 16);
            if (!trk->sectorSize)
                trk->sectorSize = isDvd ? 2048 : 2352 + (block[1] ? 96 : 0);
            trk->startLBA = (long)readU32(block + 36);
            trk->index1 = trk->startLBA;
            trk->fileOffset = readU64(block + 40);
            strcpy(trk->mode, trk->isAudio ? "AUDIO" : mode == 0x0A ? "MODE1/2352" : "MODE2/2352");
            getMdfPath(mdsPath, mds, (size_t)fileSize, block, trk->filePath);

            if (!cd->binPath[0])
                wcscpy(cd->binPath, trk->filePath);
        }

        /* a track ends where the next one starts, the last one of a session at the session end */
        for (b = firstInSession; b + 1 < cd->trackCount; b++)
            cd->tracks[b].endLBA = cd->tracks[b + 1].startLBA;
        if (cd->trackCount > firstInSession)
            cd->tracks[cd->trackCount - 1].endLBA = sessionEnd;
    }

    HeapFree(GetProcessHeap(), 0, mds);

    if (!cd->trackCount || !getFileSize64(cd->binPath))
        return false;

    /* sanity: a broken session end must not hide the last track */
    {
        CDTrack* last = &cd->tracks[cd->trackCount - 1];
        ULONGLONG mdfSize = getFileSize64(last->filePath);
        long available = mdfSize > last->fileOffset ? (long)((mdfSize - last->fileOffset) / last->sectorSize) : 0;

        if (last->endLBA <= last->startLBA || last->endLBA - last->startLBA > available)
            last->endLBA = last->startLBA + available;
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
    bool ok = false;
    int i;

    if (!GetCurrentDiscPath(path, MAX_PATH)) {
        return false;
    }

    TRACE("CDImagePath=%ls\n", path);

    if (hasEx(path, L"cue")) {
        ok = parseCue(path, cd);
    }
    else if (hasEx(path, L"ccd")) {
        ok = parseCCD(path, cd);
    }
    else if (hasEx(path, L"mds") || hasEx(path, L"mdf")) {
        /* the file manager stores whichever file of the pair was selected */
        if (hasEx(path, L"mdf"))
            replaceExtension(path, L".mds");
        ok = parseMDS(path, cd);
    }

    if (!ok)
        return false;

    for (i = 0; i < cd->trackCount; i++) {
        CDTrack* trk = &cd->tracks[i];
        TRACE("Track %02d mode=%s start=%ld end=%ld size=%lu offset=%I64u file=%s\n",
            trk->trackNumber, trk->mode, trk->startLBA, trk->endLBA, trk->sectorSize,
            trk->fileOffset, debugstr_w(trk->filePath));
    }

    return true;
}
