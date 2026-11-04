/*
 * Windows Media Audio SDK (1999), reader only, on top of wmvcore.
 *
 * Interfaces and behaviour were taken from what applications call and from
 * running the redistributable: there is no public header for this version.
 *
 * Copyright 2026 Royel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "mmreg.h"
#include "wmsdk.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wmaudsdk);

#define WMAUDIO_STATUS_ERROR   0
#define WMAUDIO_STATUS_STARTED 1
#define WMAUDIO_STATUS_PLAYING 2
#define WMAUDIO_STATUS_EOF     3

#define NS_E_INVALID_REQUEST_OLD ((HRESULT)0xc00d002b)
#define HRESULT_HANDLE_EOF       ((HRESULT)0x80070026)

/* The native dll delivers samples this far ahead of real time. */
#define LEAD_MS 5000

typedef struct IWMAudioReadCallback IWMAudioReadCallback;
typedef struct
{
    HRESULT (WINAPI *QueryInterface)(IWMAudioReadCallback *iface, REFIID iid, void **out);
    ULONG (WINAPI *AddRef)(IWMAudioReadCallback *iface);
    ULONG (WINAPI *Release)(IWMAudioReadCallback *iface);
    HRESULT (WINAPI *OnSample)(IWMAudioReadCallback *iface, const BYTE *data, DWORD size, DWORD ms);
    HRESULT (WINAPI *OnStatus)(IWMAudioReadCallback *iface, DWORD status, HRESULT hr, void *param);
} IWMAudioReadCallbackVtbl;
struct IWMAudioReadCallback { const IWMAudioReadCallbackVtbl *lpVtbl; };

typedef struct IWMAudioReader IWMAudioReader;
typedef struct
{
    HRESULT (WINAPI *QueryInterface)(IWMAudioReader *iface, REFIID iid, void **out);
    ULONG (WINAPI *AddRef)(IWMAudioReader *iface);
    ULONG (WINAPI *Release)(IWMAudioReader *iface);
    HRESULT (WINAPI *GetAttributeByName)(IWMAudioReader *iface, const WCHAR *name, DWORD *type, BYTE *value, WORD *size);
    HRESULT (WINAPI *GetAttributeCount)(IWMAudioReader *iface, WORD *count);
    HRESULT (WINAPI *GetAttributeByIndex)(IWMAudioReader *iface, WORD index, WCHAR *name, WORD *name_len,
            DWORD *type, BYTE *value, WORD *size);
    HRESULT (WINAPI *GetOutputFormat)(IWMAudioReader *iface, WAVEFORMATEX *format, DWORD size);
    HRESULT (WINAPI *Start)(IWMAudioReader *iface);
    HRESULT (WINAPI *Stop)(IWMAudioReader *iface);
    HRESULT (WINAPI *Seek)(IWMAudioReader *iface, DWORD ms);
} IWMAudioReaderVtbl;
struct IWMAudioReader { const IWMAudioReaderVtbl *lpVtbl; };

#define IWMAudioReadCallback_AddRef(iface) (iface)->lpVtbl->AddRef(iface)
#define IWMAudioReadCallback_Release(iface) (iface)->lpVtbl->Release(iface)
#define IWMAudioReadCallback_OnSample(iface, a, b, c) (iface)->lpVtbl->OnSample(iface, a, b, c)
#define IWMAudioReadCallback_OnStatus(iface, a, b, c) (iface)->lpVtbl->OnStatus(iface, a, b, c)
#define IWMAudioReader_AddRef(iface) (iface)->lpVtbl->AddRef(iface)

enum attr_type { ATTR_DWORD = 0, ATTR_STRING = 1, ATTR_BOOL = 3 };
enum attr_id
{
    ATTR_DURATION, ATTR_BITRATE, ATTR_SEEKABLE, ATTR_BROADCAST, ATTR_PROTECTED, ATTR_TRUSTED,
    ATTR_SIGNATURE, ATTR_TITLE, ATTR_AUTHOR, ATTR_DESCRIPTION, ATTR_RATING, ATTR_COPYRIGHT, ATTR_COUNT
};

static const struct { const WCHAR *name; enum attr_type type; } attributes[ATTR_COUNT] =
{
    [ATTR_DURATION]    = {L"Duration", ATTR_DWORD},
    [ATTR_BITRATE]     = {L"Bitrate", ATTR_DWORD},
    [ATTR_SEEKABLE]    = {L"Seekable", ATTR_BOOL},
    [ATTR_BROADCAST]   = {L"Broadcast", ATTR_BOOL},
    [ATTR_PROTECTED]   = {L"Is_Protected", ATTR_BOOL},
    [ATTR_TRUSTED]     = {L"Is_Trusted", ATTR_BOOL},
    [ATTR_SIGNATURE]   = {L"Signature_Name", ATTR_STRING},
    [ATTR_TITLE]       = {L"Title", ATTR_STRING},
    [ATTR_AUTHOR]      = {L"Author", ATTR_STRING},
    [ATTR_DESCRIPTION] = {L"Description", ATTR_STRING},
    [ATTR_RATING]      = {L"Rating", ATTR_STRING},
    [ATTR_COPYRIGHT]   = {L"Copyright", ATTR_STRING},
};

enum reader_state { STATE_IDLE, STATE_RUNNING, STATE_EOF, STATE_PAST_END };

struct reader
{
    IWMAudioReader IWMAudioReader_iface;
    LONG refcount;

    IWMAudioReadCallback *callback;
    IWMSyncReader *sync_reader;
    WAVEFORMATEX format;

    DWORD duration, bitrate;
    WCHAR *strings[ATTR_COUNT];

    HANDLE thread, wake_event, idle_event;
    CRITICAL_SECTION cs;
    enum reader_state state;
    DWORD position;     /* start of the current run, in ms */
    BOOL start_pending, stop_pending, quit;
};

static struct reader *impl_from_IWMAudioReader(IWMAudioReader *iface)
{
    return CONTAINING_RECORD(iface, struct reader, IWMAudioReader_iface);
}

static WCHAR *string_from_header(const BYTE *data, DWORD size)
{
    WCHAR *ret;

    if (!(ret = calloc(1, size + sizeof(WCHAR)))) return NULL;
    memcpy(ret, data, size);
    return ret;
}

/* wmvcore does not expose what the attributes need, so read them from the ASF header. */
static void parse_asf_header(struct reader *reader, const WCHAR *filename)
{
    static const GUID header_guid = {0x75b22630,0x668e,0x11cf,{0xa6,0xd9,0x00,0xaa,0x00,0x62,0xce,0x6c}};
    static const GUID file_props_guid = {0x8cabdca1,0xa947,0x11cf,{0x8e,0xe4,0x00,0xc0,0x0c,0x20,0x53,0x65}};
    static const GUID content_guid = {0x75b22633,0x668e,0x11cf,{0xa6,0xd9,0x00,0xaa,0x00,0x62,0xce,0x6c}};
    BYTE top[30], *data = NULL;
    DWORD read, size, pos;
    HANDLE file;

    file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;

    if (!ReadFile(file, top, sizeof(top), &read, NULL) || read != sizeof(top)
            || memcmp(top, &header_guid, sizeof(GUID)))
        goto done;
    size = *(DWORD *)(top + 16);
    if (size <= sizeof(top) || size > 16 * 1024 * 1024) goto done;
    size -= sizeof(top);
    if (!(data = malloc(size)) || !ReadFile(file, data, size, &read, NULL) || read != size) goto done;

    for (pos = 0; size - pos >= 24;)
    {
        const BYTE *object = data + pos;
        DWORD object_size = *(DWORD *)(object + 16);

        if (object_size < 24 || object_size > size - pos) break;

        if (!memcmp(object, &file_props_guid, sizeof(GUID)) && object_size >= 104)
        {
            ULONGLONG play_duration = *(ULONGLONG *)(object + 64);
            ULONGLONG preroll = *(ULONGLONG *)(object + 80);
            ULONGLONG ms = play_duration / 10000;

            reader->duration = ms > preroll ? ms - preroll : 0;
            reader->bitrate = *(DWORD *)(object + 100);
        }
        else if (!memcmp(object, &content_guid, sizeof(GUID)) && object_size >= 34)
        {
            static const enum attr_id order[] = {ATTR_TITLE, ATTR_AUTHOR, ATTR_COPYRIGHT, ATTR_DESCRIPTION, ATTR_RATING};
            DWORD offset = 34;
            unsigned int i;

            for (i = 0; i < ARRAY_SIZE(order); ++i)
            {
                DWORD len = *(WORD *)(object + 24 + i * 2);

                if (len > object_size - offset) break;
                reader->strings[order[i]] = string_from_header(object + offset, len);
                offset += len;
            }
        }
        pos += object_size;
    }

done:
    free(data);
    CloseHandle(file);
}

static HRESULT get_output_format(struct reader *reader)
{
    IWMOutputMediaProps *props;
    WM_MEDIA_TYPE *type;
    DWORD size = 0;
    HRESULT hr;

    if (FAILED(hr = IWMSyncReader_GetOutputProps(reader->sync_reader, 0, &props)))
        return hr;
    if (SUCCEEDED(hr = IWMOutputMediaProps_GetMediaType(props, NULL, &size)))
    {
        if (!(type = malloc(size)))
            hr = E_OUTOFMEMORY;
        else
        {
            if (SUCCEEDED(hr = IWMOutputMediaProps_GetMediaType(props, type, &size)))
            {
                if (type->cbFormat >= sizeof(PCMWAVEFORMAT) && type->pbFormat)
                {
                    memset(&reader->format, 0, sizeof(reader->format));
                    memcpy(&reader->format, type->pbFormat, sizeof(PCMWAVEFORMAT));
                    reader->format.wFormatTag = WAVE_FORMAT_PCM;
                }
                else
                    hr = E_FAIL;
            }
            free(type);
        }
    }
    IWMOutputMediaProps_Release(props);
    return hr;
}

/* Returns FALSE if the run was stopped while waiting. */
static BOOL wait_for_lead(struct reader *reader, DWORD started, DWORD sample_ms, DWORD first_ms)
{
    for (;;)
    {
        DWORD played = GetTickCount() - started, due = sample_ms - first_ms;
        BOOL stop;

        EnterCriticalSection(&reader->cs);
        stop = reader->stop_pending || reader->quit;
        LeaveCriticalSection(&reader->cs);
        if (stop) return FALSE;
        if (due <= played + LEAD_MS) return TRUE;
        Sleep(min(due - played - LEAD_MS, 50));
    }
}

static void run(struct reader *reader)
{
    DWORD first_ms, last_ms, started = GetTickCount();

    IWMAudioReadCallback_OnStatus(reader->callback, WMAUDIO_STATUS_STARTED, S_OK, NULL);
    IWMAudioReadCallback_OnStatus(reader->callback, WMAUDIO_STATUS_PLAYING, S_OK, NULL);

    first_ms = last_ms = reader->position;

    for (;;)
    {
        QWORD pts, duration;
        INSSBuffer *sample;
        DWORD flags, output, size;
        WORD stream;
        BYTE *data;
        BOOL stop;
        HRESULT hr;

        EnterCriticalSection(&reader->cs);
        stop = reader->stop_pending || reader->quit;
        if (stop) hr = S_FALSE;
        else hr = IWMSyncReader_GetNextSample(reader->sync_reader, 0, &sample, &pts, &duration, &flags, &output, &stream);
        LeaveCriticalSection(&reader->cs);
        if (stop) return;

        if (hr == NS_E_NO_MORE_SAMPLES)
        {
            EnterCriticalSection(&reader->cs);
            reader->state = STATE_EOF;
            LeaveCriticalSection(&reader->cs);
            IWMAudioReadCallback_OnStatus(reader->callback, WMAUDIO_STATUS_EOF, S_OK, NULL);
            return;
        }
        if (FAILED(hr))
        {
            ERR("Failed to get a sample, hr %#lx.\n", hr);
            EnterCriticalSection(&reader->cs);
            reader->state = STATE_IDLE;
            LeaveCriticalSection(&reader->cs);
            IWMAudioReadCallback_OnStatus(reader->callback, WMAUDIO_STATUS_ERROR, hr, NULL);
            return;
        }

        /* Timestamps from the decoder can step back slightly; the native dll's never do. */
        last_ms = max(last_ms, pts / 10000);

        if (wait_for_lead(reader, started, last_ms, first_ms)
                && SUCCEEDED(INSSBuffer_GetBufferAndLength(sample, &data, &size)) && size)
            IWMAudioReadCallback_OnSample(reader->callback, data, size, last_ms);
        INSSBuffer_Release(sample);
    }
}

static DWORD WINAPI worker_thread(void *arg)
{
    struct reader *reader = arg;

    for (;;)
    {
        BOOL start, eof;

        WaitForSingleObject(reader->wake_event, INFINITE);

        EnterCriticalSection(&reader->cs);
        if (reader->quit)
        {
            LeaveCriticalSection(&reader->cs);
            return 0;
        }
        start = reader->start_pending;
        reader->start_pending = FALSE;
        eof = reader->state == STATE_EOF;
        if (start) ResetEvent(reader->idle_event);
        LeaveCriticalSection(&reader->cs);

        if (!start) continue;

        if (eof)
            IWMAudioReadCallback_OnStatus(reader->callback, WMAUDIO_STATUS_EOF, S_OK, NULL);
        else
            run(reader);

        EnterCriticalSection(&reader->cs);
        if (reader->state == STATE_RUNNING) reader->state = STATE_IDLE;
        reader->stop_pending = FALSE;
        if (!reader->start_pending) SetEvent(reader->idle_event);
        LeaveCriticalSection(&reader->cs);
    }
}

static HRESULT WINAPI reader_QueryInterface(IWMAudioReader *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown))
    {
        *out = iface;
        IWMAudioReader_AddRef(iface);
        return S_OK;
    }

    FIXME("%s not implemented.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI reader_AddRef(IWMAudioReader *iface)
{
    struct reader *reader = impl_from_IWMAudioReader(iface);
    return InterlockedIncrement(&reader->refcount);
}

static ULONG WINAPI reader_Release(IWMAudioReader *iface)
{
    struct reader *reader = impl_from_IWMAudioReader(iface);
    ULONG refcount = InterlockedDecrement(&reader->refcount);
    unsigned int i;

    TRACE("%p decreasing refcount to %lu.\n", reader, refcount);

    if (refcount) return refcount;

    EnterCriticalSection(&reader->cs);
    reader->stop_pending = TRUE;
    reader->quit = TRUE;
    SetEvent(reader->wake_event);
    LeaveCriticalSection(&reader->cs);

    /* The callback may block on a lock the caller holds, so do not wait forever.
     * If it does not return, leak the reader rather than free it under the thread. */
    if (WaitForSingleObject(reader->thread, 5000))
    {
        ERR("Worker thread did not stop, leaking reader %p.\n", reader);
        return 0;
    }

    CloseHandle(reader->thread);
    CloseHandle(reader->wake_event);
    CloseHandle(reader->idle_event);
    DeleteCriticalSection(&reader->cs);
    IWMSyncReader_Release(reader->sync_reader);
    IWMAudioReadCallback_Release(reader->callback);
    for (i = 0; i < ATTR_COUNT; ++i)
        free(reader->strings[i]);
    free(reader);
    return 0;
}

static HRESULT get_attribute(struct reader *reader, enum attr_id id, DWORD *type, BYTE *value, WORD *size)
{
    if (type) *type = attributes[id].type;

    if (attributes[id].type == ATTR_STRING)
    {
        const WCHAR *str = reader->strings[id] ? reader->strings[id] : L"";
        WORD needed = (lstrlenW(str) + 1) * sizeof(WCHAR);

        if (value)
        {
            if (*size < needed) return E_INVALIDARG;
            memcpy(value, str, needed);
        }
        *size = needed;
    }
    else
    {
        DWORD val = 0;

        switch (id)
        {
            case ATTR_DURATION: val = reader->duration; break;
            case ATTR_BITRATE: val = reader->bitrate; break;
            case ATTR_SEEKABLE: val = TRUE; break;
            default: break;
        }
        if (value)
        {
            if (*size < sizeof(DWORD)) return E_INVALIDARG;
            *(DWORD *)value = val;
        }
        *size = sizeof(DWORD);
    }
    return S_OK;
}

static HRESULT WINAPI reader_GetAttributeByName(IWMAudioReader *iface, const WCHAR *name,
        DWORD *type, BYTE *value, WORD *size)
{
    struct reader *reader = impl_from_IWMAudioReader(iface);
    unsigned int i;

    TRACE("iface %p, name %s, type %p, value %p, size %p.\n", iface, debugstr_w(name), type, value, size);

    for (i = 0; i < ATTR_COUNT; ++i)
    {
        if (!wcscmp(name, attributes[i].name))
            return get_attribute(reader, i, type, value, size);
    }
    return E_INVALIDARG;
}

static HRESULT WINAPI reader_GetAttributeCount(IWMAudioReader *iface, WORD *count)
{
    TRACE("iface %p, count %p.\n", iface, count);

    *count = ATTR_COUNT;
    return S_OK;
}

static HRESULT WINAPI reader_GetAttributeByIndex(IWMAudioReader *iface, WORD index, WCHAR *name,
        WORD *name_len, DWORD *type, BYTE *value, WORD *size)
{
    struct reader *reader = impl_from_IWMAudioReader(iface);
    WORD needed;

    TRACE("iface %p, index %u, name %p, name_len %p, type %p, value %p, size %p.\n",
            iface, index, name, name_len, type, value, size);

    if (index >= ATTR_COUNT) return E_INVALIDARG;

    needed = lstrlenW(attributes[index].name) + 1;
    if (name)
    {
        if (*name_len < needed) return E_INVALIDARG;
        memcpy(name, attributes[index].name, needed * sizeof(WCHAR));
    }
    *name_len = needed;
    return get_attribute(reader, index, type, value, size);
}

static HRESULT WINAPI reader_GetOutputFormat(IWMAudioReader *iface, WAVEFORMATEX *format, DWORD size)
{
    struct reader *reader = impl_from_IWMAudioReader(iface);

    TRACE("iface %p, format %p, size %lu.\n", iface, format, size);

    memcpy(format, &reader->format, min(size, sizeof(reader->format)));
    return S_OK;
}

static HRESULT WINAPI reader_Start(IWMAudioReader *iface)
{
    struct reader *reader = impl_from_IWMAudioReader(iface);
    HRESULT hr = S_OK;

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&reader->cs);
    if (reader->state == STATE_PAST_END)
        hr = HRESULT_HANDLE_EOF;
    else if (reader->state == STATE_RUNNING && !reader->stop_pending)
        hr = NS_E_INVALID_REQUEST_OLD;
    else
    {
        if (reader->state != STATE_EOF) reader->state = STATE_RUNNING;
        reader->start_pending = TRUE;
        SetEvent(reader->wake_event);
    }
    LeaveCriticalSection(&reader->cs);
    return hr;
}

static HRESULT WINAPI reader_Stop(IWMAudioReader *iface)
{
    struct reader *reader = impl_from_IWMAudioReader(iface);
    HRESULT hr = S_OK;

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&reader->cs);
    reader->start_pending = FALSE;
    if (reader->state == STATE_RUNNING)
    {
        reader->state = STATE_IDLE;
        reader->stop_pending = TRUE;
    }
    else
        hr = NS_E_INVALID_REQUEST_OLD;
    LeaveCriticalSection(&reader->cs);
    return hr;
}

static HRESULT WINAPI reader_Seek(IWMAudioReader *iface, DWORD ms)
{
    struct reader *reader = impl_from_IWMAudioReader(iface);
    HRESULT hr = S_OK;

    TRACE("iface %p, ms %lu.\n", iface, ms);

    EnterCriticalSection(&reader->cs);
    if (reader->duration && ms >= reader->duration)
        reader->state = STATE_PAST_END;
    else if (SUCCEEDED(hr = IWMSyncReader_SetRange(reader->sync_reader, (QWORD)ms * 10000, 0)))
    {
        reader->position = ms;
        if (reader->state != STATE_RUNNING) reader->state = STATE_IDLE;
    }
    LeaveCriticalSection(&reader->cs);
    return hr;
}

static const IWMAudioReaderVtbl reader_vtbl =
{
    reader_QueryInterface,
    reader_AddRef,
    reader_Release,
    reader_GetAttributeByName,
    reader_GetAttributeCount,
    reader_GetAttributeByIndex,
    reader_GetOutputFormat,
    reader_Start,
    reader_Stop,
    reader_Seek,
};

HRESULT WINAPI WMAudioCreateReader(const WCHAR *filename, IWMAudioReadCallback *callback,
        IWMAudioReader **out, void *reserved)
{
    struct reader *reader;
    HRESULT hr;

    TRACE("filename %s, callback %p, out %p, reserved %p.\n", debugstr_w(filename), callback, out, reserved);

    *out = NULL;
    if (!(reader = calloc(1, sizeof(*reader)))) return E_OUTOFMEMORY;

    reader->IWMAudioReader_iface.lpVtbl = &reader_vtbl;
    reader->refcount = 1;

    if (FAILED(hr = WMCreateSyncReader(NULL, 0, &reader->sync_reader)))
        goto fail;
    if (FAILED(hr = IWMSyncReader_Open(reader->sync_reader, filename)))
    {
        ERR("Failed to open %s, hr %#lx.\n", debugstr_w(filename), hr);
        goto fail;
    }
    if (FAILED(hr = get_output_format(reader)))
    {
        ERR("Failed to get the output format, hr %#lx.\n", hr);
        goto fail;
    }
    parse_asf_header(reader, filename);

    InitializeCriticalSection(&reader->cs);
    reader->wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    reader->idle_event = CreateEventW(NULL, TRUE, TRUE, NULL);
    reader->thread = CreateThread(NULL, 0, worker_thread, reader, 0, NULL);

    IWMAudioReadCallback_AddRef(callback);
    reader->callback = callback;

    *out = &reader->IWMAudioReader_iface;
    return S_OK;

fail:
    if (reader->sync_reader) IWMSyncReader_Release(reader->sync_reader);
    free(reader);
    return hr;
}
