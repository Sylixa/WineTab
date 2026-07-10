//
// This is native Linux code that our WinTab DLL will call into. The entrypoints
// are at the bottom of the file.
//
// Currently this code uses XInput 1 functionality via libxcb. This keeps us
// seperate from the libX11 used by Wine since that maintains per-process error
// handling information.
//

#include <ctype.h>
#include <stdio.h>
#include <stdio.h>
#include <stdio.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <windef.h>

// WineGCC defines _WIN32 for us to use any windows headers but XCB has
// different headers on Windows and we want the unix version.
#undef _WIN32
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#define _WIN32 1

#include <sys/poll.h>
#include <time.h>

#include "XWinTabTypes.h"


typedef struct HelperDataTAG {
    DeviceInfo device;
    xcb_connection_t *connection;
    EventCallback callback;
    uint8_t xiEventBase;
    uint8_t xiMajorOpcode;
} HelperData;

static HelperData g_data;

static EventInfo g_eventInfo;

static const char *g_requiredName;

// Last real position seen from a raw motion event. ButtonPress/Release
// events carry no axis data of their own, so we substitute this cached
// value instead of dispatching a bogus (0, 0) which draws a stray line
// from the origin on stroke start.
static int32_t g_lastX = 0;
static int32_t g_lastY = 0;

// Legacy XInput1 event delivery (DeviceMotionNotify/DeviceValuator/etc, via
// xcb_input_select_extension_event) has been observed to silently stall for
// a given client after certain focus-grab transitions under Xwayland (e.g.
// switching to a fullscreen app, clicking it with the mouse, then switching
// back) -- confirmed via `xinput test-xi2` that raw XInput2 events keep
// flowing through the exact same transition when XI1 delivery to this
// process does not. So we use XInput2 raw events (focus-independent by
// design) instead. Raw events have no distinct proximity type, so proximity
// in/out is synthesized from motion activity/inactivity (see check_events).
static int g_inProximity = 0;
static struct timespec g_lastEventTime;

static void mark_event_activity(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_lastEventTime);
}


// ----------------
// XCB Event Handling
//

static double fp3232_to_double(xcb_input_fp3232_t v) {
    return (double) v.integral + (double) v.frac / 4294967296.0;
}

// Sends a synthetic proximity-in using the last known position, if we
// aren't already marked as in proximity. Raw XI2 events carry no proximity
// notion of their own, so this is inferred from motion/button activity.
static void ensure_proximity_in(unsigned int time) {
    if (g_inProximity)
        return;
    g_inProximity = 1;

    g_eventInfo.type = kEventTypeProximityIn;
    g_eventInfo.time = time;
    g_eventInfo.x = g_lastX;
    g_eventInfo.y = g_lastY;
    g_eventInfo.pressure = 0;
    g_eventInfo.xTilt = 0;
    g_eventInfo.yTilt = 0;
    g_data.callback(&g_eventInfo);
    g_eventInfo.type = kEventTypeUnknown;
}

static void handle_raw_motion(xcb_input_raw_motion_event_t *event) {
    if (event->sourceid != g_data.device.id)
        return;

    uint32_t *mask = xcb_input_raw_button_press_valuator_mask(
        (xcb_input_raw_button_press_event_t *) event);
    int mask_words = xcb_input_raw_button_press_valuator_mask_length(
        (xcb_input_raw_button_press_event_t *) event);
    xcb_input_fp3232_t *values = xcb_input_raw_button_press_axisvalues(
        (xcb_input_raw_button_press_event_t *) event);

    int32_t x = g_lastX, y = g_lastY;
    int pressure = g_eventInfo.pressure;
    int xTilt = g_eventInfo.xTilt, yTilt = g_eventInfo.yTilt;
    int got_position = 0;
    int value_idx = 0;

    for (int bit = 0; bit < mask_words * 32; bit++) {
        if (!(mask[bit / 32] & (1u << (bit % 32))))
            continue;

        double v = fp3232_to_double(values[value_idx++]);
        switch (bit) {
        case 0: x = (int32_t) v; got_position = 1; break;
        case 1: y = (int32_t) v; got_position = 1; break;
        case 2: pressure = (int) v; break;
        case 3: xTilt = (int) v; break;
        case 4: yTilt = (int) v; break;
        default: break;
        }
    }

    if (!got_position)
        return;

    mark_event_activity();
    ensure_proximity_in(event->time);

    g_lastX = x;
    g_lastY = y;

    g_eventInfo.type = kEventTypeMotionNotify;
    g_eventInfo.time = event->time;
    g_eventInfo.x = x;
    g_eventInfo.y = y;
    g_eventInfo.pressure = pressure;
    g_eventInfo.xTilt = xTilt;
    g_eventInfo.yTilt = yTilt;
    g_data.callback(&g_eventInfo);
    g_eventInfo.type = kEventTypeUnknown;
}

static void handle_raw_button(xcb_input_raw_button_press_event_t *event, int is_press) {
    if (event->sourceid != g_data.device.id)
        return;

    mark_event_activity();
    ensure_proximity_in(event->time);

    g_eventInfo.type = is_press ? kEventTypeButtonPress : kEventTypeButtonRelease;
    g_eventInfo.time = event->time;
    g_eventInfo.x = g_lastX;
    g_eventInfo.y = g_lastY;
    g_eventInfo.pressure = 0;
    g_eventInfo.xTilt = 0;
    g_eventInfo.yTilt = 0;

    if (event->detail > 0 && event->detail < 32) {
        if (is_press)
            g_eventInfo.buttonsState |= 1 << (event->detail - 1);
        else
            g_eventInfo.buttonsState &= ~(1 << (event->detail - 1));
    }

    g_data.callback(&g_eventInfo);
    g_eventInfo.type = kEventTypeUnknown;
}

static void handle_event(xcb_generic_event_t *xcb_event) {
    if ((xcb_event->response_type & 0x7f) != XCB_GE_GENERIC)
        return;

    xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *) xcb_event;
    if (ge->extension != g_data.xiMajorOpcode)
        return;

    switch (ge->event_type) {
    case XCB_INPUT_RAW_MOTION:
        handle_raw_motion((xcb_input_raw_motion_event_t *) ge);
        break;
    case XCB_INPUT_RAW_BUTTON_PRESS:
        handle_raw_button((xcb_input_raw_button_press_event_t *) ge, 1);
        break;
    case XCB_INPUT_RAW_BUTTON_RELEASE:
        handle_raw_button((xcb_input_raw_button_press_event_t *) ge, 0);
        break;
    default:
        break;
    }
}

// How long to wait after the last motion/button activity before synthesizing
// a proximity-out (pen lifted out of range). Raw XI2 events carry no
// proximity notion of their own so this has to be inferred.
static const long kProximityTimeoutMs = 200;

static void check_proximity_timeout(void) {
    if (!g_inProximity)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long elapsed_ms = (now.tv_sec - g_lastEventTime.tv_sec) * 1000 +
                      (now.tv_nsec - g_lastEventTime.tv_nsec) / 1000000;
    if (elapsed_ms < kProximityTimeoutMs)
        return;

    g_inProximity = 0;
    g_eventInfo.type = kEventTypeProximityOut;
    g_eventInfo.time = (unsigned int) (now.tv_sec * 1000 + now.tv_nsec / 1000000);
    g_eventInfo.x = g_lastX;
    g_eventInfo.y = g_lastY;
    g_eventInfo.pressure = 0;
    g_eventInfo.xTilt = 0;
    g_eventInfo.yTilt = 0;
    g_data.callback(&g_eventInfo);
    g_eventInfo.type = kEventTypeUnknown;
}

static int check_events(unsigned int timeout) {
    int conn_fd = xcb_get_file_descriptor(g_data.connection);

    struct pollfd poll_fd;
    poll_fd.fd = conn_fd;
    poll_fd.events = POLLIN;

    poll(&poll_fd, 1, timeout);

    xcb_generic_event_t *xcb_event = NULL;
    while (xcb_event = xcb_poll_for_event(g_data.connection)) {
        handle_event(xcb_event);
        free(xcb_event);
    }

    check_proximity_timeout();

    return !xcb_connection_has_error(g_data.connection);
}


// ----------------
// Event Subscription
//

static int select_events() {
    // No free(), reply data belongs to cache.
    const xcb_query_extension_reply_t *ext_info;
    ext_info = xcb_get_extension_data(g_data.connection, &xcb_input_id);
    if (!ext_info || !ext_info->present)
        return 0;
    g_data.xiEventBase = ext_info->first_event;
    g_data.xiMajorOpcode = ext_info->major_opcode;

    // A client must announce the XI2 version it supports before any other
    // XI2 request (like XISelectEvents with XI2 event masks) will be
    // accepted by the server.
    xcb_input_xi_query_version_cookie_t ver_cookie;
    ver_cookie = xcb_input_xi_query_version(g_data.connection, 2, 2);
    xcb_generic_error_t *ver_error = NULL;
    xcb_input_xi_query_version_reply_t *ver_reply;
    ver_reply = xcb_input_xi_query_version_reply(g_data.connection, ver_cookie, &ver_error);
    if (ver_error || !ver_reply) {
        free(ver_error);
        free(ver_reply);
        return 0;
    }
    free(ver_reply);

    const xcb_setup_t *setup = xcb_get_setup(g_data.connection);
    if (!setup->roots_len)
        return 0;

    xcb_screen_iterator_t screen_itr = xcb_setup_roots_iterator(setup);
    xcb_window_t window = screen_itr.data->root;

    // Raw events (focus-independent, unlike regular XI2/XI1 device events)
    // for just our tablet's stylus device -- see the comment above
    // g_inProximity for why we use these instead of legacy XInput1 events.
    struct {
        xcb_input_event_mask_t header;
        uint32_t mask;
    } req;
    // Raw events can only be selected for XIAllDevices/XIAllMasterDevices,
    // not for an individual slave device id -- selecting on our specific
    // stylus id silently degrades under Xwayland (events only trickle
    // through opportunistically, e.g. while another client also holds a
    // valid raw selection). Select broadly and filter by sourceid instead.
    req.header.deviceid = XCB_INPUT_DEVICE_ALL;
    req.header.mask_len = 1;
    req.mask = XCB_INPUT_XI_EVENT_MASK_RAW_MOTION |
               XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS |
               XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE;

    xcb_input_xi_select_events(g_data.connection, window, 1, &req.header);

    xcb_flush(g_data.connection);
    return 1;
}


// ----------------
// Connection Setup and Device Selection.
//

static int match_token(xcb_str_t *name, const char *match) {
    const char *p, *q, *end;
    p = xcb_str_name(name);
    end = p + xcb_str_name_length(name);

    while (p != end) {
        while (p != end && isspace(*p))
            p++;
        if (p == end)
            break;

        for (q = match; *q && p != end && tolower(*p) == tolower(*q); q++)
            p++;
        if (!*q && (p == end || isspace(*p) || *p == ':'))
            return 1;

        while (p != end && ! isspace(*p))
            p++;
    }
    return 0;
}

static int check_device(const xcb_input_device_info_t *device,
                        const xcb_input_input_info_iterator_t *inputItr,
                        xcb_str_t *name) {
    if (device->device_use != XCB_INPUT_DEVICE_USE_IS_X_EXTENSION_DEVICE &&
        device->device_use != XCB_INPUT_DEVICE_USE_IS_X_EXTENSION_KEYBOARD &&
        device->device_use != XCB_INPUT_DEVICE_USE_IS_X_EXTENSION_POINTER)
        return 0;

    if (!g_requiredName) {
        if (!match_token(name, "stylus") && !match_token(name, "pen"))
            return 0;
    } else {
        const char *name_p = xcb_str_name(name);
        int name_len = xcb_str_name_length(name);
        if (name_len != strlen(g_requiredName) ||
            strncmp(name_p, g_requiredName, name_len))
            return 0;
    }

    int class_count = device->num_class_info;
    xcb_input_input_info_iterator_t input_itr = *inputItr;

    xcb_input_button_info_t *button_info = NULL;
    xcb_input_valuator_info_t *valuator_info = NULL;
    while (class_count-- > 0) {
        if (input_itr.data->class_id == XCB_INPUT_INPUT_CLASS_BUTTON)
            button_info = (xcb_input_button_info_t *) input_itr.data;
        else if (input_itr.data->class_id == XCB_INPUT_INPUT_CLASS_VALUATOR)
            valuator_info = (xcb_input_valuator_info_t *) input_itr.data;

        if (input_itr.rem == 0)
            break;
        xcb_input_input_info_next(&input_itr);
    }

    const int kMinStylusAxis = 3;
    if (!valuator_info ||
        valuator_info->axes_len < kMinStylusAxis)
        return 0;

    // Ok, this is probably a tablet stylus.
    g_data.device.id = device->device_id;

    xcb_input_axis_info_t *axis = xcb_input_valuator_info_axes(valuator_info);
    AxisInfo *dev_axis = &g_data.device.xAxis;
    for (int i = 0; i < kMinStylusAxis; i++) {
        dev_axis[i].min = axis[i].minimum;
        dev_axis[i].max = axis[i].maximum;
        dev_axis[i].resolution = axis[i].resolution;
    }

    g_data.device.nButtons = button_info ? (button_info->num_buttons > 32 ? 32 : button_info->num_buttons) : 0;
    g_data.device.hasTilt = valuator_info->axes_len > 4 ? 1 : 0;

    return 1;
}

static void check_devices(const xcb_input_list_input_devices_reply_t *r) {
    xcb_input_device_info_iterator_t dev_itr;
    xcb_input_input_info_iterator_t input_itr;
    xcb_str_iterator_t name_itr;

    int count = xcb_input_list_input_devices_devices_length(r);
    if (!count)
        return;

    dev_itr = xcb_input_list_input_devices_devices_iterator(r);
    input_itr = xcb_input_list_input_devices_infos_iterator(r);
    name_itr = xcb_input_list_input_devices_names_iterator(r);

    while (1) {
        xcb_input_device_info_t *dev = dev_itr.data;
        xcb_input_input_info_t *input = input_itr.data;
        xcb_str_t *name = name_itr.data;

        // input_itr is passed const here.
        fprintf(stderr, "XWINTAB-SCAN: dev id=%d use=%d name=%.*s classes=%d\n", dev->device_id, dev->device_use, xcb_str_name_length(name), xcb_str_name(name), dev->num_class_info);
        if (check_device(dev, &input_itr, name))
            return;

        if (--count == 0) break;

        xcb_input_device_info_next(&dev_itr);
        xcb_str_next(&name_itr);

        // Note that input_itr contains every device's input classes so we have
        // advance it multiple times per device.
        int class_count = dev->num_class_info;
        while (class_count--)
            xcb_input_input_info_next(&input_itr);
    }
}

static int setup() {
    g_data.connection = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(g_data.connection)) {
        xcb_disconnect(g_data.connection);
        g_data.connection = NULL;
        return 0;
    }

    xcb_input_list_input_devices_cookie_t dev_list_cookie;
    dev_list_cookie = xcb_input_list_input_devices(g_data.connection);

    xcb_generic_error_t *xcb_err = NULL;
    xcb_input_list_input_devices_reply_t *dev_list_reply;
    dev_list_reply = xcb_input_list_input_devices_reply(g_data.connection,
                                                        dev_list_cookie,
                                                        &xcb_err);
    if (xcb_err) {
        xcb_disconnect(g_data.connection);
        g_data.connection = NULL;
        return 0;
    }

    g_requiredName = getenv("XWINTAB_DEVICE");

    g_data.device.id = -1;
    check_devices(dev_list_reply);

    free(dev_list_reply);
    return 1;
}


// ----------------
// DLL Exports
//

// Start a connection to the X11 server and query device information.
// Returns zero if there was an error. Failure to find a suitable device
// is not considered an error.
int WINAPI Load() {
    return setup();
}

// Check if a suitable device was found and get the needed information.
// Returns NULL if no suitable device was found.
const DeviceInfo* WINAPI GetSelectedDevice() {
    if (g_data.device.id == -1)
        return NULL;
    return &g_data.device;
}

// Open the device and ask the X11 server to send us device events for the
// first root window. The callback function will called when a call to
// CheckEvents() finds a device event.
//
// This is currently only designed to be called once.
//
// Returns 0 if there was an error.
int WINAPI BeginEvents(EventCallback callback) {
    if (g_data.device.id == -1)
        return 0;
    g_data.callback = callback;
    return select_events();
}

// Causes the thread to wait for device events for the specified timeout
// (in milliseconds). Any relevant device events will be passed to the callback
// given in BeginEvents().
//
// This should be called from a background thread so as not to block the main
// UI thread of the Application.
//
// Returns 0 if there was an error.
int WINAPI CheckEvents(unsigned int timeout) {
    return check_events(timeout);
}

// Closes the connection. This should be called after any event handling thread
// has stopped.
int WINAPI Shutdown() {
    if (g_data.connection) {
        xcb_disconnect(g_data.connection);
        g_data.connection = NULL;
    }
    return 1;
}
