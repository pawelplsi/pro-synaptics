#define MAXX 2033
#define MAXY 1332
#define VERTSCROLLDELTA 16
#define HORIZSCROLLDELTA 16

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg-server.h>
#include <unistd.h>
#include <misc.h>
#include <xf86.h>
#include <math.h>
#include <stdio.h>
#include <xf86_OSproc.h>
#include <xf86Xinput.h>
#include <exevents.h>

#include <X11/Xatom.h>
#include <X11/extensions/XI2.h>
#include <xserver-properties.h>
#include <ptrveloc.h>

#include "synapticsstr.h"
enum EdgeType
{
	NO_EDGE=0,
	BOTTOM_EDGE=1,
	TOP_EDGE=2,
	LEFT_EDGE=4,
	RIGHT_EDGE=8,
	LEFT_BOTTOM_EDGE=BOTTOM_EDGE|LEFT_EDGE,
	RIGHT_BOTTOM_EDGE=BOTTOM_EDGE|RIGHT_EDGE,
	RIGHT_TOP_EDGE=TOP_EDGE|RIGHT_EDGE,
	LEFT_TOP_EDGE=TOP_EDGE|LEFT_EDGE
};

/*
 * We expect to be receiving a steady 80 packets/sec (which gives 40
 * reports/sec with more than one finger on the pad, as Advanced Gesture Mode
 * requires two PS/2 packets per report).  Instead of a random scattering of
 * magic 13 and 20ms numbers scattered throughout the driver, introduce
 * POLL_MS as 14ms, which is slightly less than 80Hz.  13ms is closer to
 * 80Hz, but if the kernel event reporting was even slightly delayed,
 * we would produce synthetic motion followed immediately by genuine
 * motion, so use 14.
 *
 * We use this to call back at a constant rate to at least produce the
 * illusion of smooth motion.  It works a lot better than you'd expect.
 */
#define POLL_MS 14

#define MAX(a, b) (((a)>(b))?(a):(b))
#define MIN(a, b) (((a)<(b))?(a):(b))
#define TIME_DIFF(a, b) ((int)((a)-(b)))

#define SQR(x) ((x) * (x))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define INPUT_BUFFER_SIZE 200

/*****************************************************************************
 * Forward declaration
 ****************************************************************************/
static int SynapticsPreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
static void SynapticsUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
static Bool DeviceControl(DeviceIntPtr, int);
static void ReadInput(InputInfoPtr);
static int HandleState(InputInfoPtr, struct SynapticsHwState *, CARD32 now, Bool from_timer);
static int ControlProc(InputInfoPtr, xDeviceCtl *);
static int SwitchMode(ClientPtr, DeviceIntPtr, int);
static int DeviceInit(DeviceIntPtr);
static int DeviceOn(DeviceIntPtr);
static int DeviceOff(DeviceIntPtr);
static int DeviceClose(DeviceIntPtr);
static Bool QueryHardware(InputInfoPtr);
static void ReadDevDimensions(InputInfoPtr);
static void SanitizeDimensions(InputInfoPtr pInfo);

void InitDeviceProperties(InputInfoPtr pInfo);
int SetProperty(DeviceIntPtr dev, Atom property, XIPropertyValuePtr prop, BOOL checkonly);

const static struct
{
	const char *name;
	struct SynapticsProtocolOperations *proto_ops;
} protocols[]=
{
//#ifdef BUILD_EVENTCOMM
        {	"event", &event_proto_operations},
//#endif
        {NULL, NULL}};

InputDriverRec SYNAPTICS=
{1, "synaptics",
NULL, SynapticsPreInit, SynapticsUnInit,
NULL,
NULL,
#ifdef XI86_DRV_CAP_SERVER_FD
        XI86_DRV_CAP_SERVER_FD
#endif
        };

static XF86ModuleVersionInfo VersionRec=
{"synaptics",
MODULEVENDORSTRING,
MODINFOSTRING1,
MODINFOSTRING2,
XORG_VERSION_CURRENT, PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
ABI_CLASS_XINPUT,
ABI_XINPUT_VERSION,
MOD_CLASS_XINPUT,
{0, 0, 0, 0}};

static pointer SetupProc(pointer module, pointer options, int *errmaj, int *errmin)
{
	xf86AddInputDriver(&SYNAPTICS, module, 0);
	return module;
}

_X_EXPORT XF86ModuleData synapticsModuleData=
{&VersionRec, &SetupProc,
NULL};

/*****************************************************************************
 *	Function Definitions
 ****************************************************************************/
static inline void SynapticsCloseFd(InputInfoPtr pInfo)
{
	if(pInfo->fd>-1&&!(pInfo->flags&XI86_SERVER_FD))
	{
		xf86CloseSerial(pInfo->fd);
		pInfo->fd=-1;
	}
}

/**
 * Fill in default dimensions for backends that cannot query the hardware.
 * Eventually, we want the edges to be 1900/5400 for x, 1900/4000 for y.
 * These values are based so that calculate_edge_widths() will give us the
 * right values.
 *
 * The default values 1900, etc. come from the dawn of time, when men where
 * men, or possibly apes.
 */
static void SanitizeDimensions(InputInfoPtr pInfo)
{

}

static Bool SetDeviceAndProtocol(InputInfoPtr pInfo)
{
	SynapticsPrivate *priv=pInfo->private;
	char *proto, *device;
	int i;

	proto=xf86SetStrOption(pInfo->options, "Protocol", NULL);
	device=xf86SetStrOption(pInfo->options, "Device", NULL);

	/* If proto is auto-dev, unset and let the code do the rest */
	if(proto&&!strcmp(proto, "auto-dev"))
	{
		free(proto);
		proto= NULL;
	}

	for(i=0; protocols[i].name; i++)
	{
		if((!device||!proto)&&protocols[i].proto_ops->AutoDevProbe&&protocols[i].proto_ops->AutoDevProbe(pInfo, device))
			break;
		else if(proto&&!strcmp(proto, protocols[i].name))
			break;
	}
	free(proto);
	free(device);

	priv->proto_ops=protocols[i].proto_ops;

	return (priv->proto_ops!=NULL);
}

/* Area options support both percent values and absolute values. This is
 * awkward. The xf86Set* calls will print to the log, but they'll
 * also print an error if we request a percent value but only have an
 * int. So - check first for percent, then call xf86Set* again to get
 * the log message.
 */
static int set_percent_option(pointer options, const char *optname, const int range, const int offset, const int default_value)
{
	int result;
	double percent=xf86CheckPercentOption(options, optname, -1);

	if(percent>=0.0)
	{
		percent=xf86SetPercentOption(options, optname, -1);
		result=percent/100.0*range+offset;
	}
	else
		result=xf86SetIntOption(options, optname, default_value);

	return result;
}
//TODO HUUUJ
static void set_default_parameters(InputInfoPtr pInfo)
{
	SynapticsPrivate *priv=pInfo->private; /* read-only */
	pointer opts=pInfo->options; /* read-only */
	SynapticsParameters *pars=&priv->synpara; /* modified */

	int horizScrollDelta, vertScrollDelta; /* pixels */
	int tapMove; /* pixels */
	int l, r, t, b; /* left, right, top, bottom */
	double accelFactor; /* 1/pixels */
	int fingerLow, fingerHigh; /* pressure */
	int emulateTwoFingerMinZ; /* pressure */
	int emulateTwoFingerMinW; /* width */
	int pressureMotionMinZ, pressureMotionMaxZ; /* pressure */
	int palmMinWidth, palmMinZ; /* pressure */
	int tapButton1, tapButton2, tapButton3;
	int clickFinger1, clickFinger2, clickFinger3;
	Bool vertEdgeScroll, horizEdgeScroll;
	Bool vertTwoFingerScroll, horizTwoFingerScroll;
	int horizResolution=1;
	int vertResolution=1;
	int width, height, diag, range;
	int horizHyst, vertHyst;
	int middle_button_timeout;
	int grab_event_device=0;
	const char *source;

	/* The synaptics specs specify typical edge widths of 4% on x, and 5.4% on
	 * y (page 7) [Synaptics TouchPad Interfacing Guide, 510-000080 - A
	 * Second Edition, http://www.synaptics.com/support/dev_support.cfm, 8 Sep
	 * 2008]. We use 7% for both instead for synaptics devices, and 15% for
	 * ALPS models.
	 * http://bugs.freedesktop.org/show_bug.cgi?id=21214
	 *
	 * If the range was autodetected, apply these edge widths to all four
	 * sides.
	 */

	diag=sqrt(width*width+height*height);

	/* Again, based on typical x/y range and defaults */
	horizScrollDelta=diag*.020;
	vertScrollDelta=diag*.020;
	tapMove=diag*.044;
	accelFactor=200.0/diag; /* trial-and-error */

	/* hysteresis, assume >= 0 is a detected value (e.g. evdev fuzz) */
	horizHyst=pars->hyst_x>=0 ? pars->hyst_x : diag*0.005;
	vertHyst=pars->hyst_y>=0 ? pars->hyst_y : diag*0.005;

	/* Enable vert edge scroll if we can't detect doubletap */

	/* Enable twofinger scroll if we can detect doubletap */
	horizTwoFingerScroll= FALSE;

	/* Use resolution reported by hardware if available */
	if((priv->resx>0)&&(priv->resy>0))
	{
		horizResolution=priv->resx;
		vertResolution=priv->resy;
	}

	/* set the parameters */
	pars->left_edge=xf86SetIntOption(opts, "LeftEdge", l);
	pars->right_edge=xf86SetIntOption(opts, "RightEdge", r);
	pars->top_edge=xf86SetIntOption(opts, "TopEdge", t);
	pars->bottom_edge=xf86SetIntOption(opts, "BottomEdge", b);

	pars->hyst_x=set_percent_option(opts, "HorizHysteresis", width, 0, horizHyst);
	pars->hyst_y=set_percent_option(opts, "VertHysteresis", height, 0, vertHyst);

	pars->finger_low=xf86SetIntOption(opts, "FingerLow", fingerLow);
	pars->finger_high=xf86SetIntOption(opts, "FingerHigh", fingerHigh);
	pars->tap_time=xf86SetIntOption(opts, "MaxTapTime", 180);
	pars->tap_move=xf86SetIntOption(opts, "MaxTapMove", tapMove);
	pars->tap_time_2=xf86SetIntOption(opts, "MaxDoubleTapTime", 180);
	pars->click_time=xf86SetIntOption(opts, "ClickTime", 100);
	pars->clickpad=xf86SetBoolOption(opts, "ClickPad", pars->clickpad); /* Probed */
	if(pars->clickpad)
		pars->has_secondary_buttons=xf86SetBoolOption(opts, "HasSecondarySoftButtons", pars->has_secondary_buttons);
	pars->clickpad_ignore_motion_time=100; /* ms */
	/* middle mouse button emulation on a clickpad? nah, you're joking */
	middle_button_timeout=pars->clickpad ? 0 : 75;
	pars->emulate_mid_button_time=xf86SetIntOption(opts, "EmulateMidButtonTime", middle_button_timeout);
	pars->emulate_twofinger_z=xf86SetIntOption(opts, "EmulateTwoFingerMinZ", emulateTwoFingerMinZ);
	pars->emulate_twofinger_w=xf86SetIntOption(opts, "EmulateTwoFingerMinW", emulateTwoFingerMinW);
	pars->scroll_dist_vert=xf86SetIntOption(opts, "VertScrollDelta", vertScrollDelta);
	pars->scroll_dist_horiz=xf86SetIntOption(opts, "HorizScrollDelta", horizScrollDelta);
	pars->scroll_edge_vert=xf86SetBoolOption(opts, "VertEdgeScroll", vertEdgeScroll);
	pars->scroll_edge_horiz=xf86SetBoolOption(opts, "HorizEdgeScroll", horizEdgeScroll);
	pars->scroll_edge_corner=xf86SetBoolOption(opts, "CornerCoasting", FALSE);
	pars->scroll_twofinger_vert=xf86SetBoolOption(opts, "VertTwoFingerScroll", vertTwoFingerScroll);
	pars->scroll_twofinger_horiz=xf86SetBoolOption(opts, "HorizTwoFingerScroll", horizTwoFingerScroll);
	pars->touchpad_off=xf86SetIntOption(opts, "TouchpadOff", TOUCHPAD_ON);

	pars->scroll_button_repeat=xf86SetIntOption(opts, "ScrollButtonRepeat", 100);

	pars->locked_drags=xf86SetBoolOption(opts, "LockedDrags", FALSE);
	pars->locked_drag_time=xf86SetIntOption(opts, "LockedDragTimeout", 5000);
	pars->circular_scrolling=xf86SetBoolOption(opts, "CircularScrolling",
	FALSE);
	pars->circular_trigger=xf86SetIntOption(opts, "CircScrollTrigger", 0);
	pars->circular_pad=xf86SetBoolOption(opts, "CircularPad", FALSE);
	pars->palm_detect=xf86SetBoolOption(opts, "PalmDetect", FALSE);
	pars->palm_min_width=xf86SetIntOption(opts, "PalmMinWidth", palmMinWidth);
	pars->palm_min_z=xf86SetIntOption(opts, "PalmMinZ", palmMinZ);
	pars->single_tap_timeout=xf86SetIntOption(opts, "SingleTapTimeout", 180);
	pars->press_motion_min_z=xf86SetIntOption(opts, "PressureMotionMinZ", pressureMotionMinZ);
	pars->press_motion_max_z=xf86SetIntOption(opts, "PressureMotionMaxZ", pressureMotionMaxZ);

	pars->min_speed=xf86SetRealOption(opts, "MinSpeed", 0.4);
	pars->max_speed=xf86SetRealOption(opts, "MaxSpeed", 0.7);
	pars->accl=xf86SetRealOption(opts, "AccelFactor", accelFactor);
	pars->scroll_dist_circ=xf86SetRealOption(opts, "CircScrollDelta", 0.1);
	pars->coasting_speed=xf86SetRealOption(opts, "CoastingSpeed", 20.0);
	pars->coasting_friction=xf86SetRealOption(opts, "CoastingFriction", 50);
	pars->press_motion_min_factor=xf86SetRealOption(opts, "PressureMotionMinFactor", 1.0);
	pars->press_motion_max_factor=xf86SetRealOption(opts, "PressureMotionMaxFactor", 1.0);

	/* Only grab the device by default if it's not coming from a config
	 backend. This way we avoid the device being added twice and sending
	 duplicate events.
	 */
	source=xf86CheckStrOption(opts, "_source", NULL);
	if(source==NULL||strncmp(source, "server/", 7)!=0)
		grab_event_device= TRUE;
	pars->grab_event_device=xf86SetBoolOption(opts, "GrabEventDevice", grab_event_device);

	pars->tap_and_drag_gesture=xf86SetBoolOption(opts, "TapAndDragGesture",
	TRUE);
	pars->resolution_horiz=xf86SetIntOption(opts, "HorizResolution", horizResolution);
	pars->resolution_vert=xf86SetIntOption(opts, "VertResolution", vertResolution);
	if(pars->resolution_horiz<=0)
	{
		xf86IDrvMsg(pInfo, X_ERROR, "Invalid X resolution, using 1 instead.\n");
		pars->resolution_horiz=1;
	}
	if(pars->resolution_vert<=0)
	{
		xf86IDrvMsg(pInfo, X_ERROR, "Invalid Y resolution, using 1 instead.\n");
		pars->resolution_vert=1;
	}

	/* Touchpad sampling rate is too low to detect all movements.
	 A user may lift one finger and put another one down within the same
	 EV_SYN or even between samplings so the driver doesn't notice at all.

	 We limit the movement to 20 mm within one event, that is more than
	 recordings showed is needed (17mm on a T440).
	 */
	if(pars->resolution_horiz>1&&pars->resolution_vert>1)
		pars->maxDeltaMM=20;
	else
	{
		/* on devices without resolution set the vector length to 0.25 of
		 the touchpad diagonal */
		pars->maxDeltaMM=diag*0.25;
	}

	/* Warn about (and fix) incorrectly configured TopEdge/BottomEdge parameters */
	if(pars->top_edge>pars->bottom_edge)
	{
		int tmp=pars->top_edge;

		pars->top_edge=pars->bottom_edge;
		pars->bottom_edge=tmp;
		xf86IDrvMsg(pInfo, X_WARNING, "TopEdge is bigger than BottomEdge. Fixing.\n");
	}
}

Bool SynapticsIsSoftButtonAreasValid(int *values)
{
	Bool right_disabled= FALSE;
	Bool middle_disabled= FALSE;

	enum
	{
		/* right button left, right, top, bottom */
		RBL=0, RBR=1, RBT=2, RBB=3,
		/* middle button left, right, top, bottom */
		MBL=4, MBR=5, MBT=6, MBB=7,
	};

	/* Check right button area */
	if((((values[RBL]!=0)&&(values[RBR]!=0))&&(values[RBL]>values[RBR]))
	        ||(((values[RBT]!=0)&&(values[RBB]!=0))&&(values[RBT]>values[RBB])))
		return FALSE;

	/* Check middle button area */
	if((((values[MBL]!=0)&&(values[MBR]!=0))&&(values[MBL]>values[MBR]))
	        ||(((values[MBT]!=0)&&(values[MBB]!=0))&&(values[MBT]>values[MBB])))
		return FALSE;

	if(values[RBL]==0&&values[RBR]==0&&values[RBT]==0&&values[RBB]==0)
		right_disabled= TRUE;

	if(values[MBL]==0&&values[MBR]==0&&values[MBT]==0&&values[MBB]==0)
		middle_disabled= TRUE;

	if(!right_disabled&&((values[RBL]&&values[RBL]==values[RBR])||(values[RBT]&&values[RBT]==values[RBB])))
		return FALSE;

	if(!middle_disabled&&((values[MBL]&&values[MBL]==values[MBR])||(values[MBT]&&values[MBT]==values[MBB])))
		return FALSE;

	/* Check for overlapping button areas */
	if(!right_disabled&&!middle_disabled)
	{
		int right_left=values[RBL] ? values[RBL] : INT_MIN;
		int right_right=values[RBR] ? values[RBR] : INT_MAX;
		int right_top=values[RBT] ? values[RBT] : INT_MIN;
		int right_bottom=values[RBB] ? values[RBB] : INT_MAX;
		int middle_left=values[MBL] ? values[MBL] : INT_MIN;
		int middle_right=values[MBR] ? values[MBR] : INT_MAX;
		int middle_top=values[MBT] ? values[MBT] : INT_MIN;
		int middle_bottom=values[MBB] ? values[MBB] : INT_MAX;

		/* If areas overlap in the Y axis */
		if((right_bottom<=middle_bottom&&right_bottom>=middle_top)||(right_top<=middle_bottom&&right_top>=middle_top))
		{
			/* Check for overlapping left edges */
			if((right_left<middle_left&&right_right>middle_left)||(middle_left<right_left&&middle_right>right_left))
				return FALSE;

			/* Check for overlapping right edges */
			if((right_right>middle_right&&right_left<middle_right)||(middle_right>right_right&&middle_left<right_right))
				return FALSE;
		}

		/* If areas overlap in the X axis */
		if((right_left>=middle_left&&right_left<=middle_right)||(right_right>=middle_left&&right_right<=middle_right))
		{
			/* Check for overlapping top edges */
			if((right_top<middle_top&&right_bottom>middle_top)||(middle_top<right_top&&middle_bottom>right_top))
				return FALSE;

			/* Check for overlapping bottom edges */
			if((right_bottom>middle_bottom&&right_top<middle_bottom)||(middle_bottom>right_bottom&&middle_top<right_bottom))
				return FALSE;
		}
	}

	return TRUE;
}

//TODO Dzikie węże
static double SynapticsAccelerationProfile(DeviceIntPtr dev, DeviceVelocityPtr vel, double velocity, double thr, double acc)
{
	InputInfoPtr pInfo=dev->public.devicePrivate;
	SynapticsPrivate *priv=(SynapticsPrivate *) (pInfo->private);
	SynapticsParameters *para=&priv->synpara;

	double accelfct;

	/*
	 * synaptics accel was originally base on device coordinate based
	 * velocity, which we recover this way so para->accl retains its scale.
	 */
	velocity/=vel->const_acceleration;

	/* speed up linear with finger velocity */
	para->accl=0.082;
	accelfct=velocity*para->accl;

	/* clip acceleration factor */
	if(accelfct>2.5)
		accelfct=2.5;
	else if(accelfct<0.5)
		accelfct=0.5;
//		xf86DrvMsg(pInfo, X_PROBED, "XDD %f\n",acc);

		if(accelfct!=accelfct)
		{
			xf86DrvMsg(pInfo, X_PROBED, "HUJ\nKURWA\nJEBAĆ\nPOLICJE\n",accelfct);
		}
	return accelfct;
}

static int SynapticsPreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
	SynapticsPrivate *priv;

	/* allocate memory for SynapticsPrivateRec */
	priv=calloc(1, sizeof(SynapticsPrivate));
	if(!priv)
		return BadAlloc;

	pInfo->type_name= XI_TOUCHPAD;
	pInfo->device_control=DeviceControl;
	pInfo->read_input=ReadInput;
	pInfo->control_proc=ControlProc;
	pInfo->switch_mode=SwitchMode;
	pInfo->private=priv;

	/* allocate now so we don't allocate in the signal handler */
	priv->timer=TimerSet(NULL, 0, 0, NULL, NULL);
	if(!priv->timer)
	{
		free(priv);
		return BadAlloc;
	}

	/* may change pInfo->options */
	if(!SetDeviceAndProtocol(pInfo))
	{
		xf86IDrvMsg(pInfo, X_ERROR, "Synaptics driver unable to detect protocol\n");
		goto SetupProc_fail;
	}

	priv->device=xf86FindOptionValue(pInfo->options, "Device");

	/* open the touchpad device */
	pInfo->fd=xf86OpenSerial(pInfo->options);
	if(pInfo->fd==-1)
	{
		xf86IDrvMsg(pInfo, X_ERROR, "Synaptics driver unable to open device\n");
		goto SetupProc_fail;
	}
	xf86ErrorFVerb(6, "port opened successfully\n");

	/* initialize variables */
	priv->count_packet_finger=0;
	priv->tap_button=0;
	priv->synpara.hyst_x=-1;
	priv->synpara.hyst_y=-1;

	/* read hardware dimensions */
	ReadDevDimensions(pInfo);
	set_default_parameters(pInfo);

#ifndef NO_DRIVER_SCALING
	CalculateScalingCoeffs(priv);
#endif

	priv->comm.buffer=XisbNew(pInfo->fd, INPUT_BUFFER_SIZE);

	if(!QueryHardware(pInfo))
	{
		xf86IDrvMsg(pInfo, X_ERROR, "Unable to query/initialize Synaptics hardware.\n");
		goto SetupProc_fail;
	}

	xf86ProcessCommonOptions(pInfo, pInfo->options);

	if(priv->comm.buffer)
	{
		XisbFree(priv->comm.buffer);
		priv->comm.buffer= NULL;
	}
	SynapticsCloseFd(pInfo);

	return Success;

	SetupProc_fail: SynapticsCloseFd(pInfo);

	if(priv->comm.buffer)
		XisbFree(priv->comm.buffer);
	free(priv->proto_data);
	free(priv->timer);
	free(priv);
	pInfo->private= NULL;
	return BadAlloc;
}

/*
 *  Uninitialize the device.
 */
static void SynapticsUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
	SynapticsPrivate *priv=((SynapticsPrivate *) pInfo->private);

	if(priv&&priv->timer)
		free(priv->timer);
	if(priv&&priv->proto_data)
		free(priv->proto_data);
	if(priv&&priv->scroll_events_mask)
		valuator_mask_free(&priv->scroll_events_mask);
	free(pInfo->private);
	pInfo->private= NULL;
	xf86DeleteInput(pInfo, 0);
}

/*
 *  Alter the control parameters for the mouse. Note that all special
 *  protocol values are handled by dix.
 */
static void SynapticsCtrl(DeviceIntPtr device, PtrCtrl * ctrl)
{
}

static int DeviceControl(DeviceIntPtr dev, int mode)
{
	Bool RetValue;

	switch(mode)
	{
		case DEVICE_INIT:
			RetValue=DeviceInit(dev);
			break;
		case DEVICE_ON:
			RetValue=DeviceOn(dev);
			break;
		case DEVICE_OFF:
			RetValue=DeviceOff(dev);
			break;
		case DEVICE_CLOSE:
			RetValue=DeviceClose(dev);
			break;
		default:
			RetValue= BadValue;
	}

	return RetValue;
}

static int DeviceOn(DeviceIntPtr dev)
{
	InputInfoPtr pInfo=dev->public.devicePrivate;
	SynapticsPrivate *priv=(SynapticsPrivate *) (pInfo->private);

	DBG(3, "Synaptics DeviceOn called\n");

	pInfo->fd=xf86OpenSerial(pInfo->options);
	if(pInfo->fd==-1)
	{
		xf86IDrvMsg(pInfo, X_WARNING, "cannot open input device\n");
		return !Success;
	}

	if(priv->proto_ops->DeviceOnHook&&!priv->proto_ops->DeviceOnHook(pInfo, &priv->synpara))
		goto error;

	priv->comm.buffer=XisbNew(pInfo->fd, INPUT_BUFFER_SIZE);
	if(!priv->comm.buffer)
		goto error;

	xf86FlushInput(pInfo->fd);

	/* reinit the pad */
	if(!QueryHardware(pInfo))
		goto error;

	xf86AddEnabledDevice(pInfo);
	dev->public.on= TRUE;

	return Success;

	error: if(priv->comm.buffer)
	{
		XisbFree(priv->comm.buffer);
		priv->comm.buffer= NULL;
	}
	SynapticsCloseFd(pInfo);
	return !Success;
}

static void SynapticsReset(SynapticsPrivate * priv)
{
	int i;

	SynapticsResetHwState(priv->hwState);
	SynapticsResetHwState(priv->local_hw_state);
	SynapticsResetHwState(priv->comm.hwState);

	priv->count_packet_finger=0;
	priv->last_motion_millis=0;
	priv->tap_button=0;
}

static int DeviceOff(DeviceIntPtr dev)
{
	InputInfoPtr pInfo=dev->public.devicePrivate;
	SynapticsPrivate *priv=(SynapticsPrivate *) (pInfo->private);
	Bool rc= Success;

	DBG(3, "Synaptics DeviceOff called\n");

	if(pInfo->fd!=-1)
	{
		TimerCancel(priv->timer);
		xf86RemoveEnabledDevice(pInfo);
		SynapticsReset(priv);

		if(priv->proto_ops->DeviceOffHook&&!priv->proto_ops->DeviceOffHook(pInfo))
			rc=!Success;
		if(priv->comm.buffer)
		{
			XisbFree(priv->comm.buffer);
			priv->comm.buffer= NULL;
		}
		SynapticsCloseFd(pInfo);
	}
	dev->public.on= FALSE;
	return rc;
}

static int DeviceClose(DeviceIntPtr dev)
{
	Bool RetValue;
	InputInfoPtr pInfo=dev->public.devicePrivate;
	SynapticsPrivate *priv=(SynapticsPrivate *) pInfo->private;

	RetValue=DeviceOff(dev);
	TimerFree(priv->timer);
	priv->timer= NULL;
	SynapticsHwStateFree(&priv->hwState);
	SynapticsHwStateFree(&priv->local_hw_state);
	SynapticsHwStateFree(&priv->comm.hwState);
	return RetValue;
}

static void InitAxesLabels(Atom *labels, int nlabels, const SynapticsPrivate * priv)
{
	int i;

	memset(labels, 0, nlabels*sizeof(Atom));
	switch(nlabels)
	{
		default:
		case 4:
			labels[3]=XIGetKnownProperty(AXIS_LABEL_PROP_REL_VSCROLL);
		case 3:
			labels[2]=XIGetKnownProperty(AXIS_LABEL_PROP_REL_HSCROLL);
		case 2:
			labels[1]=XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);
		case 1:
			labels[0]=XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
			break;
	}

}

static void InitButtonLabels(Atom *labels, int nlabels)
{
	memset(labels, 0, nlabels*sizeof(Atom));
	switch(nlabels)
	{
		default:
		case 7:
			labels[6]=XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
		case 6:
			labels[5]=XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
		case 5:
			labels[4]=XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
		case 4:
			labels[3]=XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
		case 3:
			labels[2]=XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
		case 2:
			labels[1]=XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
		case 1:
			labels[0]=XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
			break;
	}
}

static void DeviceInitTouch(DeviceIntPtr dev, Atom *axes_labels)
{
	InputInfoPtr pInfo=dev->public.devicePrivate;
	SynapticsPrivate *priv=(SynapticsPrivate *) (pInfo->private);

}

static int DeviceInit(DeviceIntPtr dev)
{
	InputInfoPtr pInfo=dev->public.devicePrivate;
	SynapticsPrivate *priv=(SynapticsPrivate *) (pInfo->private);
	Atom prop;
	float tmpf;
	unsigned char map[SYN_MAX_BUTTONS+1];
	int i;
	int min, max;
	int num_axes=2;
	Atom btn_labels[SYN_MAX_BUTTONS]=
	{0};
	Atom *axes_labels;
	DeviceVelocityPtr pVel;

	num_axes+=2;

	axes_labels=calloc(num_axes, sizeof(Atom));
	if(!axes_labels)
	{
		xf86IDrvMsg(pInfo, X_ERROR, "failed to allocate axis labels\n");
		return !Success;
	}

	InitAxesLabels(axes_labels, num_axes, priv);
	InitButtonLabels(btn_labels, SYN_MAX_BUTTONS);

	DBG(3, "Synaptics DeviceInit called\n");

	for(i=0; i<=SYN_MAX_BUTTONS; i++)
		map[i]=i;

	dev->public.on= FALSE;

	InitPointerDeviceStruct((DevicePtr) dev, map,
	SYN_MAX_BUTTONS, btn_labels, SynapticsCtrl, GetMotionHistorySize(), num_axes, axes_labels);

	/*
	 * setup dix acceleration to match legacy synaptics settings, and
	 * etablish a device-specific profile to do stuff like pressure-related
	 * acceleration.
	 */
//    if (NULL != (pVel = GetDevicePredictableAccelData(dev))) {
//        SetDeviceSpecificAccelerationProfile(pVel,
//                                             SynapticsAccelerationProfile);
	/* float property type */
        Atom float_type = XIGetKnownProperty(XATOM_FLOAT);
	/* translate MinAcc to constant deceleration.
	 * May be overridden in xf86InitValuatorDefaults */
//        priv->synpara.min_speed=1/2.5;
	tmpf=1;

//        tmpf=2.5;////////////

//        xf86IDrvMsg(pInfo, X_CONFIG,
//                    "(accel) MinSpeed is now constant deceleration " "%.1f\n",
//                    tmpf);
        prop = XIGetKnownProperty(ACCEL_PROP_CONSTANT_DECELERATION);
        XIChangeDeviceProperty(dev, prop, float_type, 32,
                               PropModeReplace, 1, &tmpf, FALSE);

	/* adjust accordingly */
//        priv->synpara.max_speed /= priv->synpara.min_speed;
//        priv->synpara.min_speed = 1.0;
	/* synaptics seems to report 80 packet/s, but dix scales for
	 * 100 packet/s by default. */
//        pVel->corr_mul = 12.5f; /*1000[ms]/80[/s] = 12.5 */
//priv->synpara.max_speed=1.75;///////////////
//        xf86IDrvMsg(pInfo, X_CONFIG, "(accel) MaxSpeed is now %.2f\n",
//                    priv->synpara.max_speed);
//        priv->synpara.accl=0.085;
//        xf86IDrvMsg(pInfo, X_CONFIG, "(accel) AccelFactor is now %.3f\n",
//                    priv->synpara.accl);
	pVel = GetDevicePredictableAccelData(dev);
	SetDeviceSpecificAccelerationProfile(pVel, SynapticsAccelerationProfile);
    /* synaptics seems to report 80 packet/s, but dix scales for
     * 100 packet/s by default. */
    pVel->corr_mul = 12.5f; /*1000[ms]/80[/s] = 12.5 */
	SetScrollValuator(dev, 2, SCROLL_TYPE_HORIZONTAL,
	HORIZSCROLLDELTA, 0);
	SetScrollValuator(dev, 3, SCROLL_TYPE_VERTICAL,
	VERTSCROLLDELTA, 0);
	prop=XIGetKnownProperty(ACCEL_PROP_PROFILE_NUMBER);
	i= AccelProfileDeviceSpecific;
	XIChangeDeviceProperty(dev, prop, XA_INTEGER, 32, PropModeReplace, 1, &i, FALSE);
//    }

	free(axes_labels);

	priv->hwState=SynapticsHwStateAlloc(priv);
	if(!priv->hwState)
		goto fail;

	priv->local_hw_state=SynapticsHwStateAlloc(priv);
	if(!priv->local_hw_state)
		goto fail;

	priv->comm.hwState=SynapticsHwStateAlloc(priv);

	InitDeviceProperties(pInfo);
	XIRegisterPropertyHandler(pInfo->dev, SetProperty, NULL, NULL);

	SynapticsReset(priv);

	return Success;

	fail: free(priv->local_hw_state);
	free(priv->hwState);
	return !Success;
}

static CARD32 timerFunc(OsTimerPtr timer, CARD32 now, pointer arg)
{
	InputInfoPtr pInfo=arg;
	SynapticsPrivate *priv=(SynapticsPrivate *) (pInfo->private);
	struct SynapticsHwState *hw=priv->local_hw_state;
	int delay;
#if !HAVE_THREADED_INPUT
	int sigstate = xf86BlockSIGIO();
#else
	input_lock();
#endif

	priv->hwState->millis+=now-priv->timer_time;
	SynapticsCopyHwState(hw, priv->hwState);
	//TODO DYS
//    SynapticsResetTouchHwState(hw, FALSE);
	delay=HandleState(pInfo, hw, hw->millis, TRUE);

	priv->timer_time=now;
	priv->timer=TimerSet(priv->timer, 0, delay, timerFunc, pInfo);

#if !HAVE_THREADED_INPUT
	xf86UnblockSIGIO(sigstate);
#else
	input_unlock();
#endif

	return 0;
}

static Bool SynapticsGetHwState(InputInfoPtr pInfo, SynapticsPrivate * priv, struct SynapticsHwState *hw)
{
	return priv->proto_ops->ReadHwState(pInfo, &priv->comm, hw);
}

/*
 *  called for each full received packet from the touchpad
 */
static void ReadInput(InputInfoPtr pInfo)
{
	SynapticsPrivate *priv=(SynapticsPrivate *) (pInfo->private);
	struct SynapticsHwState *hw=priv->local_hw_state;
	int delay=0;
	Bool newDelay= FALSE;

	while(SynapticsGetHwState(pInfo, priv, hw))
	{

		/* timer may cause actual events to lag behind (#48777) */
		if(priv->hwState->millis>hw->millis)
			hw->millis=priv->hwState->millis;

		SynapticsCopyHwState(priv->hwState, hw);
		delay=HandleState(pInfo, hw, hw->millis, FALSE);
		newDelay= TRUE;
	}

	if(newDelay)
	{
		priv->timer_time=GetTimeInMillis();
		priv->timer=TimerSet(priv->timer, 0, delay, timerFunc, pInfo);
	}
}

/*
 * React on changes in the hardware state. This function is called every time
 * the hardware state changes. The return value is used to specify how many
 * milliseconds to wait before calling the function again if no state change
 * occurs.
 *
 * from_timer denotes if HandleState was triggered from a timer (e.g. to
 * generate fake motion events, or for the tap-to-click state machine), rather
 * than from having received a motion event.
 */
#define BUTTONS_Y MAXY*0.75
#define MIDDLEBTN_TOP MAXY*0.7
#define MIDDLEBTN_BOTTOM MAXY*0.85
#define MIDDLEBTN_RADIUS 100
#define VERTSCROLL_WIDTH 100
#define HORIZSCROLL_WIDTH 100
#define MODIFIER_RADIUS 200

static enum SynapticsRegion getRegionAt(int x, int y)
{
	if(x<0||y<0)
	{
		return RG_NULL;
	}
	//if((x>MAXX-VERTSCROLL_WIDTH||x<VERTSCROLL_WIDTH)&&y<HORIZSCROLL_WIDTH)
	if(SQR(x)+SQR(y)<SQR(MODIFIER_RADIUS))
	{
		return RG_MODIFIER;
	}
	if(x>MAXX-VERTSCROLL_WIDTH)
	{
		return RG_VERTSCROLL;
	}
	if(y<HORIZSCROLL_WIDTH)
	{
		return RG_HORIZSCROLL;
	}
	if(y>=MIDDLEBTN_TOP&&y<=MIDDLEBTN_BOTTOM&&x>MAXX/2-MIDDLEBTN_RADIUS&&x<=MAXX/2+MIDDLEBTN_RADIUS)
	{
		return RG_MIDDLEBTN;
	}
	if(y>BUTTONS_Y)
	{
		if(x>MAXX/2)
		{
			return RG_RIGHTBTN;
		}
		else
		{
			return RG_LEFTBTN;
		}
	}
	return RG_MOVE;
}
static int HandleState(InputInfoPtr pInfo, struct SynapticsHwState *hw, CARD32 now, Bool from_timer)
{
	SynapticsPrivate *priv=(SynapticsPrivate *) (pInfo->private);
	SynapticsParameters *para=&priv->synpara;
	int delay=100000;
	int timeleft;
	double dx=0, dy=0, scH=0, scV=0;
	Bool inside_active_area;

	//Finger mode processing
	for(int f=0; f<5; f++)
	{
		enum SynapticsRegion region=getRegionAt(hw->x[f], hw->y[f]);
		switch(priv->fingerModes[f])
		{
			case FM_NULL:
					switch(region)
					{
						case RG_MOVE:
							priv->fingerModes[f]=FM_MOVE;
							priv->lastX[f]=hw->x[f];
							priv->lastY[f]=hw->y[f];
							break;
						case RG_VERTSCROLL:
							priv->fingerModes[f]=FM_VERTSCROLL;
							priv->lastX[f]=hw->x[f];
							priv->lastY[f]=hw->y[f];
							break;
						case RG_HORIZSCROLL:
							priv->fingerModes[f]=FM_HORIZSCROLL;
							priv->lastX[f]=hw->x[f];
							priv->lastY[f]=hw->y[f];
							break;
						case RG_MODIFIER:
							priv->fingerModes[f]=FM_MODIFIER;
							break;
						case RG_LEFTBTN:
							priv->fingerModes[f]=FM_LEFTBTN;
							break;
						case RG_RIGHTBTN:
							priv->fingerModes[f]=FM_RIGHTBTN;
							break;
						case RG_MIDDLEBTN:
							priv->fingerModes[f]=FM_MIDDLEBTN;
							break;
					}
				break;
			case FM_MOVE:
				if(region==RG_NULL)
				{
					priv->fingerModes[f]=FM_NULL;
				}
				break;
			case FM_VERTSCROLL:
				if(region==RG_NULL)
				{
					priv->fingerModes[f]=FM_NULL;
				}
				break;
			case FM_HORIZSCROLL:
				if(region==RG_NULL)
				{
					priv->fingerModes[f]=FM_NULL;
				}
				break;

			case FM_RIGHTBTN:
				if(region==RG_NULL)
				{
					priv->fingerModes[f]=FM_NULL;
				}
				else if(region==RG_LEFTBTN)
				{
					priv->fingerModes[f]=FM_LEFTBTN;
				}
				else if(region==RG_MIDDLEBTN)
				{
					priv->fingerModes[f]=FM_MIDDLEBTN;
				}
				break;
			case FM_LEFTBTN:
				if(region==RG_NULL)
				{
					priv->fingerModes[f]=FM_NULL;
				}
				else if(region==RG_RIGHTBTN)
				{
					priv->fingerModes[f]=FM_RIGHTBTN;
				}
				else if(region==RG_MIDDLEBTN)
				{
					priv->fingerModes[f]=FM_MIDDLEBTN;
				}
				break;
			case FM_MIDDLEBTN:
				if(region==RG_NULL)
				{
					priv->fingerModes[f]=FM_NULL;
				}
				else if(region==RG_LEFTBTN)
				{
					priv->fingerModes[f]=FM_LEFTBTN;
				}
				else if(region==RG_RIGHTBTN)
				{
					priv->fingerModes[f]=FM_RIGHTBTN;
				}
				break;
			case FM_MODIFIER:
				if(region==RG_NULL)
				{
					priv->fingerModes[f]=FM_NULL;
				}
				break;
		}
	}

//	xf86DrvMsg(pInfo, X_PROBED, "X=%d; Y=%d; REGION: %d\n", hw->x[0], hw->y[0], priv->fingerModes[0]);
//    xf86DrvMsg(pInfo, X_PROBED, "FINGEsRS = %d%d%d%d%d\n",hw->finger[0],hw->finger[1],hw->finger[2],hw->finger[3],hw->finger[4]);
//	xf86DrvMsg(pInfo, X_PROBED, "Finger #0 (%d,%d) in region %d, active=%d, mode=%d\n", hw->x[0], hw->y[0], getRegionAt(hw->x[0], hw->y[0]),hw->finger[0],priv->fingerModes[0]);
//    xf86DrvMsg(pInfo, X_PROBED, "Finger modes: %d %d %d %d %d\n",priv->fingerModes[0],priv->fingerModes[1],priv->fingerModes[2],priv->fingerModes[3],priv->fingerModes[4]);
//	for(int i = 0; i<5;i++)
//	{
//		if(hw->finger[i])
//		{
//				xf86DrvMsg(pInfo, X_PROBED, "Finger #%d: X=%d; Y=%d; MODE: %d\n",i, hw->x[i], hw->y[i], priv->fingerModes[i]);
//		}
//	}

//Movement processing
	Bool mod = FALSE;
	for(int finger=0; finger<5; finger++){
		if(priv->fingerModes[finger]==FM_MODIFIER)
			mod=TRUE;
	}
	int movingFingers = 0;
	double factor=1;
	for(int f=0; f<5; f++)
	{
		switch(priv->fingerModes[f])
		{
			case FM_MOVE:
				dx+=hw->x[f]-priv->lastX[f];
				dy+=hw->y[f]-priv->lastY[f];
				movingFingers++;
				break;
			case FM_VERTSCROLL:
				scV+=hw->y[f]-priv->lastY[f];
				break;
			case FM_HORIZSCROLL:
				scH+=hw->x[f]-priv->lastX[f];
				break;
		}
	}
	if(movingFingers==3){
//		factor=1.0/8;
	}
	if(mod){
		factor=0.5;
	}
	
	dx*=factor;
	dy*=factor;
	scH*=factor;
	scV*=factor;

	dx+=priv->fracX;
	dy+=priv->fracY;
	int outX=floor(dx);
	int outY=floor(dy);
	priv->fracX=dx-outX;
	priv->fracY=dy-outY;
	

//	xf86DrvMsg(pInfo, X_PROBED, "DX=%d;DY=%d\n", dx,dy);
	if(dx!=0||dy!=0)
		xf86PostMotionEvent(pInfo->dev, 0, 0, 2, outX, outY);
	if(scH!=0)
		xf86PostMotionEvent(pInfo->dev, 0, 2, 1, (int)scH);
	if(scV!=0)
		xf86PostMotionEvent(pInfo->dev, 0, 3, 1, (int)scV);
	//Save values
	for(int f=0; f<5; f++)
	{
		priv->lastX[f]=hw->x[f];
		priv->lastY[f]=hw->y[f];
	}

	//Button processing
	if(priv->ongoingBtnPress)
	{
		if(!hw->pressed)
		{
			xf86PostButtonEvent(pInfo->dev, FALSE, priv->OngoingBtnId, FALSE, 0, 0);
			priv->ongoingBtnPress= FALSE;
		}
	}
	else if(hw->pressed)
	{
		Bool LB= FALSE, MB= FALSE, RB= FALSE;
		for(int finger=0; finger<5; finger++)
		{
			switch(priv->fingerModes[finger])
			{
				case FM_LEFTBTN:
					LB= TRUE;
					break;
				case FM_MIDDLEBTN:
					MB= TRUE;
					break;
				case FM_RIGHTBTN:
					RB= TRUE;
					break;
			}
		}
		if((LB&&!MB&&!RB)||
				(!LB&&!MB&&!RB))
		{
			xf86PostButtonEvent(pInfo->dev, FALSE, 1, TRUE, 0, 0);
			priv->ongoingBtnPress= TRUE;
			priv->OngoingBtnId=1;
		}
		else if((!LB&&MB&&!RB))
		{
			xf86PostButtonEvent(pInfo->dev, FALSE, 2, TRUE, 0, 0);
			priv->ongoingBtnPress= TRUE;
			priv->OngoingBtnId=2;
		}
		else if((!LB&&!MB&&RB))
		{
			xf86PostButtonEvent(pInfo->dev, FALSE, 3, TRUE, 0, 0);
			priv->ongoingBtnPress= TRUE;
			priv->OngoingBtnId=3;
		}

	}
//	xf86DrvMsg(NULL, X_PROBED, "XDDDDDD %d\n",para->press_motion_min_z);
	return delay;
}

static int ControlProc(InputInfoPtr pInfo, xDeviceCtl * control)
{
	DBG(3, "Control Proc called\n");
	return Success;
}

static int SwitchMode(ClientPtr client, DeviceIntPtr dev, int mode)
{
	DBG(3, "SwitchMode called\n");

	return XI_BadMode;
}

static void ReadDevDimensions(InputInfoPtr pInfo)
{
	SynapticsPrivate *priv=(SynapticsPrivate *) pInfo->private;

	if(priv->proto_ops->ReadDevDimensions)
		priv->proto_ops->ReadDevDimensions(pInfo);

	SanitizeDimensions(pInfo);
}

static Bool QueryHardware(InputInfoPtr pInfo)
{
	SynapticsPrivate *priv=(SynapticsPrivate *) pInfo->private;

	priv->comm.protoBufTail=0;

	if(!priv->proto_ops->QueryHardware(pInfo))
	{
		xf86IDrvMsg(pInfo, X_PROBED, "no supported touchpad found\n");
		if(priv->proto_ops->DeviceOffHook)
			priv->proto_ops->DeviceOffHook(pInfo);
		return FALSE;
	}

	return TRUE;
}

