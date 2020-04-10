/*
 * Copyright �� 2004-2007 Peter Osterlund
 * Copyright �� 2008-2012 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *      Peter Osterlund (petero2@telia.com)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg-server.h>
#include <xserver-properties.h>
#include "eventcomm.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "synproto.h"
#include "synapticsstr.h"
#include <xf86.h>
#include <libevdev/libevdev.h>

#ifndef INPUT_PROP_BUTTONPAD
#define INPUT_PROP_BUTTONPAD 0x02
#endif
#ifndef INPUT_PROP_SEMI_MT
#define INPUT_PROP_SEMI_MT 0x03
#endif
#ifndef INPUT_PROP_TOPBUTTONPAD
#define INPUT_PROP_TOPBUTTONPAD 0x04
#endif
#ifndef ABS_MT_TOOL_Y
#define ABS_MT_TOOL_Y 0x3d
#endif

#define SYSCALL(call) while (((call) == -1) && (errno == EINTR))

#define LONG_BITS (sizeof(long) * 8)
#define NBITS(x) (((x) + LONG_BITS - 1) / LONG_BITS)
#define OFF(x)   ((x) % LONG_BITS)
#define LONG(x)  ((x) / LONG_BITS)
#define TEST_BIT(bit, array) ((array[LONG(bit)] >> OFF(bit)) & 1)

#define ABS_MT_MIN ABS_MT_SLOT
#define ABS_MT_MAX ABS_MT_TOOL_Y
#define ABS_MT_CNT (ABS_MT_MAX - ABS_MT_MIN + 1)

/**
 * Protocol-specific data.
 */
struct eventcomm_proto_data {
    /**
     * Do we need to grab the event device?
     * Note that in the current flow, this variable is always false and
     * exists for readability of the code.
     */
    BOOL need_grab;
    int st_to_mt_offset[2];
    double st_to_mt_scale[2];
    int axis_map[ABS_MT_CNT];
    int cur_slot;
    ValuatorMask **last_mt_vals;
    int num_touches;

    struct libevdev *evdev;
    enum libevdev_read_flag read_flag;

    int have_monotonic_clock;
};

#ifdef HAVE_LIBEVDEV_DEVICE_LOG_FUNCS
static void
libevdev_log_func(const struct libevdev *dev,
                  enum libevdev_log_priority priority,
                  void *data,
                  const char *file, int line, const char *func,
                  const char *format, va_list args)
_X_ATTRIBUTE_PRINTF(7, 0);

static void
libevdev_log_func(const struct libevdev *dev,
                  enum libevdev_log_priority priority,
                  void *data,
                  const char *file, int line, const char *func,
                  const char *format, va_list args)
{
    int verbosity;

    switch(priority) {
        case LIBEVDEV_LOG_ERROR: verbosity = 0; break;
        case LIBEVDEV_LOG_INFO: verbosity = 4; break;
        case LIBEVDEV_LOG_DEBUG:
        default:
            verbosity = 10;
            break;
    }

    LogVMessageVerbSigSafe(X_NOTICE, verbosity, format, args);
}
#endif

struct eventcomm_proto_data *
EventProtoDataAlloc(int fd)
{
    struct eventcomm_proto_data *proto_data;
    int rc;


    proto_data = calloc(1, sizeof(struct eventcomm_proto_data));
    if (!proto_data)
        return NULL;

    proto_data->st_to_mt_scale[0] = 1;
    proto_data->st_to_mt_scale[1] = 1;

    proto_data->evdev = libevdev_new();
    if (!proto_data->evdev) {
        rc = -1;
        goto out;
    }

#ifdef HAVE_LIBEVDEV_DEVICE_LOG_FUNCS
    libevdev_set_device_log_function(proto_data->evdev, libevdev_log_func,
                                     LIBEVDEV_LOG_DEBUG, NULL);
#endif

    rc = libevdev_set_fd(proto_data->evdev, fd);
    if (rc < 0) {
        goto out;
    }

    proto_data->read_flag = LIBEVDEV_READ_FLAG_NORMAL;

out:
    if (rc < 0) {
        if (proto_data && proto_data->evdev)
            libevdev_free(proto_data->evdev);
        free(proto_data);
        proto_data = NULL;
    }

    return proto_data;
}

static void
UninitializeTouch(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    struct eventcomm_proto_data *proto_data =
        (struct eventcomm_proto_data *) priv->proto_data;


    if (proto_data->last_mt_vals) {
        int i;

        free(proto_data->last_mt_vals);
        proto_data->last_mt_vals = NULL;
    }

    proto_data->num_touches = 0;
}

static void
InitializeTouch(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    struct eventcomm_proto_data *proto_data =
        (struct eventcomm_proto_data *) priv->proto_data;
    int i;

    proto_data->cur_slot = libevdev_get_current_slot(proto_data->evdev);
    proto_data->num_touches = 0;

    if (!proto_data->last_mt_vals) {
        xf86IDrvMsg(pInfo, X_WARNING,
                    "failed to allocate MT last values mask array\n");
        UninitializeTouch(pInfo);
        return;
    }


}

static Bool
EventDeviceOnHook(InputInfoPtr pInfo, SynapticsParameters * para)
{
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    struct eventcomm_proto_data *proto_data =
        (struct eventcomm_proto_data *) priv->proto_data;
    int ret;

    if (libevdev_get_fd(proto_data->evdev) != -1) {
        struct input_event ev;

        libevdev_change_fd(proto_data->evdev, pInfo->fd);

        /* re-sync libevdev's state, but we don't care about the actual
           events here */
        libevdev_next_event(proto_data->evdev, LIBEVDEV_READ_FLAG_FORCE_SYNC, &ev);
        while (libevdev_next_event(proto_data->evdev,
                    LIBEVDEV_READ_FLAG_SYNC, &ev) == LIBEVDEV_READ_STATUS_SYNC)
            ;

    } else
        libevdev_set_fd(proto_data->evdev, pInfo->fd);


    if (para->grab_event_device) {
        /* Try to grab the event device so that data don't leak to /dev/input/mice */

        ret = libevdev_grab(proto_data->evdev, LIBEVDEV_GRAB);
        if (ret < 0) {
            xf86IDrvMsg(pInfo, X_WARNING, "can't grab event device, errno=%d\n",
                        -ret);
            return FALSE;
        }
    }

    proto_data->need_grab = FALSE;

    ret = libevdev_set_clock_id(proto_data->evdev, CLOCK_MONOTONIC);
    proto_data->have_monotonic_clock = (ret == 0);

    InitializeTouch(pInfo);

    return TRUE;
}

static Bool
EventDeviceOffHook(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    struct eventcomm_proto_data *proto_data = priv->proto_data;

    UninitializeTouch(pInfo);
    libevdev_grab(proto_data->evdev, LIBEVDEV_UNGRAB);
    libevdev_set_log_function(NULL, NULL);
    libevdev_set_log_priority(LIBEVDEV_LOG_INFO); /* reset to default */

    return Success;
}

/**
 * Test if the device on the file descriptior is recognized as touchpad
 * device. Required bits for touchpad recognition are:
 * - ABS_X + ABS_Y for absolute axes
 * - ABS_PRESSURE or BTN_TOUCH
 * - BTN_TOOL_FINGER
 * - BTN_TOOL_PEN is _not_ set
 *
 * @param evdev Libevdev handle
 *
 * @return TRUE if the device is a touchpad or FALSE otherwise.
 */
static Bool
event_query_is_touchpad(struct libevdev *evdev)
{
    /* Check for ABS_X, ABS_Y, ABS_PRESSURE and BTN_TOOL_FINGER */
    if (!libevdev_has_event_type(evdev, EV_SYN) ||
        !libevdev_has_event_type(evdev, EV_ABS) ||
        !libevdev_has_event_type(evdev, EV_KEY))
        return FALSE;

    if (!libevdev_has_event_code(evdev, EV_ABS, ABS_X) ||
        !libevdev_has_event_code(evdev, EV_ABS, ABS_Y))
        return FALSE;

    /* we expect touchpad either report raw pressure or touches */
    if (!libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH) &&
        !libevdev_has_event_code(evdev, EV_ABS, ABS_PRESSURE))
        return FALSE;

    /* all Synaptics-like touchpad report BTN_TOOL_FINGER */
    if (!libevdev_has_event_code(evdev, EV_KEY, BTN_TOOL_FINGER) ||
        libevdev_has_event_code(evdev, EV_ABS, BTN_TOOL_PEN)) /* Don't match wacom tablets */
        return FALSE;

    if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_SLOT)) {
        if (libevdev_get_num_slots(evdev) == -1)
            return FALSE; /* Ignore fake MT devices */

        if (!libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X) ||
            !libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_Y))
            return FALSE;
    }

    return TRUE;
}

/**
 * Get absinfo information from the given file descriptor for the given
 * ABS_FOO code and store the information in min, max, fuzz and res.
 *
 * @param fd File descriptor to an event device
 * @param code Event code (e.g. ABS_X)
 * @param[out] min Minimum axis range
 * @param[out] max Maximum axis range
 * @param[out] fuzz Fuzz of this axis. If NULL, fuzz is ignored.
 * @param[out] res Axis resolution. If NULL or the current kernel does not
 * support the resolution field, res is ignored
 *
 * @return Zero on success, or errno otherwise.
 */
static int
event_get_abs(struct libevdev *evdev, int code,
              int *min, int *max, int *fuzz, int *res)
{
    const struct input_absinfo *abs;

    abs = libevdev_get_abs_info(evdev, code);
    *min = abs->minimum;
    *max = abs->maximum;

    /* We dont trust a zero fuzz as it probably is just a lazy value */
    if (fuzz && abs->fuzz > 0)
        *fuzz = abs->fuzz;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,30)
    if (res)
        *res = abs->resolution;
#endif

    return 0;
}


static Bool
EventQueryHardware(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    struct eventcomm_proto_data *proto_data = priv->proto_data;

    if (!event_query_is_touchpad(proto_data->evdev))
        return FALSE;

    xf86IDrvMsg(pInfo, X_PROBED, "touchpad found\n");

    return TRUE;
}

static Bool
SynapticsReadEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    struct eventcomm_proto_data *proto_data = priv->proto_data;
    int rc;
    static struct timeval last_event_time;

    rc = libevdev_next_event(proto_data->evdev, proto_data->read_flag, ev);
    if (rc < 0) {
        if (rc != -EAGAIN) {
            LogMessageVerbSigSafe(X_ERROR, 0, "%s: Read error %d\n", pInfo->name,
                    errno);
        } else if (proto_data->read_flag == LIBEVDEV_READ_FLAG_SYNC) {
            proto_data->read_flag = LIBEVDEV_READ_FLAG_NORMAL;
            return SynapticsReadEvent(pInfo, ev);
        }

        return FALSE;
    }

    /* SYN_DROPPED received in normal mode. Create a normal EV_SYN
       so we process what's in the queue atm, then ensure we sync
       next time */
    if (rc == LIBEVDEV_READ_STATUS_SYNC &&
        proto_data->read_flag == LIBEVDEV_READ_FLAG_NORMAL) {
        proto_data->read_flag = LIBEVDEV_READ_FLAG_SYNC;
        ev->type = EV_SYN;
        ev->code = SYN_REPORT;
        ev->value = 0;
        ev->time = last_event_time;
    } else if (ev->type == EV_SYN)
        last_event_time = ev->time;

    return TRUE;
}


Bool
EventReadHwState(InputInfoPtr pInfo,
                 struct CommData *comm, struct SynapticsHwState *hwRet)
{
    struct input_event ev;
    Bool v;
    struct SynapticsHwState *hw = comm->hwState;
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    SynapticsParameters *para = &priv->synpara;
    struct eventcomm_proto_data *proto_data = priv->proto_data;


    while (SynapticsReadEvent(pInfo, &ev)) {
        switch (ev.type) {
        case EV_SYN:
            switch (ev.code) {
            case SYN_REPORT:
                if (proto_data->have_monotonic_clock)
                    hw->millis = 1000 * ev.time.tv_sec + ev.time.tv_usec / 1000;
                else
                    hw->millis = GetTimeInMillis();
                SynapticsCopyHwState(hwRet, hw);
                return TRUE;
            }
            break;
        case EV_KEY:
        	if(ev.code==BTN_MOUSE)
        	{
        		hw->pressed=ev.value;
        	}
                break;


        case EV_ABS:
                switch (ev.code) {
                case ABS_X:
                    break;
                case ABS_Y:
                    break;
                case ABS_PRESSURE:
                    break;
                case ABS_MT_SLOT:
                    proto_data->cur_slot=ev.value;
                	break;
                case ABS_MT_TRACKING_ID:
                    if(ev.value<0){
                    	hw->finger[proto_data->cur_slot]=0;
                    	hw->fingersCount--;
                    	hw->x[proto_data->cur_slot]=-1;
                    	hw->y[proto_data->cur_slot]=-1;
                    	hw->z[proto_data->cur_slot]=-1;
                    }
                    else{
                    	hw->finger[proto_data->cur_slot]=1;
                    	hw->fingersCount++;
                    }
                	break;
                case ABS_MT_POSITION_X:
                	hw->x[proto_data->cur_slot]=ev.value;
                	break;
                case ABS_MT_POSITION_Y:
                	hw->y[proto_data->cur_slot]=ev.value;
                	break;
                case ABS_MT_PRESSURE:
                	hw->z[proto_data->cur_slot]=ev.value;
                	break;
                default:
//                    xf86DrvMsg(pInfo, X_PROBED, "Unknown packet %d = %d\n",ev.code,ev.value);
                	break;
                }
            break;
        }
    }
    return FALSE;
}

/* filter for the AutoDevProbe scandir on /dev/input */
static int
EventDevOnly(const struct dirent *dir)
{
    return strncmp(EVENT_DEV_NAME, dir->d_name, 5) == 0;
}

static void
event_query_touch(InputInfoPtr pInfo)
{

}

/**
 * Probe the open device for dimensions.
 */
static void
EventReadDevDimensions(InputInfoPtr pInfo)
{
    SynapticsPrivate *priv = (SynapticsPrivate *) pInfo->private;
    struct eventcomm_proto_data *proto_data = priv->proto_data;
    int i;

    proto_data = EventProtoDataAlloc(pInfo->fd);
    priv->proto_data = proto_data;

    for (i = 0; i < ABS_MT_CNT; i++)
        proto_data->axis_map[i] = -1;
    proto_data->cur_slot = -1;


}
static Bool
EventAutoDevProbe(InputInfoPtr pInfo, const char *device)
{
    /* We are trying to find the right eventX device or fall back to
       the psaux protocol and the given device from XF86Config */
    int i;
    Bool touchpad_found = FALSE;
    struct dirent **namelist;

    if (device) {
        int fd = -1;

        if (pInfo->flags & XI86_SERVER_FD)
            fd = pInfo->fd;
        else
            SYSCALL(fd = open(device, O_RDONLY));

        if (fd >= 0) {
            int rc;
            struct libevdev *evdev;

            rc = libevdev_new_from_fd(fd, &evdev);
            if (rc >= 0) {
                touchpad_found = event_query_is_touchpad(evdev);
                libevdev_free(evdev);
            }

            if (!(pInfo->flags & XI86_SERVER_FD))
                SYSCALL(close(fd));
        }


        /* if a device is set and not a touchpad (or already grabbed),
         * we must return FALSE.  Otherwise, we'll add a device that
         * wasn't requested for and repeat
         * f5687a6741a19ef3081e7fd83ac55f6df8bcd5c2. */
        return touchpad_found;
    }

    i = scandir(DEV_INPUT_EVENT, &namelist, EventDevOnly, alphasort);
    if (i < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "Couldn't open %s\n", DEV_INPUT_EVENT);
        return FALSE;
    }
    else if (i == 0) {
        xf86IDrvMsg(pInfo, X_ERROR,
                    "The /dev/input/event* device nodes seem to be missing\n");
        free(namelist);
        return FALSE;
    }

    while (i--) {
        char fname[64];
        int fd = -1;

        if (!touchpad_found) {
            int rc;
            struct libevdev *evdev;

            sprintf(fname, "%s/%s", DEV_INPUT_EVENT, namelist[i]->d_name);
            SYSCALL(fd = open(fname, O_RDONLY));
            if (fd < 0)
                continue;

            rc = libevdev_new_from_fd(fd, &evdev);
            if (rc >= 0) {
                touchpad_found = event_query_is_touchpad(evdev);
                libevdev_free(evdev);
                if (touchpad_found) {
                    xf86IDrvMsg(pInfo, X_PROBED, "auto-dev sets device to %s\n",
                                fname);
                    pInfo->options = xf86ReplaceStrOption(pInfo->options,
                                                          "Device",
                                                          fname);
                }
            }
            SYSCALL(close(fd));
        }
        free(namelist[i]);
    }

    free(namelist);

    if (!touchpad_found) {
        xf86IDrvMsg(pInfo, X_ERROR, "no synaptics event device found\n");
        return FALSE;
    }

    return TRUE;
}

struct SynapticsProtocolOperations event_proto_operations = {
    EventDeviceOnHook,
    EventDeviceOffHook,
    EventQueryHardware,
    EventReadHwState,
    EventAutoDevProbe,
    EventReadDevDimensions
};
