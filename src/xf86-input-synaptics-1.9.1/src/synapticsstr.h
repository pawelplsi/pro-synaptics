/*
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
 */

#ifndef	_SYNAPTICSSTR_H_
#define _SYNAPTICSSTR_H_

#include "synproto.h"

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 18
#define LogMessageVerbSigSafe xf86MsgVerb
#endif

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) > 19
#define NO_DRIVER_SCALING 1
#elif GET_ABI_MAJOR(ABI_XINPUT_VERSION) == 19 && GET_ABI_MINOR(ABI_XINPUT_VERSION) >= 2
/* as of 19.2, the server takes device resolution into account when scaling
   relative events from abs device, so we must not scale in synaptics. */
#define NO_DRIVER_SCALING 1
#endif

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 23
#define HAVE_THREADED_INPUT 1
#endif

#ifdef DBG
#undef DBG
#endif
#define DEBUG
#ifdef DEBUG
#define DBG(verb, ...) \
    xf86MsgVerb(X_INFO, verb, __VA_ARGS__)
#else
#define DBG(verb, msg, ...)     /* */
#endif

/******************************************************************************
 *		Definitions
 *					structs, typedefs, #defines, enums
 *****************************************************************************/
#define SYNAPTICS_MOVE_HISTORY	5
#define SYNAPTICS_MAX_TOUCHES	10
#define SYN_MAX_BUTTONS 12      /* Max number of mouse buttons */

/* Minimum and maximum values for scroll_button_repeat */
#define SBR_MIN 10
#define SBR_MAX 1000

enum OffState {
    TOUCHPAD_ON = 0,
    TOUCHPAD_OFF = 1,
    TOUCHPAD_TAP_OFF = 2,
};


typedef struct _SynapticsMoveHist {
    int x, y;
    CARD32 millis;
} SynapticsMoveHistRec;

typedef struct _SynapticsTouchAxis {
    const char *label;
    int min;
    int max;
    int res;
} SynapticsTouchAxisRec;

enum FingerMode{
	FM_NULL,
	FM_MOVE,
	FM_LEFTBTN,
	FM_RIGHTBTN,
	FM_MIDDLEBTN,
	FM_HORIZSCROLL,
	FM_VERTSCROLL,
	FM_MODIFIER,
};

enum SynapticsRegion{
	RG_NULL,
	RG_MOVE,
	RG_LEFTBTN,
	RG_RIGHTBTN,
	RG_MIDDLEBTN,
	RG_HORIZSCROLL,
	RG_VERTSCROLL,
	RG_MODIFIER,
};

typedef struct _SynapticsParameters {
    /* Parameter data */
    int left_edge, right_edge, top_edge, bottom_edge;   /* edge coordinates absolute */
    int finger_low, finger_high, finger_press;  /* finger detection values in Z-values */
    int tap_time;
    int tap_move;               /* max. tapping time and movement in packets and coord. */
    int single_tap_timeout;     /* timeout to recognize a single tap */
    int tap_time_2;             /* max. tapping time for double taps */
    int click_time;             /* The duration of a single click */
    Bool clickpad;              /* Device is a has integrated buttons */
    Bool has_secondary_buttons; /* Device has a top soft-button area */
    int clickpad_ignore_motion_time; /* Ignore motion for X ms after a click */
    int emulate_mid_button_time;        /* Max time between left and right button presses to
                                           emulate a middle button press. */
    int emulate_twofinger_z;    /* pressure threshold to emulate two finger touch (for Alps) */
    int emulate_twofinger_w;    /* Finger width threshold to emulate two finger touch */
    int scroll_dist_vert;       /* Scrolling distance in absolute coordinates */
    int scroll_dist_horiz;      /* Scrolling distance in absolute coordinates */
    Bool scroll_edge_vert;      /* Enable/disable vertical scrolling on right edge */
    Bool scroll_edge_horiz;     /* Enable/disable horizontal scrolling on left edge */
    Bool scroll_edge_corner;    /* Enable/disable continuous edge scrolling when in the corner */
    Bool scroll_twofinger_vert; /* Enable/disable vertical two-finger scrolling */
    Bool scroll_twofinger_horiz;        /* Enable/disable horizontal two-finger scrolling */
    double min_speed, max_speed, accl;  /* movement parameters */

    Bool updown_button_scrolling;       /* Up/Down-Button scrolling or middle/double-click */
    Bool leftright_button_scrolling;    /* Left/right-button scrolling, or two lots of middle button */
    Bool updown_button_repeat;  /* If up/down button being used to scroll, auto-repeat? */
    Bool leftright_button_repeat;       /* If left/right button being used to scroll, auto-repeat? */
    int scroll_button_repeat;   /* time, in milliseconds, between scroll events being
                                 * sent when holding down scroll buttons */
    int touchpad_off;           /* Switches the touchpad off
                                 * 0 : Not off
                                 * 1 : Off
                                 * 2 : Only tapping and scrolling off
                                 */
    Bool locked_drags;          /* Enable locked drags */
    int locked_drag_time;       /* timeout for locked drags */
    Bool circular_scrolling;    /* Enable circular scrolling */
    double scroll_dist_circ;    /* Scrolling angle radians */
    int circular_trigger;       /* Trigger area for circular scrolling */
    Bool circular_pad;          /* Edge has an oval or circular shape */
    Bool palm_detect;           /* Enable Palm Detection */
    int palm_min_width;         /* Palm detection width */
    int palm_min_z;             /* Palm detection depth */
    double coasting_speed;      /* Coasting threshold scrolling speed in scrolls/s */
    double coasting_friction;   /* Number of scrolls per second per second to change coasting speed */
    int press_motion_min_z;     /* finger pressure at which minimum pressure motion factor is applied */
    int press_motion_max_z;     /* finger pressure at which maximum pressure motion factor is applied */
    double press_motion_min_factor;     /* factor applied on speed when finger pressure is at minimum */
    double press_motion_max_factor;     /* factor applied on speed when finger pressure is at minimum */
    Bool resolution_detect;     /* report pad size to xserver? */
    Bool grab_event_device;     /* grab event device for exclusive use? */
    Bool tap_and_drag_gesture;  /* Switches the tap-and-drag gesture on/off */
    unsigned int resolution_horiz;      /* horizontal resolution of touchpad in units/mm */
    unsigned int resolution_vert;       /* vertical resolution of touchpad in units/mm */
    int area_left_edge, area_right_edge, area_top_edge, area_bottom_edge;       /* area coordinates absolute */
    int softbutton_areas[4][4]; /* soft button area coordinates, 0 => right, 1 => middle , 2 => secondary right, 3 => secondary middle button */
    int hyst_x, hyst_y;         /* x and y width of hysteresis box */

    int maxDeltaMM;               /* maximum delta movement (vector length) in mm */
} SynapticsParameters;

struct _SynapticsPrivateRec {

    SynapticsParameters synpara;        /* Default parameter settings, read from
                                           the X config file */\

    struct SynapticsProtocolOperations *proto_ops;
    void *proto_data;           /* protocol-specific data */

    enum FingerMode fingerModes[5];
    Bool ongoingBtnPress;
    int OngoingBtnId;
    int lastX[5];
    int lastY[5];
    double fracX;
    double fracY;


    struct SynapticsHwState *hwState;
    const char *device;         /* device node */
    CARD32 timer_time;          /* when timer last fired */
    OsTimerPtr timer;           /* for up/down-button repeat, tap processing, etc */
    struct CommData comm;
    struct SynapticsHwState *local_hw_state;    /* used in place of local hw state variables */
    int count_packet_finger;    /* packet counter with finger on the touchpad */
    int button_delay_millis;    /* button delay for 3rd button emulation */
    Bool prev_up;               /* Previous up button value, for double click emulation */
    CARD32 last_motion_millis;  /* time of the last motion */
    int tap_max_fingers;        /* Max number of fingers seen since entering start state */
    int tap_button;             /* Which button started the tap processing */
    int resx, resy;             /*resolution of coordinates as detected in units/mm */
    int scroll_axis_horiz;      /* Horizontal smooth-scrolling axis */
    int scroll_axis_vert;       /* Vertical smooth-scrolling axis */
    ValuatorMask *scroll_events_mask;   /* ValuatorMask for smooth-scrolling */


};

#endif                          /* _SYNAPTICSSTR_H_ */
