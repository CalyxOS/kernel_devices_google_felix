/*
  *
  **************************************************************************
  **                        STMicroelectronics				  **
  **************************************************************************
  *                                                                        *
  * FTS Capacitive touch screen controller (FingerTipS)		           *
  *                                                                        *
  **************************************************************************
  **************************************************************************
  *
  */

/*!
  * \file fts.h
  * \brief Contains all the definitions and structs used generally by the driver
  */

#ifndef _LINUX_FTS_I2C_H_
#define _LINUX_FTS_I2C_H_

#include <linux/device.h>

#define FTS_TS_DRV_NAME		"fst2"
#define FTS_TS_DRV_VERSION	"6.0.3"
#define FTS_TS_DRV_VER		0x06000004

#define PINCTRL_STATE_ACTIVE    "pmx_ts_active"
#define PINCTRL_STATE_SUSPEND   "pmx_ts_suspend"
#define PINCTRL_STATE_RELEASE   "pmx_ts_release"

#define MAX_FIFO_EVENT	100 /* /< max number of events that the FIFO can
				 * collect  */

/* **** PANEL SPECIFICATION **** */
#define X_AXIS_MAX	2207	/* /< Max X coordinate of the display */
#define X_AXIS_MIN	0	/* /< min X coordinate of the display */
#define Y_AXIS_MAX	1839	/* /< Max Y coordinate of the display */
#define Y_AXIS_MIN	0	/* /< min Y coordinate of the display */

#define PRESSURE_MIN	0	/* /< min value of pressure reported */
#define PRESSURE_MAX	127	/* /< Max value of pressure reported */

#define DISTANCE_MIN	0	/* /< min distance between the tool and the
				 * display */
#define DISTANCE_MAX	127	/* /< Max distance between the tool and the
				 * display */

#define TOUCH_ID_MAX	10	/* /< Max number of simoultaneous touches
				 * reported */
#define PEN_ID_MAX		4	/* /< Max number of simoultaneous pen
				 * touches reported */

#define AREA_MIN	PRESSURE_MIN	/* /< min value of Major/minor axis
					 * reported */
#define AREA_MAX	PRESSURE_MAX	/* /< Man value of Major/minor axis
					 * reported */
/* **** END **** */



//#define DEBUG

/* Touch Types */
#define TOUCH_TYPE_FINGER_HOVER		0x00	/* /< Finger hover */
#define TOUCH_TYPE_FINGER			0x01	/* /< Finger touch */
#define TOUCH_TYPE_GLOVE			0x02	/* /< Glove touch */
#define TOUCH_TYPE_LARGE			0x03	/* /< Large touch */

#define GTI_PM_WAKELOCK_TYPE_LOCK_MASK 0xFFFF
/**
 * @brief: wakelock type.
 */
enum pm_wakelock_type {
	PM_WAKELOCK_TYPE_SCREEN_ON = 	0x01,
	PM_WAKELOCK_TYPE_IRQ = 		0x02,
	PM_WAKELOCK_TYPE_FW_UPDATE = 	0x04,
	PM_WAKELOCK_TYPE_SYSFS = 	0x08,
	PM_WAKELOCK_TYPE_FORCE_ACTIVE = 0x10,
	PM_WAKELOCK_TYPE_BUGREPORT = 	0x20,
	PM_WAKELOCK_TYPE_SYSINIT = 	0x40,
};

/*
  * Forward declaration
  */
struct fts_ts_info;

/*
  * Dispatch event handler
  */
typedef void (*event_dispatch_handler_t)
	(struct fts_ts_info *info, unsigned char *data);

/**
  * Struct which contains information about the HW platform and set up
  */
struct fts_hw_platform_data {
	int (*power)(bool on);
	int irq_gpio;
	int reset_gpio;
	struct drm_panel *panel;
};
/**
  * Struct contains FTS capacitive touch screen device information
  */
struct fts_ts_info {
	struct device            *dev;	/* /< Pointer to the structure device */
#ifdef I2C_INTERFACE
	struct i2c_client        *client;	/* /< I2C client structure */
#else
	struct spi_device        *client;	/* /< SPI client structure */
#endif
	struct fts_hw_platform_data *board;	/* /< HW info retrieved from
						 * device tree */
	struct regulator *vdd_reg;	/* /< DVDD power regulator */
	struct regulator *avdd_reg;	/* /< AVDD power regulator */

	struct pinctrl       *ts_pinctrl;		/* touch pin control state holder */
	struct pinctrl_state *pinctrl_state_active;	/* Active pin state*/
	struct pinctrl_state *pinctrl_state_suspend;	/* Suspend pin state*/
	struct pinctrl_state *pinctrl_state_release;	/* Release pin state*/

	ktime_t timestamp; /* time that the event was first received from the
		touch IC, acquired during hard interrupt, in CLOCK_MONOTONIC */
	struct mutex fts_int_mutex;
	bool irq_enabled;	/* Interrupt state */

	struct input_dev *input_dev; /* /< Input device structure */
	struct mutex input_report_mutex;/* /< mutex for handling the report
						 * of the pressure of keys */
	struct work_struct suspend_work;	/* /< Suspend work thread */
	struct work_struct resume_work;	/* /< Resume work thread */
	struct workqueue_struct *event_wq;	/* /< Workqueue used for event
						 * handler, suspend and resume
						 * work threads */
	event_dispatch_handler_t *event_dispatch_table;
	int resume_bit;	/* /< Indicate if screen off/on */
	unsigned int mode;	/* /< Device operating mode (bitmask: msb
				 * indicate if active or lpm) */
	u8 pm_wake_locks;
	struct mutex pm_wakelock_mutex;	/* Protect access to the bus_ref */
	unsigned long touch_id;	/* /< Bitmask for touch id (mapped to input
				 * slots) */
	bool sensor_sleep;	/* /< if true suspend was called while if false
				 * resume was called */
#ifndef FW_UPDATE_ON_PROBE
	struct delayed_work fwu_work;	/* /< Delayed work thread for fw update
					 * process */
	struct workqueue_struct  *fwu_workqueue;/* /< Fw update work
							 * queue */
#endif
	bool dma_mode;
};

extern int fts_proc_init(struct fts_ts_info *info);
extern int fts_proc_remove(void);
int fts_set_interrupt(struct fts_ts_info *info, bool enable);

int pm_wake_lock(struct fts_ts_info *info, enum pm_wakelock_type type);
int pm_wake_unlock(struct fts_ts_info *info, enum pm_wakelock_type type);



#endif
