/*
 * Gamepad support for Winlator
 *
 * Copyright 2026 BrunoSX
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>

#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "ddk/hidtypes.h"
#include "hidusage.h"

#include "wine/debug.h"
#include "wine/hid.h"
#include "wine/unixlib.h"

#include "time_utils.h"
#include "unix_private.h"

#define SERVER_PORT 7949
#define CLIENT_PORT 7947
#define GAMEPAD_MAX_COUNT 4
#define GAMEPAD_GUID "3cdf206d9b324d26b024a0d029105c5d"

#define GAMEPAD_STATE_DISCONNECTED 0
#define GAMEPAD_STATE_CONNECTED 1
#define GAMEPAD_STATE_STARTED 2

#define REQUEST_CODE_GET_GAMEPAD 8
#define REQUEST_CODE_GET_GAMEPAD_STATE 9
#define REQUEST_CODE_RELEASE_GAMEPAD 10
#define REQUEST_CODE_SET_GAMEPAD_STATE 19

#define AXIS_MODE_X_Y_Z_RZ 0
#define AXIS_MODE_X_Y_RX_RY_Z_RZ 1

WINE_DEFAULT_DEBUG_CHANNEL(hid);

struct gamepad
{
    struct unix_device unix_device;
    uint8_t index;
    uint8_t button_count;
    uint8_t axis_mode;
    BOOL vibration;
    uint8_t state;
};

static pthread_mutex_t gamepad_cs = PTHREAD_MUTEX_INITIALIZER;
static int server_fd = -1;
static struct sockaddr_in client_addr = {0};
static struct list event_queue = LIST_INIT(event_queue);
static struct list device_list = LIST_INIT(device_list);

static void close_server_socket(void)
{
    if (server_fd != -1)
    {
        close(server_fd);
        server_fd = -1;
    }
}

static BOOL create_server_socket(void)
{
    struct sockaddr_in server_addr = {0};
    const int reuse_addr = 1;
    int res;

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SERVER_PORT);

    close_server_socket();
    server_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    if (server_fd == -1) goto error;

    res = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr, sizeof(reuse_addr));
    if (res == -1) goto error;

    res = bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (res == -1) goto error;

    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_addr.sin_port = htons(CLIENT_PORT);

    return TRUE;

error:
    close_server_socket();
    return FALSE;
}

static void get_gamepad_request(void)
{
    char buffer[64] = {0};
    buffer[0] = REQUEST_CODE_GET_GAMEPAD;
    sendto(server_fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
}

static void set_gamepad_state_request(int slot, int left_motor_speed, int right_motor_speed, int duration_ms)
{
    char buffer[64];

    buffer[0] = REQUEST_CODE_SET_GAMEPAD_STATE;
    buffer[1] = slot;
    *(int*)(buffer + 2) = left_motor_speed;
    *(int*)(buffer + 6) = right_motor_speed;
    *(int*)(buffer + 10) = duration_ms;
    sendto(server_fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
}

static void release_gamepad_request( void )
{
    char buffer[64];
    buffer[0] = REQUEST_CODE_RELEASE_GAMEPAD;
    sendto(server_fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
}

static inline struct gamepad *impl_from_unix_device(struct unix_device *iface)
{
    return CONTAINING_RECORD(iface, struct gamepad, unix_device);
}

static struct gamepad *find_device_from_index(int index)
{
    struct gamepad *impl;

    LIST_FOR_EACH_ENTRY(impl, &device_list, struct gamepad, unix_device.entry)
        if (impl->index == index) return impl;

    return NULL;
}

static void gamepad_destroy(struct unix_device *iface)
{
}

static NTSTATUS gamepad_start(struct unix_device *iface)
{
    struct gamepad *impl = impl_from_unix_device(iface);

    pthread_mutex_lock(&gamepad_cs);
    impl->state = GAMEPAD_STATE_STARTED;
    pthread_mutex_unlock(&gamepad_cs);
    return STATUS_SUCCESS;
}

static void gamepad_stop(struct unix_device *iface)
{
    struct gamepad *impl = impl_from_unix_device(iface);

    pthread_mutex_lock(&gamepad_cs);
    impl->state = GAMEPAD_STATE_CONNECTED;
    list_remove(&impl->unix_device.entry);
    pthread_mutex_unlock(&gamepad_cs);
}

static NTSTATUS gamepad_haptics_start(struct unix_device *iface, UINT duration_ms,
                                      USHORT rumble_intensity, USHORT buzz_intensity,
                                      USHORT left_intensity, USHORT right_intensity)
{
    struct gamepad *impl = impl_from_unix_device(iface);

    TRACE("iface %p, duration_ms %u, rumble_intensity %u, buzz_intensity %u, left_intensity %u, right_intensity %u.\n",
          iface, duration_ms, rumble_intensity, buzz_intensity, left_intensity, right_intensity);

    if (!impl->vibration) return STATUS_NOT_SUPPORTED;

    set_gamepad_state_request(impl->index, rumble_intensity, buzz_intensity, duration_ms);
    return STATUS_SUCCESS;
}

static NTSTATUS gamepad_haptics_stop(struct unix_device *iface)
{
    struct gamepad *impl = impl_from_unix_device(iface);

    TRACE("iface %p.\n", iface);

    if (!impl->vibration) return STATUS_NOT_SUPPORTED;

    set_gamepad_state_request(impl->index, 0, 0, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS gamepad_physical_device_control(struct unix_device *iface, USAGE control)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS gamepad_physical_device_set_gain(struct unix_device *iface, BYTE percent)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS gamepad_physical_effect_control(struct unix_device *iface, BYTE index,
                                                 USAGE control, BYTE iterations)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS gamepad_physical_effect_update(struct unix_device *iface, BYTE index, struct effect_params *params)
{
    return STATUS_NOT_SUPPORTED;
}

static const struct hid_device_vtbl gamepad_vtbl =
{
    gamepad_destroy,
    gamepad_start,
    gamepad_stop,
    gamepad_haptics_start,
    gamepad_haptics_stop,
    gamepad_physical_device_control,
    gamepad_physical_device_set_gain,
    gamepad_physical_effect_control,
    gamepad_physical_effect_update,
};

static NTSTATUS build_gamepad_report_descriptor(struct unix_device *iface)
{
    const USAGE_AND_PAGE device_usage = {.UsagePage = HID_USAGE_PAGE_GENERIC, .Usage = HID_USAGE_GENERIC_GAMEPAD};
    const USAGE left_axis_usages[] = {HID_USAGE_GENERIC_X, HID_USAGE_GENERIC_Y};
    const USAGE right_axis_usages[] = {HID_USAGE_GENERIC_RX, HID_USAGE_GENERIC_RY};
    const USAGE trigger_axis_usages[] = {HID_USAGE_GENERIC_Z, HID_USAGE_GENERIC_RZ};
    struct gamepad *impl = impl_from_unix_device(iface);

    if (!hid_device_begin_report_descriptor(iface, &device_usage))
        return STATUS_NO_MEMORY;

    if (!hid_device_begin_input_report(iface, &device_usage))
        return STATUS_NO_MEMORY;

    if (impl->axis_mode == AXIS_MODE_X_Y_RX_RY_Z_RZ)
    {
        if (!hid_device_add_axes(iface, 2, HID_USAGE_PAGE_GENERIC, left_axis_usages,
                                 FALSE, -32768, 32767))
            return STATUS_NO_MEMORY;

        if (!hid_device_add_axes(iface, 2, HID_USAGE_PAGE_GENERIC, right_axis_usages,
                                 FALSE, -32768, 32767))
            return STATUS_NO_MEMORY;

        if (!hid_device_add_axes(iface, 2, HID_USAGE_PAGE_GENERIC, trigger_axis_usages,
                                 FALSE, 0, 32767))
            return STATUS_NO_MEMORY;
    }
    else if (impl->axis_mode == AXIS_MODE_X_Y_Z_RZ)
    {
        if (!hid_device_add_axes(iface, 2, HID_USAGE_PAGE_GENERIC, left_axis_usages,
                                 FALSE, -32768, 32767))
            return STATUS_NO_MEMORY;

        if (!hid_device_add_axes(iface, 2, HID_USAGE_PAGE_GENERIC, trigger_axis_usages,
                                 FALSE, -32768, 32767))
            return STATUS_NO_MEMORY;
    }

    if (!hid_device_add_hatswitch(iface, 1))
        return STATUS_NO_MEMORY;

    if (!hid_device_add_buttons(iface, HID_USAGE_PAGE_BUTTON, 1, impl->button_count))
        return STATUS_NO_MEMORY;

    if (!hid_device_end_input_report(iface))
        return STATUS_NO_MEMORY;

    if (impl->vibration)
    {
        if (!hid_device_add_haptics(&impl->unix_device))
            return FALSE;
    }

    if (!hid_device_end_report_descriptor(iface))
        return STATUS_NO_MEMORY;

    return STATUS_SUCCESS;
}

static void gamepad_create(int index, char *data)
{
    uint8_t button_count = data[0];
    uint8_t axis_mode = data[1];
    BOOL vibration = data[2] == 1 ? TRUE : FALSE;
    struct device_desc desc =
    {
        .input = -1,
        .manufacturer = {'W','i','n','l','a','t','o','r',0},
        .version = 0,
        .vid = *(short*)(data + 3),
        .pid = *(short*)(data + 5),
        .is_gamepad = FALSE,
    };
    int name_len = data[7];
    char* name = data + 8;
    char buffer[ARRAY_SIZE(desc.product)];
    struct gamepad *impl;

    snprintf(buffer, sizeof(buffer), "%s.%d", GAMEPAD_GUID, index);
    ntdll_umbstowcs(buffer, strlen(buffer) + 1, desc.serialnumber, ARRAY_SIZE(desc.serialnumber));
    ntdll_umbstowcs(name, name_len, desc.product, ARRAY_SIZE(desc.product));
    desc.product[name_len] = '\0';

    if (axis_mode == AXIS_MODE_X_Y_RX_RY_Z_RZ && button_count == 10)
        desc.is_gamepad = TRUE;

    TRACE("index %d, name %s, desc %s.\n", index, name, debugstr_device_desc(&desc));

    if (!(impl = hid_device_create(&gamepad_vtbl, sizeof(struct gamepad)))) return;
    list_add_tail(&device_list, &impl->unix_device.entry);
    impl->index = index;
    impl->button_count = button_count;
    impl->axis_mode = axis_mode;
    impl->vibration = vibration;
    impl->state = GAMEPAD_STATE_CONNECTED;

    if (build_gamepad_report_descriptor(&impl->unix_device))
    {
        list_remove(&impl->unix_device.entry);
        impl->unix_device.vtbl->destroy(&impl->unix_device);
        return;
    }

    bus_event_queue_device_created(&event_queue, &impl->unix_device, &desc);
}

static void update_gamepad_list(char *data)
{
    int i, j;
    BOOL connected;
    struct gamepad *impl;

    for (i = 0, j = 0; i < GAMEPAD_MAX_COUNT; i++, j += 60)
    {
        connected = data[j+0] == 1 ? TRUE : FALSE;
        impl = find_device_from_index(i);

        if (connected && !impl)
        {
            gamepad_create(i, data + j + 1);
        }
        else if (!connected && impl &&
                               impl->state != GAMEPAD_STATE_DISCONNECTED)
        {
            impl->state = GAMEPAD_STATE_DISCONNECTED;
            bus_event_queue_device_removed(&event_queue, &impl->unix_device);
        }
    }
}

static void gamepad_set_state(int slot, char *data)
{
    int i, j, axis_count = 0;
    char dpad;
    BOOL pressed;
    short buttons, axis_value;
    struct gamepad *impl = find_device_from_index(slot);
    struct unix_device *iface = &impl->unix_device;
    struct hid_device_state *state = &iface->hid_device_state;
    LONG dpad_x = 0, dpad_y = 0;

    if (impl->state != GAMEPAD_STATE_STARTED)
    {
        WARN("Device %p with index %d is stopped, can't set state\n", impl, slot);
        return;
    }

    buttons = *(short*)(data + 0);
    dpad = data[2];

    for (i = 0; i < impl->button_count; i++)
    {
        pressed = (buttons & (1<<i)) ? TRUE : FALSE;
        hid_device_set_button(iface, i, pressed);
    }

    switch (dpad)
    {
    case 0: dpad_y = -1; break;
    case 1: dpad_x =  1; dpad_y = -1; break;
    case 2: dpad_x =  1; break;
    case 3: dpad_x =  1; dpad_y =  1; break;
    case 4: dpad_y =  1; break;
    case 5: dpad_x = -1; dpad_y =  1; break;
    case 6: dpad_x = -1; break;
    case 7: dpad_x = -1; dpad_y = -1; break;
    }

    hid_device_set_hatswitch_x(iface, 0, dpad_x);
    hid_device_set_hatswitch_y(iface, 0, dpad_y);

    if (impl->axis_mode == AXIS_MODE_X_Y_RX_RY_Z_RZ)
        axis_count = 6;
    else if (impl->axis_mode == AXIS_MODE_X_Y_Z_RZ)
        axis_count = 4;

    for (i = 0, j = 3; i < axis_count; i++, j += 2)
    {
        axis_value = *(short*)(data + j);
        hid_device_set_abs_axis(iface, i, axis_value);
    }

    bus_event_queue_input_report(&event_queue, iface, state->report_buf, state->report_len);
}

NTSTATUS winlator_bus_init(void *args)
{
    TRACE("args %p\n", args);

    if (!create_server_socket())
    {
        ERR("could not init Winlator bus\n");
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS winlator_bus_wait(void *args)
{
    struct bus_event *result = args;
    int res;
    char buffer[256];
    DWORD curr_time, last_time;

    /* cleanup previously returned event */
    bus_event_cleanup(result);

    last_time = currentTimeMillis();
    do
    {
        if (bus_event_queue_pop(&event_queue, result)) return STATUS_PENDING;

        res = recvfrom(server_fd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (res <= 0)
        {
            if (errno != EWOULDBLOCK) break;

            curr_time = currentTimeMillis();
            if ((curr_time - last_time) >= 2000)
            {
                get_gamepad_request();
                last_time = curr_time;
            }

            msleep(16);
            continue;
        }

        pthread_mutex_lock(&gamepad_cs);
        if (buffer[0] == REQUEST_CODE_GET_GAMEPAD)
        {
            update_gamepad_list(buffer + 1);
        }
        else if (buffer[0] == REQUEST_CODE_GET_GAMEPAD_STATE)
        {
            char slot = buffer[1];
            gamepad_set_state(slot, buffer + 2);
        }
        pthread_mutex_unlock(&gamepad_cs);
    } while (1);

    TRACE("Winlator main loop exiting\n");
    bus_event_queue_destroy(&event_queue);

    release_gamepad_request();
    close_server_socket();
    return STATUS_SUCCESS;
}

NTSTATUS winlator_bus_stop(void *args)
{
    if (server_fd == -1) return STATUS_SUCCESS;

    release_gamepad_request();
    close_server_socket();
    return STATUS_SUCCESS;
}
