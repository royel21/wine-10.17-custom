/*
 * CD requests answered from the disc image loaded in the Winlator file manager
 *
 * The drive the file manager loads images into is a plain folder, so requests that need
 * the disc itself (table of contents, geometry, raw audio sectors) are answered from the
 * image the same way mcicda plays its audio.
 */

#include <stdarg.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "parser.h"
#include "winternl.h"
#include "winioctl.h"
#include "ntddcdrm.h"

/* not in Wine's ntddcdrm.h yet, values from the Windows SDK */
#ifndef IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX
#define IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX CTL_CODE(IOCTL_CDROM_BASE, 0x0014, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif
#ifndef IOCTL_CDROM_READ_TOC_EX
#define IOCTL_CDROM_READ_TOC_EX CTL_CODE(IOCTL_CDROM_BASE, 0x0015, METHOD_BUFFERED, FILE_READ_ACCESS)

#define CDROM_READ_TOC_EX_FORMAT_TOC     0x00
#define CDROM_READ_TOC_EX_FORMAT_SESSION 0x01

typedef struct _CDROM_READ_TOC_EX {
    UCHAR Format : 4;
    UCHAR Reserved1 : 3;
    UCHAR Msf : 1;
    UCHAR SessionTrack;
    UCHAR Reserved2;
    UCHAR Reserved3;
} CDROM_READ_TOC_EX;

typedef struct _CDROM_TOC_SESSION_DATA {
    UCHAR Length[2];
    UCHAR FirstCompleteSession;
    UCHAR LastCompleteSession;
    TRACK_DATA TrackData[1];
} CDROM_TOC_SESSION_DATA;
#endif

#include "image_cd.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mountmgr);

#define RAW_SECTOR_SIZE 2352

static VirtualCD disc;
static WCHAR disc_path[MAX_PATH];
static BOOL disc_loaded;

/* parse the image again only when the file manager loads another one */
static BOOL refresh_disc(void)
{
    WCHAR path[MAX_PATH] = {0};

    if (!GetCurrentDiscPath( path, MAX_PATH ) || !path[0])
    {
        disc_path[0] = 0;
        disc_loaded = FALSE;
        return FALSE;
    }
    if (!wcscmp( path, disc_path )) return disc_loaded;

    lstrcpynW( disc_path, path, MAX_PATH );
    disc_loaded = LoadCurrentDisc( &disc ) && disc.trackCount > 0;
    if (disc_loaded) TRACE( "loaded %s, %d tracks\n", debugstr_w(path), disc.trackCount );
    else WARN( "can't parse disc image %s\n", debugstr_w(path) );
    return disc_loaded;
}

static LONG disc_end(void)
{
    return disc.tracks[disc.trackCount - 1].endLBA;
}

/* TOC addresses: MSF counts the 2 second lead-in, LBA is big-endian */
static void fill_address( UCHAR *address, LONG lba, BOOL msf )
{
    if (msf)
    {
        lba += 150;
        address[0] = 0;
        address[1] = lba / (60 * 75);
        address[2] = (lba / 75) % 60;
        address[3] = lba % 75;
    }
    else
    {
        address[0] = (lba >> 24) & 0xff;
        address[1] = (lba >> 16) & 0xff;
        address[2] = (lba >> 8) & 0xff;
        address[3] = lba & 0xff;
    }
}

static void fill_track( TRACK_DATA *data, int index, BOOL msf )
{
    BOOL leadout = index == disc.trackCount;
    const CDTrack *track = &disc.tracks[leadout ? index - 1 : index];

    memset( data, 0, sizeof(*data) );
    data->Adr = 1;
    data->Control = track->isAudio ? 0 : 4;
    data->TrackNumber = leadout ? 0xaa : track->trackNumber;
    fill_address( data->Address, leadout ? disc_end() : track->startLBA, msf );
}

static ULONG fill_toc( CDROM_TOC *toc, BOOL msf )
{
    ULONG size = FIELD_OFFSET( CDROM_TOC, TrackData[disc.trackCount + 1] );
    int i;

    memset( toc, 0, sizeof(*toc) );
    toc->Length[0] = (size - 2) >> 8;
    toc->Length[1] = (size - 2) & 0xff;
    toc->FirstTrack = disc.tracks[0].trackNumber;
    toc->LastTrack = disc.tracks[disc.trackCount - 1].trackNumber;
    for (i = 0; i <= disc.trackCount; i++) fill_track( &toc->TrackData[i], i, msf );
    return size;
}

/* the parser keeps no session numbers: report one session starting at the first track */
static ULONG fill_session( CDROM_TOC_SESSION_DATA *session, BOOL msf )
{
    ULONG size = sizeof(*session);

    memset( session, 0, size );
    session->Length[0] = (size - 2) >> 8;
    session->Length[1] = (size - 2) & 0xff;
    session->FirstCompleteSession = 1;
    session->LastCompleteSession = 1;
    fill_track( &session->TrackData[0], 0, msf );
    return size;
}

/* the same geometry Windows' cdrom.sys reports: 64 tracks of 32 sectors per cylinder */
static void fill_geometry( DISK_GEOMETRY *geometry )
{
    geometry->Cylinders.QuadPart = (disc_end() + 64 * 32 - 1) / (64 * 32);
    geometry->MediaType = RemovableMedia;
    geometry->TracksPerCylinder = 64;
    geometry->SectorsPerTrack = 32;
    geometry->BytesPerSector = 2048;
}

static NTSTATUS raw_read( const RAW_READ_INFO *request, void *out, ULONG out_len, ULONG_PTR *info )
{
    LONG lba = (LONG)(request->DiskOffset.QuadPart / 2048);
    const WCHAR *open_path = NULL;
    BYTE *buffer = out;
    FILE *file = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    int t;

    if ((ULONGLONG)request->SectorCount * RAW_SECTOR_SIZE > out_len) return STATUS_BUFFER_TOO_SMALL;

    for (i = 0; i < request->SectorCount; i++, lba++)
    {
        const CDTrack *track = NULL;

        for (t = 0; t < disc.trackCount; t++)
        {
            if (lba >= disc.tracks[t].startLBA && lba < disc.tracks[t].endLBA)
            {
                track = &disc.tracks[t];
                break;
            }
        }

        /* cooked 2048 byte images hold no raw sectors to return */
        if (!track || track->sectorSize < RAW_SECTOR_SIZE)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (!open_path || wcscmp( open_path, track->filePath ))
        {
            if (file) fclose( file );
            open_path = track->filePath;
            if (!(file = _wfopen( open_path, L"rb" )))
            {
                status = STATUS_DEVICE_DATA_ERROR;
                break;
            }
        }

        if (_fseeki64( file, track->fileOffset + (ULONGLONG)(lba - track->startLBA) * track->sectorSize, SEEK_SET ) ||
            fread( buffer, 1, RAW_SECTOR_SIZE, file ) != RAW_SECTOR_SIZE)
        {
            status = STATUS_DEVICE_DATA_ERROR;
            break;
        }
        buffer += RAW_SECTOR_SIZE;
    }

    if (file) fclose( file );
    *info = buffer - (BYTE *)out;
    return status;
}

BOOL image_cd_ioctl( ULONG code, const void *in, ULONG in_len, void *out, ULONG out_len,
                     NTSTATUS *status, ULONG_PTR *info )
{
    *info = 0;

    switch (code)
    {
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX:
    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX:
    case IOCTL_CDROM_READ_TOC:
    case IOCTL_CDROM_READ_TOC_EX:
    case IOCTL_CDROM_GET_LAST_SESSION:
    case IOCTL_CDROM_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY2:
    case IOCTL_CDROM_MEDIA_REMOVAL:
    case IOCTL_STORAGE_MEDIA_REMOVAL:
    case IOCTL_CDROM_RAW_READ:
        break;
    default:
        return FALSE;
    }

    if (!refresh_disc()) return FALSE;

    switch (code)
    {
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    {
        DISK_GEOMETRY geometry;

        if (out_len < sizeof(geometry))
        {
            *status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        fill_geometry( &geometry );
        memcpy( out, &geometry, sizeof(geometry) );
        *info = sizeof(geometry);
        *status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY_EX:
    case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX:
    {
        DISK_GEOMETRY_EX geometry = {{{0}}};
        ULONG len = FIELD_OFFSET( DISK_GEOMETRY_EX, Data );

        if (out_len < len)
        {
            *status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        fill_geometry( &geometry.Geometry );
        geometry.DiskSize.QuadPart = (LONGLONG)disc_end() * 2048;
        memcpy( out, &geometry, len );
        *info = len;
        *status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_CDROM_READ_TOC:
    {
        CDROM_TOC toc;
        ULONG size = fill_toc( &toc, TRUE );

        *info = min( size, out_len );
        memcpy( out, &toc, *info );
        *status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_CDROM_READ_TOC_EX:
    {
        const CDROM_READ_TOC_EX *request = in;

        if (in_len < sizeof(*request))
        {
            *status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (request->Format == CDROM_READ_TOC_EX_FORMAT_TOC)
        {
            CDROM_TOC toc;
            ULONG size = fill_toc( &toc, request->Msf );

            *info = min( size, out_len );
            memcpy( out, &toc, *info );
            *status = STATUS_SUCCESS;
        }
        else if (request->Format == CDROM_READ_TOC_EX_FORMAT_SESSION)
        {
            CDROM_TOC_SESSION_DATA session;
            ULONG size = fill_session( &session, request->Msf );

            *info = min( size, out_len );
            memcpy( out, &session, *info );
            *status = STATUS_SUCCESS;
        }
        else
        {
            FIXME( "TOC format %u not supported for disc images\n", request->Format );
            *status = STATUS_INVALID_DEVICE_REQUEST;
        }
        break;
    }
    case IOCTL_CDROM_GET_LAST_SESSION:
    {
        CDROM_TOC_SESSION_DATA session;
        ULONG size = fill_session( &session, TRUE );

        *info = min( size, out_len );
        memcpy( out, &session, *info );
        *status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_CDROM_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY2:
    case IOCTL_CDROM_MEDIA_REMOVAL:
    case IOCTL_STORAGE_MEDIA_REMOVAL:
        *status = STATUS_SUCCESS; /* an image is always present and never locked */
        break;
    case IOCTL_CDROM_RAW_READ:
        if (in_len < sizeof(RAW_READ_INFO) || !out)
            *status = STATUS_INVALID_PARAMETER;
        else
            *status = raw_read( in, out, out_len, info );
        break;
    }

    TRACE( "ioctl %#lx -> %#lx, %Iu bytes\n", code, *status, *info );
    return TRUE;
}
