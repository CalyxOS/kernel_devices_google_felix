/*
  * fts.c
  *
  * FTS Capacitive touch screen controller (FingerTipS)
  *
  * Copyright (C) 2016, STMicroelectronics Limited.
  * Authors: AMG(Analog Mems Group)
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License version 2 as
  * published by the Free Software Foundation.
  *
  * THE PRESENT SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES
  * OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, FOR THE SOLE
  * PURPOSE TO SUPPORT YOUR APPLICATION DEVELOPMENT.
  * AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
  * INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM
  * THE
  * CONTENT OF SUCH SOFTWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
  * INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * THIS SOFTWARE IS SPECIFICALLY DESIGNED FOR EXCLUSIVE USE WITH ST PARTS.
  */


/*!
  * \file fts.c
  * \brief It is the main file which contains all the most important functions
  * generally used by a device driver the driver
  */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/spi/spi.h>

#if !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
#include <drm/drm_panel.h>
#endif

#include "fts.h"
#include "fts_lib/fts_flash.h"
#include "fts_lib/fts_test.h"
#include "fts_lib/fts_error.h"

static int system_reseted_up;
static int system_reseted_down;
#ifdef CONFIG_PM
static const struct dev_pm_ops fts_pm_ops;
#endif

char fts_ts_phys[64];
extern struct test_to_do tests;

#define event_id(_e)		(EVT_ID_##_e >> 4)
#define handler_name(_h)	fts_##_h##_event_handler
#define install_handler(_i, _evt, _hnd) \
		(_i->event_dispatch_table[event_id(_evt)] = handler_name(_hnd))


#ifdef KERNEL_ABOVE_2_6_38
#define TYPE_B_PROTOCOL
#endif

static char *event_type_str[EVT_TYPE_STATUS_MAX_NUM] = {
	"",				// 0x00
	"Echo event of command",	// 0x01
	"GPIO Charger Detect",		// 0x02
	"Frame Drop",			// 0x03
	"",				// 0x04
	"Force Cal",			// 0x05
	"Water Mode",			// 0x06
	"SS Raw Saturated",		// 0x07
	"Previous Water",		// 0x08
	"Noise Status Event",		// 0x09
	"Stimpad Status Event",		// 0x0A
	"No Touch Status Event",	// 0x0B
	"Idle Status Event",		// 0x0C
	"Palm touch status",		// 0x0D
	"Grip touch status",		// 0x0E
	"Golden Raw Validation",	// 0x0F
	"",				// 0x10
	"",				// 0x11
	"",				// 0x12
	"",				// 0x13
	"",				// 0x14
	"",				// 0x15
	"Golden Raw Data Abnormal"	// 0x16
};

static void fts_pinctrl_setup(struct fts_ts_info *info, bool active);


/**
  * Set the value of system_reseted_up flag
  * @param val value to write in the flag
  */
void set_system_reseted_up(int val)
{
	system_reseted_up = val;
}

/**
  * Return the value of system_resetted_down.
  * @return the flag value: 0 if not set, 1 if set
  */
int is_system_resetted_down(void)
{
	return system_reseted_down;
}

/**
  * Return the value of system_resetted_up.
  * @return the flag value: 0 if not set, 1 if set
  */
int is_system_resetted_up(void)
{
	return system_reseted_up;
}

/**
  * Set the value of system_reseted_down flag
  * @param val value to write in the flag
  */
void set_system_reseted_down(int val)
{
	system_reseted_down = val;
}

/* Set the interrupt state
 * @param enable Indicates whether interrupts should enabled.
 * @return OK if success
 */
int fts_set_interrupt(struct fts_ts_info *info, bool enable)
{
	if (info->client == NULL) {
		dev_err(info->dev, "Cannot get client irq. Error = %08X\n",
			ERROR_OP_NOT_ALLOW);
		return ERROR_OP_NOT_ALLOW;
	}

	mutex_lock(&info->fts_int_mutex);
	if (enable == info->irq_enabled) {
		dev_dbg(info->dev, "Interrupt is already set (enable = %d).\n", enable);
	} else {
		info->irq_enabled = enable;
		if (enable) {
			enable_irq(info->client->irq);
			dev_dbg(info->dev, "Interrupt enabled.\n");
		} else {
			disable_irq_nosync(info->client->irq);
			dev_dbg(info->dev, "Interrupt disabled.\n");
		}
	}

	mutex_unlock(&info->fts_int_mutex);
	return OK;
}
/**
  * Release all the touches in the linux input subsystem
  * @param info pointer to fts_ts_info which contains info about the device and
  * its hw setup
  */
void release_all_touches(struct fts_ts_info *info)
{
	unsigned int type = MT_TOOL_FINGER;
	int i;
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_input_lock(info->gti);
	goog_input_set_timestamp(info->gti, info->input_dev, KTIME_RELEASE_ALL);
	for (i = 0; i < TOUCH_ID_MAX + PEN_ID_MAX ; i++) {
		type = i < TOUCH_ID_MAX ? MT_TOOL_FINGER : MT_TOOL_PEN;

		if (type == MT_TOOL_FINGER) {
			goog_input_mt_slot(info->gti, info->input_dev, i);
			goog_input_report_abs(info->gti, info->input_dev, ABS_MT_PRESSURE, 0);
			goog_input_mt_report_slot_state(info->gti, info->input_dev, type, 0);
			goog_input_report_abs(info->gti, info->input_dev, ABS_MT_TRACKING_ID, -1);
		} else {
			input_mt_slot(info->input_dev, i);
			input_report_abs(info->input_dev, ABS_MT_PRESSURE, 0);
			input_mt_report_slot_state(info->input_dev, type, 0);
			input_report_abs(info->input_dev, ABS_MT_TRACKING_ID, -1);
		}
	}
	goog_input_report_key(info->gti, info->input_dev, BTN_TOUCH, 0);
	goog_input_sync(info->gti, info->input_dev);

	goog_input_unlock(info->gti);
#else
	mutex_lock(&info->input_report_mutex);
	for (i = 0; i < TOUCH_ID_MAX + PEN_ID_MAX ; i++) {
		type = i < TOUCH_ID_MAX ? MT_TOOL_FINGER : MT_TOOL_PEN;
		input_mt_slot(info->input_dev, i);
		input_report_abs(info->input_dev, ABS_MT_PRESSURE, 0);
		input_mt_report_slot_state(info->input_dev, type, 0);
		input_report_abs(info->input_dev, ABS_MT_TRACKING_ID, -1);
	}
	input_report_key(info->input_dev, BTN_TOUCH, 0);
	input_sync(info->input_dev);
	mutex_unlock(&info->input_report_mutex);
#endif
	info->touch_id = 0;
}


/**
  * The function handle the switching of the mode in the IC enabling/disabling
  * the sensing and the features set from the host
  * @param info pointer to fts_ts_info which contains info about the device and
  * its hw setup
  * @param force if 1, the enabling/disabling command will be send even
  * if the feature was already enabled/disabled otherwise it will judge if
  * the feature changed status or the IC had a system reset
  * @return OK if success or an error code which specify the type of error
  *encountered
  */
static int fts_mode_handler(struct fts_ts_info *info, int force)
{
	int res = OK;
	u8 data = 0;

	/* disable irq wake because resuming from gesture mode */
	if ((info->mode == SCAN_MODE_LOW_POWER) && (info->resume_bit == 1))
		disable_irq_wake(info->client->irq);

	info->mode = SCAN_MODE_HIBERNATE;
	log_info(1, "%s: Mode Handler starting...\n", __func__);
	switch (info->resume_bit) {
	case 0:	/* screen down */
		log_info(1, "%s: Screen OFF...\n", __func__);
		/* do sense off in order to avoid the flooding of the fifo with
		 * touch events if someone is touching the panel during suspend
		 */
		data = SCAN_MODE_HIBERNATE;
		res = fts_write_fw_reg(SCAN_MODE_ADDR, &data, 1);
		if (res == OK)
			info->mode = SCAN_MODE_HIBERNATE;
		set_system_reseted_down(0);
		break;

	case 1:	/* screen up */
		log_info(1, "%s: Screen ON...\n", __func__);
		data = SCAN_MODE_ACTIVE;
		res = fts_write_fw_reg(SCAN_MODE_ADDR, &data, 1);
		if (res == OK)
			info->mode = SCAN_MODE_ACTIVE;
		set_system_reseted_up(0);
		break;

	default:
		log_info(1,
			 "%s: invalid resume_bit value = %d! ERROR %08X\n",
			 __func__, info->resume_bit, ERROR_OP_NOT_ALLOW);
		res = ERROR_OP_NOT_ALLOW;
	}
	/*TODO : For all the gesture related modes */

	log_info(1, "%s: Mode Handler finished! res = %08X mode = %08X\n",
		 __func__, res, info->mode);
	return res;
}

/**
  * Bottom Half Interrupt Handler function
  * This handler is called each time there is at least one new event in the FIFO
  * and the interrupt pin of the IC goes low. It will read all the events from
  * the FIFO and dispatch them to the proper event handler according the event
  * ID
  */
static irqreturn_t fts_interrupt_handler(int irq, void *handle)
{
	struct fts_ts_info *info = handle;
	int error = 0, count = 0;
	unsigned char event_id;
	unsigned char total_events = 0;
	unsigned char *evt_data;
	bool has_pointer_event = false;
	int event_start_idx = -1;

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	if (goog_pm_wake_lock(info->gti, GTI_PM_WAKELOCK_TYPE_IRQ, true) < 0) {
		dev_warn(info->dev, "Touch device already suspended.\n");
		return IRQ_HANDLED;
	}
#endif
	memset(info->evt_data, 0, EVENT_DATA_SIZE);
	for (count = 0; count < MAX_FIFO_EVENT; count++) {
		error = fts_read_fw_reg(FIFO_READ_ADDR,
			&info->evt_data[count * FIFO_EVENT_SIZE], FIFO_EVENT_SIZE);
		if (error != OK) {
			log_info(1, "%s: Failed to read fifo event (error=%d)", __func__, error);
			break;
		}

		if (info->evt_data[count * FIFO_EVENT_SIZE] == EVT_ID_NOEVENT)
			break;

		total_events++;
	}
	evt_data = &info->evt_data[0];
	if (evt_data[0] == EVT_ID_NOEVENT)
		goto exit;
	if (total_events == MAX_FIFO_EVENT)
		log_info(1, "%s: Warnning:  total_events = MAX_FIFO_EVENT(%d)",
				__func__, MAX_FIFO_EVENT);
	/*
	 * Parsing all the events ID and specifically handle the
	 * EVT_ID_CONTROLLER_READY and EVT_ID_ERROR at first.
	 */
	for (count = 0; count < total_events; count++) {
		evt_data = &info->evt_data[count * FIFO_EVENT_SIZE];
		switch (evt_data[0]) {
		case EVT_ID_CONTROLLER_READY:
		case EVT_ID_ERROR:
			event_id = evt_data[0] >> 4;
			/* Ensure event ID is within bounds */
			if (event_id < NUM_EVT_ID)
				info->event_dispatch_table[event_id](info, (evt_data));

			has_pointer_event = false;
			event_start_idx = count;
			break;
		case EVT_ID_ENTER_POINT:
		case EVT_ID_MOTION_POINT:
		case EVT_ID_LEAVE_POINT:
			has_pointer_event = true;
			break;
		default:
			break;
		}
	}
	/* Only lock input report when there is pointer event. */
	if (has_pointer_event) {
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
		goog_input_lock(info->gti);
		goog_input_set_timestamp(info->gti, info->input_dev, info->timestamp);
#else
		mutex_lock(&info->input_report_mutex);
		input_set_timestamp(info->input_dev, info->timestamp);
#endif
	}

	/*
	 * Handle the remaining events except for
	 * EVT_ID_CONTROLLER_READY and EVT_ID_ERROR.
	 */
	for (count = max(event_start_idx + 1, 0); count < total_events; count++) {
		evt_data = &info->evt_data[count * FIFO_EVENT_SIZE];
		event_id = evt_data[0] >> 4;

		/* Ensure event ID is within bounds */
		if (event_id < NUM_EVT_ID)
			info->event_dispatch_table[event_id](info, (evt_data));
	}

	if (has_pointer_event) {
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
		if (info->touch_id == 0)
			goog_input_report_key(info->gti, info->input_dev, BTN_TOUCH, 0);

		goog_input_sync(info->gti, info->input_dev);
		goog_input_unlock(info->gti);
#else
		if (info->touch_id == 0)
			input_report_key(info->input_dev, BTN_TOUCH, 0);

		input_sync(info->input_dev);
		mutex_unlock(&info->input_report_mutex);
#endif
	}
exit:
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_pm_wake_unlock(info->gti, GTI_PM_WAKELOCK_TYPE_IRQ);
#endif
	return IRQ_HANDLED;
}

/**
  * Top half Interrupt handler function
  * Respond to the interrupt and schedule the bottom half interrupt handler
  * in its work queue
  * @see fts_event_handler()
  */
static irqreturn_t fts_isr(int irq, void *handle)
{
	struct fts_ts_info *info = handle;
	info->timestamp = ktime_get();
	return IRQ_WAKE_THREAD;
}

/**
  * Event Handler for no events (EVT_ID_NOEVENT)
  */
static void fts_nop_event_handler(struct fts_ts_info *info,
					unsigned char *event)
{
	log_info(1,
		 "%s: Doing nothing for event = %02X %02X %02X %02X %02X %02X %02X %02X\n",
		 __func__, event[0], event[1], event[2], event[3],
		 event[4], event[5], event[6], event[7]);
}

/**
  * Event handler for enter and motion events (EVT_ID_ENTER_POINT,
  * EVT_ID_MOTION_POINT )
  * report to the linux input system touches with their coordinated and
  * additional informations
  */
static void fts_enter_pointer_event_handler(struct fts_ts_info *info, unsigned
					    char *event)
{
	unsigned char touch_id;
	unsigned int touch_condition = 1, tool = MT_TOOL_FINGER;
	int x, y, z, distance, major, minor;
	u8 touch_type;

	if (!info->resume_bit)
		goto no_report;

	touch_type = event[1] & 0x0F;
	touch_id = (event[1] & 0xF0) >> 4;

	x = (((int)event[3] & 0x0F) << 8) | (event[2]);
	y = ((int)event[4] << 4) | ((event[3] & 0xF0) >> 4);
	z = (int)(event[5]);
	distance = 0;	/* if the tool is touching the display the distance
			 * should be 0 */
	major = (int)(event[6]);
	minor = (int)(event[7]);
	if (x == X_AXIS_MAX)
		x--;

	if (y == Y_AXIS_MAX)
		y--;
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_input_mt_slot(info->gti, info->input_dev, touch_id);
#else
	input_mt_slot(info->input_dev, touch_id);
#endif
	switch (touch_type) {
	/* TODO: customer can implement a different strategy for each kind of
	 * touch */
	case TOUCH_TYPE_FINGER:
	case TOUCH_TYPE_GLOVE:
	case TOUCH_TYPE_LARGE:
		tool = MT_TOOL_FINGER;
		touch_condition = 1;
		__set_bit(touch_id, &info->touch_id);
		break;


	case TOUCH_TYPE_FINGER_HOVER:
		tool = MT_TOOL_FINGER;
		touch_condition = 0;	/* need to hover */
		z = 0;	/* no pressure */
		__set_bit(touch_id, &info->touch_id);
		distance = DISTANCE_MAX;	/* check with fw report the
						 * hovering distance */
		break;

	default:
		log_info(1, "%s: Invalid touch type = %d ! No Report...\n",
			  __func__, touch_type);
		goto no_report;
	}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_input_report_key(info->gti, info->input_dev, BTN_TOUCH, touch_condition);
	goog_input_mt_report_slot_state(info->gti, info->input_dev, tool, 1);
	goog_input_report_abs(info->gti, info->input_dev, ABS_MT_POSITION_X, x);
	goog_input_report_abs(info->gti, info->input_dev, ABS_MT_POSITION_Y, y);
	goog_input_report_abs(info->gti, info->input_dev, ABS_MT_TOUCH_MAJOR, major);
	goog_input_report_abs(info->gti, info->input_dev, ABS_MT_TOUCH_MINOR, minor);
	goog_input_report_abs(info->gti, info->input_dev, ABS_MT_PRESSURE, z);
	goog_input_report_abs(info->gti, info->input_dev, ABS_MT_DISTANCE, distance);
#else
	input_report_key(info->input_dev, BTN_TOUCH, touch_condition);
	input_mt_report_slot_state(info->input_dev, tool, 1);
	input_report_abs(info->input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(info->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(info->input_dev, ABS_MT_TOUCH_MAJOR, major);
	input_report_abs(info->input_dev, ABS_MT_TOUCH_MINOR, minor);
	input_report_abs(info->input_dev, ABS_MT_PRESSURE, z);
	input_report_abs(info->input_dev, ABS_MT_DISTANCE, distance);
#endif

no_report:
	return;
}

/**
  * Event handler for leave event (EVT_ID_LEAVE_POINT )
  * Report to the linux input system that one touch left the display
  */
static void fts_leave_pointer_event_handler(struct fts_ts_info *info, unsigned
					    char *event)
{
	unsigned char touch_id;
	unsigned int tool = MT_TOOL_FINGER;
	u8 touch_type;

	touch_type = event[1] & 0x0F;
	touch_id = (event[1] & 0xF0) >> 4;

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_input_mt_slot(info->gti, info->input_dev, touch_id);
#else
	input_mt_slot(info->input_dev, touch_id);
#endif
	switch (touch_type) {
	case TOUCH_TYPE_FINGER:
	case TOUCH_TYPE_GLOVE:
	case TOUCH_TYPE_LARGE:
	case TOUCH_TYPE_FINGER_HOVER:
		tool = MT_TOOL_FINGER;
		__clear_bit(touch_id, &info->touch_id);
		break;
	default:
		log_info(1, "%s: Invalid touch type = %d ! No Report...\n",
			 __func__, touch_type);
		return;
	}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	goog_input_report_abs(info->gti, info->input_dev, ABS_MT_PRESSURE, 0);
	goog_input_mt_report_slot_state(info->gti, info->input_dev, tool, 0);
	goog_input_report_abs(info->gti, info->input_dev, ABS_MT_TRACKING_ID, -1);
#else
	input_report_abs(info->input_dev, ABS_MT_PRESSURE, 0);
	input_mt_report_slot_state(info->input_dev, tool, 0);
	input_report_abs(info->input_dev, ABS_MT_TRACKING_ID, -1);
#endif
}

#define fts_motion_pointer_event_handler fts_enter_pointer_event_handler
/*!< remap the motion event handler to the same function which handle the enter
 * event */
/**
  * Event handler for error events (EVT_ID_ERROR)
  * Handle unexpected error events implementing recovery strategy and
  * restoring the sensing status that the IC had before the error occured
  */
static void fts_error_event_handler(struct fts_ts_info *info, unsigned
				    char *event)
{
	int error = 0;

	log_info(1,
		 "%s: Received event %02X %02X %02X %02X %02X %02X %02X %02X\n",
		 __func__, event[0], event[1], event[2], event[3], event[4],
		 event[5],
		 event[6], event[7]);

	switch (event[1]) {
	case EVT_TYPE_ERROR_HARD_FAULT:
	case EVT_TYPE_ERROR_WATCHDOG:
	{
		/* before reset clear all slots */
		release_all_touches(info);
		fts_set_interrupt(info, false);
		error = fts_system_reset(1);
		error |= fts_mode_handler(info, 0);
		error |= fts_set_interrupt(info, true);
		if (error < OK)
			log_info(1,
				 "%s: Cannot reset the device ERROR %08X\n",
				 __func__, error);
	}
	break;
	}
}

/**
  * Event handler for controller ready event (EVT_ID_CONTROLLER_READY)
  * Handle controller events received after unexpected reset of the IC updating
  * the resets flag and restoring the proper sensing status
  */
static void fts_controller_ready_event_handler(struct fts_ts_info *info,
					       unsigned char *event)
{
	int error;

	log_info(1,
		"%s: controller event %02X %02X %02X %02X %02X %02X %02X %02X\n",
		 __func__, event[0], event[1], event[2], event[3], event[4],
		 event[5],
		 event[6], event[7]);
	release_all_touches(info);
	set_system_reseted_up(1);
	set_system_reseted_down(1);
	error = fts_mode_handler(info, 0);
	if (error < OK)
		log_info(1,
			 "%s: Cannot restore the device status ERROR %08X\n",
			 __func__, error);
}

#define log_status_event(force, evt_ptr) \
do { \
    u8 type = evt_ptr[1]; \
    log_info(force,"%s: %s =" \
        " %02X %02X %02X %02X %02X %02X\n", \
        __func__, event_type_str[type], \
        evt_ptr[2], evt_ptr[3], evt_ptr[4], \
        evt_ptr[5], evt_ptr[6], evt_ptr[7]); \
} while (0)

#define log_status_event2(force, sub_str, evt_ptr) \
do { \
    u8 type = evt_ptr[1]; \
    log_info(force,"%s: %s - %s =" \
        " %02X %02X %02X %02X %02X %02X\n", \
        __func__, event_type_str[type], sub_str, \
        evt_ptr[2], evt_ptr[3], evt_ptr[4], \
        evt_ptr[5], evt_ptr[6], evt_ptr[7]); \
} while (0)

/**
  * Event handler for status events (EVT_ID_STATUS_UPDATE)
  * Handle status update events
  */
static void fts_status_event_handler(struct fts_ts_info *info, u8 *event)
{
	u8 grid_touch_status;
	static u8 scanning_frequency = 0;

	switch (event[1]) {
	case EVT_TYPE_STATUS_ECHO:
		log_status_event(0, event);
		break;

	case EVT_TYPE_STATUS_GPIO_CHAR_DET:
	case EVT_TYPE_STATUS_FRAME_DROP:
	case EVT_TYPE_STATUS_NO_TOUCH:
	case EVT_TYPE_STATUS_IDLE:
	case EVT_TYPE_STATUS_GOLDEN_RAW_ERR:
		log_status_event(1, event);
		break;

	case EVT_TYPE_STATUS_FORCE_CAL:
		switch (event[2]) {
		case 0x01:
			log_status_event2(1, "sense on", event);
			break;

		case 0x02:
			log_status_event2(1, "host command", event);
			break;

		case 0x10:
			log_status_event2(1, "mutual frame drop", event);
			break;

		case 0x11:
			log_status_event2(1, "mutual pure raw", event);
			break;

		case 0x20:
			log_status_event2(1, "self detect negative", event);
			break;

		case 0x21:
			log_status_event2(1, "self touch negative", event);
			break;

		case 0x22:
			log_status_event2(1, "self detect frame flatness", event);
			break;

		case 0x23:
			log_status_event2(1, "self touch frame flatness", event);
			break;

		case 0x30:
			log_status_event2(1, "invalid mutual", event);
			break;

		case 0x31:
			log_status_event2(1, "invalid differential mutual", event);
			break;

		case 0x32:
			log_status_event2(1, "invalid Self", event);
			break;

		case 0x33:
			log_status_event2(1, "invalid self island", event);
			break;

		case 0x34:
			log_status_event2(1, "invalid Self force touch", event);
			break;

		case 0x35:
			log_status_event2(1, "mutual frame flatness", event);
			break;

		default:
			log_status_event2(1, "unknown event", event);
			break;
		}
		break;

	case EVT_TYPE_STATUS_SS_RAW_SAT:
		if (event[2] == 1)
			log_status_event2(1, "saturated", event);
		else
			log_status_event2(1, "no more saturated", event);
		break;

	case EVT_TYPE_STATUS_WATER:
		switch (event[2]) {
		case 0x00:
			log_status_event2(1, "entry by BLD with real raw frame", event);
			break;

		case 0x01:
			log_status_event2(1, "entry by BLD with rom raw frame", event);
			break;

		case 0x02:
			log_status_event2(1, "entry by MID with real raw frame", event);
			break;

		case 0x03:
			log_status_event2(1, "leave by BLD with real raw frame", event);
			break;

		case 0x04:
			log_status_event2(1, "leave by BLD with rom raw frame", event);
			break;

		case 0x05:
			log_status_event2(1, "leave by MID with real raw frame", event);
			break;

		default:
			log_status_event2(1, "unknown event", event);
			break;
		}
		break;

	case EVT_TYPE_STATUS_PRE_WAT_DET:
		if (event[2] == 1)
			log_status_event2(1, "entry", event);
		else
			log_status_event2(1, "exit", event);
		break;

	case EVT_TYPE_STATUS_NOISE:
		if(scanning_frequency != event[3]) {
			log_info(1,"%s: Scanning frequency changed from %02X to %02X\n",
				__func__, scanning_frequency, event[3]);
			scanning_frequency = event[3];
			log_status_event(1, event);
		} else
			log_status_event(0, event);
		break;

	case EVT_TYPE_STATUS_PALM_TOUCH:
		switch (event[2]) {
		case 0x01:
			log_status_event2(1, "entry", event);
			break;

		case 0x02:
			log_status_event2(1, "exit", event);
			break;

		default:
			log_status_event2(1, "unknown event", event);
			break;
		}
		break;

	case EVT_TYPE_STATUS_GRIP_TOUCH:
		grid_touch_status = (event[2] & 0xF0) >> 4;
		switch (grid_touch_status) {
		case 0x01:
			log_status_event2(1, "entry", event);
			break;

		case 0x02:
			log_status_event2(1, "exit", event);
			break;

		default:
			log_status_event2(1, "unknown event", event);
			break;
		}
		break;

	case EVT_TYPE_STATUS_GOLDEN_RAW_VAL:
		switch (event[2]) {
		case 0x01:
			log_status_event2(1, "pass", event);
			break;

		case 0x02:
			log_status_event2(1, "fail", event);
			break;

		default:
			log_status_event2(1, "unknown event", event);
			break;
		}
		break;

	default:
		log_info(1, "%s: Invalid status event (%02X) ="
			" %02X %02X %02X %02X %02X %02X\n",
			__func__, event[1], event[2], event[3],
			event[4], event[5], event[6], event[7]);
		break;
	}
}

/**
  * Event handler for enter and motion events (EVT_ID_ENTER_PEN,
  * EVT_ID_MOTION_PEN)
  * report to the linux input system pen touches with their coordinated and
  * additional informations
  */
static void fts_enter_pen_event_handler(struct fts_ts_info *info, unsigned
					    char *event)
{

	unsigned char pen_id;
	unsigned int touch_condition = 1, tool = MT_TOOL_PEN;
	int x, y, pressure, tilt_x, tilt_y;


	if (!info->resume_bit)
		goto no_report;

	pen_id = (event[0] & 0x0C) >> 2;
	pen_id = pen_id + TOUCH_ID_MAX;

	x = (((int)event[2] & 0x0F) << 8) | (event[1]);
	y = ((int)event[3] << 4) | ((event[2] & 0xF0) >> 4);
	tilt_x = (int)(event[4]);
	tilt_y = (int)(event[5]);
	pressure = (((int)event[7] & 0x0F) << 8) | (event[6]);


	input_mt_slot(info->input_dev, pen_id);
	touch_condition = 1;
	__set_bit(pen_id, &info->touch_id);


	input_report_key(info->input_dev, BTN_TOUCH, touch_condition);
	input_mt_report_slot_state(info->input_dev, tool, 1);
	input_report_abs(info->input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(info->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(info->input_dev, ABS_TILT_X, tilt_x);
	input_report_abs(info->input_dev, ABS_TILT_Y, tilt_y);
	input_report_abs(info->input_dev, ABS_MT_PRESSURE, pressure);

no_report:
	return;
}

#define fts_motion_pen_event_handler fts_enter_pen_event_handler
/*!< remap the pen motion event handler to the same function which handle the
 * enter event */


/**
  * Event handler for leave event (EVT_ID_LEAVE_PEN )
  * Report to the linux input system that pen touch left the display
  */
static void fts_leave_pen_event_handler(struct fts_ts_info *info, unsigned
					    char *event)
{

	unsigned char pen_id;
	unsigned int tool = MT_TOOL_PEN;


	pen_id = (event[0] & 0x0C) >> 2;
	pen_id = pen_id + TOUCH_ID_MAX;


	input_mt_slot(info->input_dev, pen_id);
	__clear_bit(pen_id, &info->touch_id);


	input_report_abs(info->input_dev, ABS_MT_PRESSURE, 0);
	input_mt_report_slot_state(info->input_dev, tool, 0);
	input_report_abs(info->input_dev, ABS_MT_TRACKING_ID, -1);
}

/**
  * Initialize the dispatch table with the event handlers for any possible event
  * ID
  * Set IRQ pin behavior (level triggered low)
  * Register top half interrupt handler function.
  * @see fts_interrupt_handler()
  */
static int fts_interrupt_install(struct fts_ts_info *info)
{
	int i, error = 0;

	info->event_dispatch_table = kzalloc(sizeof(event_dispatch_handler_t) *
					     NUM_EVT_ID, GFP_KERNEL);
	if (!info->event_dispatch_table) {
		log_info(1, "%s: OOM allocating event dispatch table\n",
			__func__);
		return -ENOMEM;
	}

	for (i = 0; i < NUM_EVT_ID; i++)
		info->event_dispatch_table[i] = fts_nop_event_handler;

	install_handler(info, ENTER_POINT, enter_pointer);
	install_handler(info, LEAVE_POINT, leave_pointer);
	install_handler(info, MOTION_POINT, motion_pointer);
	install_handler(info, ERROR, error);
	install_handler(info, CONTROLLER_READY, controller_ready);
	install_handler(info, STATUS_UPDATE, status);
	install_handler(info, ENTER_PEN, enter_pen);
	install_handler(info, LEAVE_PEN, leave_pen);
	install_handler(info, MOTION_PEN, motion_pen);

	/* disable interrupts in any case */
	error = fts_set_interrupt(info, false);
	if (error) return error;

	log_info(1, "%s: Interrupt Mode\n", __func__);
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	if (goog_request_threaded_irq(info->gti, info->client->irq, fts_isr,
#else
	if (request_threaded_irq(info->client->irq, fts_isr,
#endif
		fts_interrupt_handler, IRQF_ONESHOT | IRQF_TRIGGER_LOW,
		FTS_TS_DRV_NAME, info)) {
		log_info(1, "%s: Request irq failed\n", __func__);
		kfree(info->event_dispatch_table);
		error = -EBUSY;
	}
	info->irq_enabled = true;
	return error;
}

/**
  *	Clean the dispatch table and the free the IRQ.
  *	This function is called when the driver need to be removed
  */
static void fts_interrupt_uninstall(struct fts_ts_info *info) {
	fts_set_interrupt(info, false);
	kfree(info->event_dispatch_table);
	free_irq(info->client->irq, info);
}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
static int gti_default_handler(void *private_data, enum gti_cmd_type cmd_type,
	struct gti_union_cmd_data *cmd)
{
	return -ESRCH;
}
#endif

#ifdef CONFIG_PM
/**
  * Resume function which perform a system reset, clean all the touches
  *from the linux input system and prepare the ground for enabling the sensing
  */
static void fts_resume(struct fts_ts_info *info)
{
	if (!info->sensor_sleep) return;
	pr_info("%s\n", __func__);

	pm_stay_awake(info->dev);
	fts_pinctrl_setup(info, true);
	fts_system_reset(1);
	info->resume_bit = 1;
	fts_mode_handler(info, 0);
	fts_set_interrupt(info, true);
	info->sensor_sleep = false;
}

/**
  * Suspend function which clean all the touches from Linux input system
  *and prepare the ground to disabling the sensing or enter in gesture mode
  */
static void fts_suspend(struct fts_ts_info *info)
{
	if (info->sensor_sleep) return;
	pr_info("%s\n", __func__);

	info->sensor_sleep = true;
	fts_set_interrupt(info, false);
	info->resume_bit = 0;
	fts_mode_handler(info, 0);
	fts_pinctrl_setup(info, false);
	release_all_touches(info);
	pm_relax(info->dev);
}
#endif

/**
  * Complete the boot up process, initializing the sensing of the IC according
  * to the current setting chosen by the host
  * Register the notifier for the suspend/resume actions and the event handler
  * @return OK if success or an error code which specify the type of error
  */
static int fts_init_sensing(struct fts_ts_info *info)
{
	int error = 0;
	int add = 0x001C;
	uint8_t int_data = 0x01;
	int res = 0;

	error |= fts_interrupt_install(info);
	log_info(1, "%s: Sensing on..\n", __func__);
	error |= fts_mode_handler(info, 0);
	error |= fts_set_interrupt(info, true); /* enable the interrupt */

	res = fts_write_fw_reg(add, &int_data, 1);
	if (res < OK) {
		log_info(1, "%s ERROR %08X\n", __func__, res);
	}

	if (error < OK)
		log_info(1, "%s: Init error (ERROR = %08X)\n",
			 __func__, error);


	return error;
}

/**
  *	Implement the fw update and initialization flow of the IC that should be
  *executed at every boot up.
  *	The function perform a fw update of the IC in case of crc error or a new
  *fw version and then understand if the IC need to be re-initialized again.
  *	@return  OK if success or an error code which specify the type of error
  *	encountered
  */

static int fts_chip_init(struct fts_ts_info *info)
{
	int res = OK;
	int i = 0;
	struct force_update_flag force_burn;

	force_burn.code_update = 0;
	force_burn.panel_init = 0;
	for (; i < FLASH_MAX_SECTIONS; i++)
		force_burn.section_update[i] = 0;
	log_info(1, "%s: [1]: FW UPDATE..\n", __func__, res);
	res = flash_update(&force_burn);
	if (res != OK) {
		log_info(1, "%s: [1]: FW UPDATE FAILED..\n", __func__, res);
		return res;
	}
	if (force_burn.panel_init) {
		log_info(1, "%s: [2]: MP TEST..\n", __func__, res);
		res = fts_production_test_main(LIMITS_FILE, 0, &tests, 0);
		if (res != OK)
			log_info(1, "%s: [2]: MP TEST FAILED..\n",
				__func__, res);
	}

	log_info(1, "%s: [3]: TOUCH INIT..\n", __func__, res);
	res = fts_init_sensing(info);
	if (res != OK) {
		log_info(1, "%s: [3]: TOUCH INIT FAILED..\n",
				__func__, res);
		return res;
	}

	return res;
}

#ifndef FW_UPDATE_ON_PROBE
/**
  *	Function called by the delayed workthread executed after the probe in
  * order to perform the fw update flow
  *	@see  fts_chip_init()
  */
static void flash_update_auto(struct work_struct *work)
{
	struct delayed_work *fwu_work = container_of(work, struct delayed_work,
						     work);
	struct fts_ts_info *info = container_of(fwu_work, struct fts_ts_info,
						fwu_work);
	fts_chip_init(info);

}
#endif

/**
  * This function try to attempt to communicate with the IC for the first time
  * during the boot up process in order to read the necessary info for the
  * following stages.
  * The function execute a system reset, read fundamental info (system info)
  * @return OK if success or an error code which specify the type of error
  */
static int fts_init(struct fts_ts_info *info)
{
	int res = 0;
	u8 data[3] = { 0 };
	u16 chip_id = 0;

	open_channel(info->client);
	set_reset_gpio(info->board->reset_gpio);
	init_test_to_do();
#ifndef I2C_INTERFACE
#ifdef SPI4_WIRE
	log_info(1, "%s: Configuring SPI4..\n", __func__);
	res = configure_spi4();
	if (res < OK) {
		log_info(1, "%s: Error configuring IC in spi4 mode: %08X\n",
			__func__, res);
		return res;
	}
#endif
#endif
	res = fts_write_read_u8ux(FTS_CMD_HW_REG_R, HW_ADDR_SIZE,
				CHIP_ID_ADDRESS, data, 2, DUMMY_BYTE);
	if (res < OK) {
		log_info(1, "%s: Bus Connection issue: %08X\n", __func__, res);
		return res;
	}
	chip_id = (u16)((data[0] << 8) + data[1]);
	log_info(1, "%s: Chip id: 0x%04X\n", __func__, chip_id);
	if (chip_id != CHIP_ID) {
		log_info(1,
			"%s: Wrong Chip detected.. Expected|Detected: 0x%04X|0x%04X\n",
			__func__, CHIP_ID, chip_id);
		return ERROR_WRONG_CHIP_ID;
	}
	res = fts_system_reset(1);
	if (res < OK) {
		if (res == ERROR_BUS_W) {
			log_info(1, "%s: Bus Connection issue\n", __func__);
			return res;
		}
		/*other errors are because of no FW,
		so we continue to flash*/
	}
	res = read_sys_info();
	if (res < 0)
		log_info(1, "%s: Couldnot read sys info.. No FW..\n",
			 __func__);
	return OK;
}

/**
  * From the name of the power regulator get/put the actual regulator structs
  * (copying their references into fts_ts_info variable)
  * @param info pointer to fts_ts_info which contains info about the device and
  * its hw setup
  * @param get if 1, the regulators are get otherwise they are put (released)
  * back to the system
  * @return OK if success or an error code which specify the type of error
  */
static int fts_get_reg(struct fts_ts_info *info, bool get)
{
	int ret_val;

	if (!get) {
		ret_val = 0;
		goto regulator_put;
	}

	if (of_property_read_bool(info->dev->of_node, "vdd-supply")) {
		info->vdd_reg = regulator_get(info->dev, "vdd");
		if (IS_ERR(info->vdd_reg)) {
			log_info(1, "%s: Failed to get power regulator\n",
				 __func__);
			ret_val = -EPROBE_DEFER;
			goto regulator_put;
		}
	}

	if (of_property_read_bool(info->dev->of_node, "avdd-supply")) {
		info->avdd_reg = regulator_get(info->dev, "avdd");
		if (IS_ERR(info->avdd_reg)) {
			log_info(1,
				 "%s: Failed to get bus pullup regulator\n",
				 __func__);
			ret_val = -EPROBE_DEFER;
			goto regulator_put;
		}
	}

	return OK;

regulator_put:
	if (info->vdd_reg) {
		regulator_put(info->vdd_reg);
		info->vdd_reg = NULL;
	}

	if (info->avdd_reg) {
		regulator_put(info->avdd_reg);
		info->avdd_reg = NULL;
	}

	return ret_val;
}

/**
  * Enable or disable the power regulators
  * @param info pointer to fts_ts_info which contains info about the device and
  * its hw setup
  * @param enable if 1, the power regulators are turned on otherwise they are
  * turned off
  * @return OK if success or an error code which specify the type of error
  */
static int fts_enable_reg(struct fts_ts_info *info, bool enable)
{
	int ret_val;

	if (!enable) {
		ret_val = 0;
		goto disable_pwr_reg;
	}

	if (info->vdd_reg) {
		ret_val = regulator_enable(info->vdd_reg);
		if (ret_val < 0) {
			log_info(1, "%s: Failed to enable bus regulator\n",
				 __func__);
			goto exit;
		}
	}

	if (info->avdd_reg) {
		ret_val = regulator_enable(info->avdd_reg);
		if (ret_val < 0) {
			log_info(1, "%s: Failed to enable power regulator\n",
				 __func__);
			goto disable_bus_reg;
		}
	}

	return OK;

disable_pwr_reg:
	if (info->avdd_reg)
		regulator_disable(info->avdd_reg);

disable_bus_reg:
	if (info->vdd_reg)
		regulator_disable(info->vdd_reg);

exit:
	return ret_val;
}

/**
  * Configure a GPIO according to the parameters
  * @param gpio gpio number
  * @param config if true, the gpio is set up otherwise it is free
  * @param dir direction of the gpio, 0 = in, 1 = out
  * @param state initial value (if the direction is in, this parameter is
  * ignored)
  * return error code
  */

static int fts_gpio_setup(int gpio, bool config, int dir, int state)
{
	int ret_val = 0;
	unsigned char buf[16];

	if (config) {
		snprintf(buf, 16, "fts_gpio_%u\n", gpio);

		ret_val = gpio_request(gpio, buf);
		if (ret_val) {
			log_info(1, "%s %s: Failed to get gpio %d (code: %d)",
				 __func__, gpio, ret_val);
			return ret_val;
		}

		if (dir == 0)
			ret_val = gpio_direction_input(gpio);
		else
			ret_val = gpio_direction_output(gpio, state);
		if (ret_val) {
			log_info(1, "%s %s: Failed to set gpio %d direction",
				 __func__, gpio);
			return ret_val;
		}
	} else
		gpio_free(gpio);

	return ret_val;
}

/**
  * Setup the IRQ and RESET (if present) gpios.
  * If the Reset Gpio is present it will perform a cycle HIGH-LOW-HIGH in order
  *to assure that the IC has been reset properly
  */
static int fts_set_gpio(struct fts_ts_info *info)
{
	int ret_val;
	struct fts_hw_platform_data *bdata = info->board;

	ret_val = fts_gpio_setup(bdata->irq_gpio, true, 0, 0);
	if (ret_val < 0) {
		log_info(1, "%s %s: Failed to configure irq GPIO\n",
			 __func__);
		goto err_gpio_irq;
	}

	if (bdata->reset_gpio >= 0) {
		ret_val = fts_gpio_setup(bdata->reset_gpio, true, 1, 0);
		if (ret_val < 0) {
			log_info(1, "%s %s: Failed to configure reset GPIO\n",
				 __func__);
			goto err_gpio_reset;
		}
	}
	if (bdata->reset_gpio >= 0) {
		gpio_set_value(bdata->reset_gpio, 0);
		msleep(20);
		gpio_set_value(bdata->reset_gpio, 1);
	}

	return OK;

err_gpio_reset:
	fts_gpio_setup(bdata->irq_gpio, false, 0, 0);
	bdata->reset_gpio = GPIO_NOT_DEFINED;
err_gpio_irq:
	return ret_val;
}

/** Set pin state to active or suspend
  * @param active 1 for active while 0 for suspend
  */
static void fts_pinctrl_setup(struct fts_ts_info *info, bool active)
{
	int retval;

	if (info->ts_pinctrl) {
		/*
		 * Pinctrl setup is optional.
		 * If pinctrl is found, set pins to active/suspend state.
		 * Otherwise, go on without showing error messages.
		 */
		retval = pinctrl_select_state(info->ts_pinctrl, active ?
				info->pinctrl_state_active :
				info->pinctrl_state_suspend);
		if (retval < 0) {
			dev_err(info->dev, "Failed to select %s pinstate %d\n", active ?
				PINCTRL_STATE_ACTIVE : PINCTRL_STATE_SUSPEND,
				retval);
		}
	} else {
		dev_warn(info->dev, "ts_pinctrl is NULL\n");
	}
}

/**
  * Get/put the touch pinctrl from the specific names. If pinctrl is used, the
  * active and suspend pin control names and states are necessary.
  * @param info pointer to fts_ts_info which contains info about the device and
  * its hw setup
  * @param get if 1, the pinctrl is get otherwise it is put (released) back to
  * the system
  * @return OK if success or an error code which specify the type of error
  */
static int fts_pinctrl_get(struct fts_ts_info *info, bool get)
{
	int retval;

	if (!get) {
		retval = 0;
		goto pinctrl_put;
	}

	info->ts_pinctrl = devm_pinctrl_get(info->dev);
	if (IS_ERR_OR_NULL(info->ts_pinctrl)) {
		retval = PTR_ERR(info->ts_pinctrl);
		dev_info(info->dev, "Target does not use pinctrl %d\n", retval);
		goto err_pinctrl_get;
	}

	info->pinctrl_state_active
		= pinctrl_lookup_state(info->ts_pinctrl, PINCTRL_STATE_ACTIVE);
	if (IS_ERR_OR_NULL(info->pinctrl_state_active)) {
		retval = PTR_ERR(info->pinctrl_state_active);
		dev_err(info->dev, "Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_ACTIVE, retval);
		goto err_pinctrl_lookup;
	}

	info->pinctrl_state_suspend
		= pinctrl_lookup_state(info->ts_pinctrl, PINCTRL_STATE_SUSPEND);
	if (IS_ERR_OR_NULL(info->pinctrl_state_suspend)) {
		retval = PTR_ERR(info->pinctrl_state_suspend);
		dev_err(info->dev, "Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_SUSPEND, retval);
		goto err_pinctrl_lookup;
	}

	info->pinctrl_state_release
		= pinctrl_lookup_state(info->ts_pinctrl, PINCTRL_STATE_RELEASE);
	if (IS_ERR_OR_NULL(info->pinctrl_state_release)) {
		retval = PTR_ERR(info->pinctrl_state_release);
		dev_warn(info->dev, "Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_RELEASE, retval);
	}

	return OK;

err_pinctrl_lookup:
	devm_pinctrl_put(info->ts_pinctrl);
err_pinctrl_get:
	info->ts_pinctrl = NULL;
pinctrl_put:
	if (info->ts_pinctrl) {
		if (IS_ERR_OR_NULL(info->pinctrl_state_release)) {
			devm_pinctrl_put(info->ts_pinctrl);
			info->ts_pinctrl = NULL;
		} else {
			if (pinctrl_select_state(
					info->ts_pinctrl,
					info->pinctrl_state_release))
				dev_warn(info->dev, "Failed to select release pinstate\n");
		}
	}
	return retval;
}



/**
  * Retrieve and parse the hw information from the device tree node defined in
  * the system.
  * the most important information to obtain are: IRQ and RESET gpio numbers,
  * power regulator names
  * In the device file node is possible to define additional optional
  *information that can be parsed here.
  */
static int parse_dt(struct device *dev, struct fts_hw_platform_data *bdata)
{
	int retval;
	int index;
	struct of_phandle_args panelmap;
	struct device_node *np = dev->of_node;
	struct drm_panel *panel = NULL;

	if (of_property_read_bool(np, "st,panel_map")) {
		for (index = 0 ;; index++) {
			retval = of_parse_phandle_with_fixed_args(np,
								  "st,panel_map",
								  1,
								  index,
								  &panelmap);
			if (retval)
				return -EPROBE_DEFER;
			panel = of_drm_find_panel(panelmap.np);
			of_node_put(panelmap.np);
			if (!IS_ERR_OR_NULL(panel)) {
				bdata->panel = panel;
				break;
			}
		}
	}

	bdata->irq_gpio = of_get_named_gpio_flags(np, "st,irq-gpio", 0, NULL);

	log_info(1, "%s: irq_gpio = %d\n", __func__, bdata->irq_gpio);

	if (of_property_read_bool(np, "st,reset-gpio")) {
		bdata->reset_gpio = of_get_named_gpio_flags(np,
				"st,reset-gpio", 0, NULL);
		log_info(1, "%s: reset_gpio =%d\n",
			__func__, bdata->reset_gpio);
	} else
		bdata->reset_gpio = GPIO_NOT_DEFINED;

	return OK;
}

/**
  * Probe function, called when the driver it is matched with a device with the
  *same name compatible name
  * This function allocate, initialize and define all the most important
  *function and flow that are used by the driver to operate with the IC.
  * It allocates device variables, initialize queues and schedule works,
  *registers the IRQ handler, suspend/resume callbacks, registers the device to
  *the linux input subsystem etc.
  */
#ifdef I2C_INTERFACE
static int fts_probe(struct i2c_client *client, const struct i2c_device_id
						*idp)
{
#else
static int fts_probe(struct spi_device *client)
{
#endif

	struct fts_ts_info *info = NULL;
	int error = 0;
	struct device_node *dp = client->dev.of_node;
	int ret_val;
	u16 bus_type;
	u8 input_dev_free_flag = 0;
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	struct gti_optional_configuration *options;
#endif

	log_info(1, "%s: driver probe begin!\n", __func__);
	log_info(1, "%s: driver ver. %s\n", __func__, FTS_TS_DRV_VERSION);

	info = kzalloc(sizeof(struct fts_ts_info), GFP_KERNEL);
	if (!info) {
		dev_err(&client->dev, "Out of memory... Impossible to allocate struct info!\n");
		error = -ENOMEM;
		goto probe_error_exit_0;
	}

#ifdef I2C_INTERFACE
	log_info(1, "%s: I2C interface...\n", __func__);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		log_info(1, "%s: Unsupported I2C functionality\n", __func__);
		error = -EIO;
		goto probe_error_exit_1;
	}

	log_info(1, "%s: I2C address: %x\n", __func__, client->addr);
	bus_type = BUS_I2C;
#else
	client->mode = SPI_MODE_0;
#ifndef SPI4_WIRE
	client->mode |= SPI_3WIRE;
#endif
	if (client->controller->rt == false) {
		client->rt = true;
		ret_val = spi_setup(client);
		if (ret_val < 0) {
			log_info(1, "%s: setup SPI rt failed(%d)\n", __func__, ret_val);
			error = -EIO;
			goto probe_error_exit_1;
		}
	}

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	info->dma_mode = goog_check_spi_dma_enabled(client);
#endif
	log_info(1, "%s: SPI interface: dma_mode %d.\n", __func__, info->dma_mode );
	bus_type = BUS_SPI;
#endif

	log_info(1, "%s SET Device driver INFO:\n", __func__);

	info->client = client;
	info->dev = &info->client->dev;
	dev_set_drvdata(info->dev, info);

	if (dp) {
		info->board = devm_kzalloc(&client->dev,
					   sizeof(struct fts_hw_platform_data),
					   GFP_KERNEL);
		if (!info->board) {
			log_info(1, "%s: ERROR:info.board kzalloc failed\n",
				 __func__);
			goto probe_error_exit_1;
		}
		parse_dt(&client->dev, info->board);
	}

	log_info(1, "%s: SET Regulators:\n", __func__);
	error = fts_get_reg(info, true);
	if (error < 0) {
		log_info(1, "%s: ERROR:Failed to get regulators\n",
			 __func__);
		goto probe_error_exit_1;
	}

	ret_val = fts_enable_reg(info, true);
	if (ret_val < 0) {
		log_info(1, "%s: ERROR Failed to enable regulators\n",
			 __func__);
		goto probe_error_exit_2;
	}

	log_info(1, "%s: SET GPIOS_Test:\n", __func__);
	ret_val = fts_set_gpio(info);
	if (ret_val < 0) {
		log_info(1, "%s: ERROR Failed to set up GPIO's\n",
			 __func__);
		goto probe_error_exit_2;
	}
	info->client->irq = gpio_to_irq(info->board->irq_gpio);
	info->dev = &info->client->dev;

	dev_info(info->dev, "SET Pinctrl:\n");
	ret_val = fts_pinctrl_get(info, true);
	if (!ret_val)
		fts_pinctrl_setup(info, true);

	mutex_init(&info->fts_int_mutex);

	log_info(1, "%s: SET Input Device Property:\n", __func__);
	info->input_dev = input_allocate_device();
	if (!info->input_dev) {
		log_info(1, "%s: ERROR: No such input device defined!\n",
			__func__);
		error = -ENODEV;
		goto probe_error_exit_2;
	}
	info->input_dev->dev.parent = &client->dev;
	info->input_dev->name = FTS_TS_DRV_NAME;
	snprintf(fts_ts_phys, sizeof(fts_ts_phys), "%s/input0",
		 info->input_dev->name);
	info->input_dev->phys = fts_ts_phys;
	info->input_dev->uniq = "fts";
	info->input_dev->id.bustype = bus_type;
	info->input_dev->id.vendor = 0x0001;
	info->input_dev->id.product = 0x0002;
	info->input_dev->id.version = 0x0100;

	__set_bit(EV_SYN, info->input_dev->evbit);
	__set_bit(EV_KEY, info->input_dev->evbit);
	__set_bit(EV_ABS, info->input_dev->evbit);
	__set_bit(BTN_TOUCH, info->input_dev->keybit);

	input_mt_init_slots(info->input_dev, TOUCH_ID_MAX + PEN_ID_MAX,
		INPUT_MT_DIRECT);
	input_set_abs_params(info->input_dev, ABS_MT_POSITION_X, X_AXIS_MIN,
						X_AXIS_MAX, 0, 0);
	input_set_abs_params(info->input_dev, ABS_MT_POSITION_Y, Y_AXIS_MIN,
						Y_AXIS_MAX, 0, 0);
	input_set_abs_params(info->input_dev, ABS_MT_TOUCH_MAJOR, AREA_MIN,
						AREA_MAX, 0, 0);
	input_set_abs_params(info->input_dev, ABS_MT_TOUCH_MINOR, AREA_MIN,
						AREA_MAX, 0, 0);
	input_set_abs_params(info->input_dev, ABS_MT_PRESSURE, PRESSURE_MIN,
						PRESSURE_MAX, 0, 0);
	input_set_abs_params(info->input_dev, ABS_MT_DISTANCE, DISTANCE_MIN,
						DISTANCE_MAX, 0, 0);
	input_set_abs_params(info->input_dev, ABS_TILT_X, DISTANCE_MIN,
						DISTANCE_MAX, 0, 0);
	input_set_abs_params(info->input_dev, ABS_TILT_Y, DISTANCE_MIN,
						DISTANCE_MAX, 0, 0);
	error = input_register_device(info->input_dev);
	if (error) {
		log_info(1, "%s: ERROR: No such input device\n", __func__);
		error = -ENODEV;
		goto probe_error_exit_5;
	}
	input_dev_free_flag = 1;

	info->resume_bit = 1;
	ret_val = fts_init(info);
	if (ret_val < OK) {
		log_info(1, "%s: Initialization fails.. exiting..\n",
			__func__);
		goto probe_error_exit_6;
	}

	ret_val = fts_proc_init(info);
	if (ret_val < OK)
		log_info(1, "%s: Cannot create /proc filenode..\n", __func__);

#if defined(FW_UPDATE_ON_PROBE) && defined(FW_H_FILE)
	ret_val = fts_chip_init(info);
	if (ret_val < OK) {
		log_info(1, "%s: Flashing FW/Production Test/Touch Init Failed..\n",
			 __func__);
		goto probe_error_exit_6;
	}
#else
	log_info(1, "%s: SET Auto Fw Update:\n", __func__);
	info->fwu_workqueue = alloc_workqueue("fts-fwu-queue", WQ_UNBOUND |
					      WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
	if (!info->fwu_workqueue) {
		log_info(1, "%s ERROR: Cannot create fwu work thread\n",
			__func__);
		goto probe_error_exit_6;
	}
	INIT_DELAYED_WORK(&info->fwu_work, flash_update_auto);
#endif
#ifndef FW_UPDATE_ON_PROBE
	queue_delayed_work(info->fwu_workqueue, &info->fwu_work,
			   msecs_to_jiffies(1000));
#endif

#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
	options = devm_kzalloc(info->dev, sizeof(struct gti_optional_configuration), GFP_KERNEL);
	if (!options) {
		log_info(1, "%s: GTI optional configuration kzalloc failed.\n",
			__func__);
		goto probe_error_exit_6;
	}
	info->gti = goog_touch_interface_probe(
		info, info->dev, info->input_dev, gti_default_handler, options);
	ret_val = goog_pm_register_notification(info->gti, &fts_pm_ops);
	if (ret_val < 0) {
		log_info(1, "%s: Failed to register gti pm", __func__);
		goto probe_error_exit_7;
	}
#endif

	log_info(1, "%s: Probe Finished!\n", __func__);
	return OK;
#if IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
probe_error_exit_7:
	devm_kfree(info->dev, options);
#endif

probe_error_exit_6:
	input_unregister_device(info->input_dev);

probe_error_exit_5:
	if (!input_dev_free_flag)
		input_free_device(info->input_dev);

probe_error_exit_2:
	fts_enable_reg(info, false);
	fts_get_reg(info, false);

probe_error_exit_1:
	kfree(info);

probe_error_exit_0:
	log_info(1, "%s: Probe Failed!\n", __func__);

	return error;
}

/**
  * Clear and free all the resources associated to the driver.
  * This function is called when the driver need to be removed.
  */
#ifdef I2C_INTERFACE
static int fts_remove(struct i2c_client *client)
{
#else
static int fts_remove(struct spi_device *client)
{
#endif
	struct fts_ts_info *info = dev_get_drvdata(&(client->dev));

	fts_proc_remove();
	fts_interrupt_uninstall(info);
	input_unregister_device(info->input_dev);

#ifndef FW_UPDATE_ON_PROBE
	destroy_workqueue(info->fwu_workqueue);
#endif
	fts_enable_reg(info, false);
	fts_get_reg(info, false);
	kfree(info);
	return OK;
}

#ifdef CONFIG_PM
static int fts_pm_suspend(struct device *dev)
{
	struct fts_ts_info *info = dev_get_drvdata(dev);
	fts_suspend(info);
	return 0;
}

static int fts_pm_resume(struct device *dev)
{
	struct fts_ts_info *info = dev_get_drvdata(dev);
	fts_resume(info);
	return 0;
}

static SIMPLE_DEV_PM_OPS(fts_pm_ops, fts_pm_suspend, fts_pm_resume);
#endif

static struct of_device_id fts_of_match_table[] = {
	{
		.compatible = "st,fst2",
	},
	{},
};

#ifdef I2C_INTERFACE
static const struct i2c_device_id fts_device_id[] = {
	{ FTS_TS_DRV_NAME, 0 },
	{}
};

static struct i2c_driver fts_i2c_driver = {
	.driver			= {
		.name		= FTS_TS_DRV_NAME,
		.of_match_table = fts_of_match_table,
#if IS_ENABLED(CONFIG_PM) && !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
		.pm		= &fts_pm_ops,
#endif
	},
	.probe			= fts_probe,
	.remove			= fts_remove,
	.id_table		= fts_device_id,
};
#else
static struct spi_driver fts_spi_driver = {
	.driver			= {
		.name		= FTS_TS_DRV_NAME,
		.of_match_table = fts_of_match_table,
#if IS_ENABLED(CONFIG_PM) && !IS_ENABLED(CONFIG_GOOG_TOUCH_INTERFACE)
		.pm		= &fts_pm_ops,
#endif
		.owner		= THIS_MODULE,
	},
	.probe			= fts_probe,
	.remove			= fts_remove,
};

#endif

static int __init fts_driver_init(void)
{
#ifdef I2C_INTERFACE
	return i2c_add_driver(&fts_i2c_driver);
#else
	return spi_register_driver(&fts_spi_driver);
#endif
}

static void __exit fts_driver_exit(void)
{
#ifdef I2C_INTERFACE
		i2c_del_driver(&fts_i2c_driver);
#else
		spi_unregister_driver(&fts_spi_driver);
#endif
}


MODULE_DESCRIPTION("STMicroelectronics MultiTouch IC Driver");
MODULE_AUTHOR("STMicroelectronics");
MODULE_LICENSE("GPL");

late_initcall(fts_driver_init);
module_exit(fts_driver_exit);
