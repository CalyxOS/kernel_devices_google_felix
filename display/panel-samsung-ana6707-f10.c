// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based ana6707 AMOLED LCD panel driver.
 *
 * Copyright (c) 2022 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <drm/drm_vblank.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <video/mipi_display.h>

#include "include/trace/dpu_trace.h"
#include "samsung/panel/panel-samsung-drv.h"


#define ANA6707_F10_WRCTRLD_BCTRL_BIT   0x20

enum early_exit_status {
	EARLY_EXIT_OFF = 0,
	/* early exit is being enabled but not finished */
	EARLY_EXIT_IN_PROGRESS,
	EARLY_EXIT_ON,
};

/**
 * struct ana6707_f10_early_exit - early exit info
 *
 * This struct maintains ana6707 panel current status related to early exit
 */
struct ana6707_f10_early_exit {
	/** @status:  current early exit status */
	enum early_exit_status status;

	/** @delayed: delayed call for ana6707_f10_early_exit_post_enable */
	atomic_t delayed;
};

/**
 * struct ana6707_f10_mode_data - panel mode specific details
 *
 * This struct maintains panel mode specific details used to help with transitions between
 * different panel modes/refresh rates.
 */
struct ana6707_f10_mode_data {
	/**
	 * @auto_mode_pre_cmd_set:
	 *
	 * This cmd set needs to be sent before enabling auto_mode.
	 */
	const struct exynos_dsi_cmd_set *auto_mode_pre_cmd_set;
	/**
	 * @manual_mode_cmd_set:
	 *
	 * This cmd set is sent to panel during mode switch to enable manual mode. This mode is
	 * typically enabled when driver is not allowed to change modes while idle. In this mode,
	 * the panel should remain in this mode (regardless of idleness) until we indicate
	 * otherwise.
	 *
	 * If auto mode cmd set is defined, then manual mode cmd set should also be defined.
	 */
	const struct exynos_dsi_cmd_set *manual_mode_cmd_set;
};

/**
 * struct ana6707_f10_panel - panel specific runtime info
 *
 * This struct maintains ana6707 panel specific runtime info, any fixed details about panel should
 * most likely go into struct exynos_panel_desc
 */
struct ana6707_f10_panel {
	/** @base: base panel struct */
	struct exynos_panel base;

	/** @early_exit: current early exit info */
	struct ana6707_f10_early_exit early_exit;

	/**
	 * @auto_mode_vrefresh: indicates current minimum refresh rate while in auto mode,
	 *			if 0 it means that auto mode is not enabled
	 */
	u32 auto_mode_vrefresh;

	/**
	* @delayed_idle: indicates idle mode set is delayed due to idle_delay_ms,
	*                we should avoid changing idle_mode when it's true
	*/
	bool delayed_idle;
};

#define to_spanel(ctx) container_of(ctx, struct ana6707_f10_panel, base)

static const unsigned char PPS_SETTING[] = {
	0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x08, 0xA0,
	0x07, 0x30, 0x00, 0x20, 0x03, 0x98, 0x03, 0x98,
	0x02, 0x00, 0x03, 0x1A, 0x00, 0x20, 0x03, 0x87,
	0x00, 0x0C, 0x00, 0x0E, 0x03, 0x9D, 0x01, 0xDA,
	0x18, 0x00, 0x10, 0xE0, 0x03, 0x0C, 0x20, 0x00,
	0x06, 0x0B, 0x0B, 0x33, 0x0E, 0x1C, 0x2A, 0x38,
	0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B,
	0x7D, 0x7E, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40,
	0x09, 0xBE, 0x19, 0xFC, 0x19, 0xFA, 0x19, 0xF8,
	0x1A, 0x38, 0x1A, 0x78, 0x1A, 0xB6, 0x2A, 0xB6,
	0x2A, 0xF4, 0x2A, 0xF4, 0x4B, 0x34, 0x63, 0x74,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const u8 unlock_cmd_f0[] = { 0xF0, 0x5A, 0x5A };
static const u8 lock_cmd_f0[]   = { 0xF0, 0xA5, 0xA5 };
static const u8 update_key[] = { 0xF7, 0x07 };
static const u8 aod_on[] = { 0x53, 0x24 };
static const u8 aod_default[] = { 0x51, 0x07, 0xFF };
static const u8 aod_10nits[] = { 0x51, 0x01, 0x99 };
static const u8 display_off[] = { MIPI_DCS_SET_DISPLAY_OFF };
static const u8 display_on[] = { MIPI_DCS_SET_DISPLAY_ON };
static const u8 sleep_in[] = { MIPI_DCS_ENTER_SLEEP_MODE };
static const u8 early_exit_global_para[] = { 0xB0, 0x05 };
static const u8 mode_set_60hz[] = { 0x60, 0x08 };
static const u8 mode_set_120hz[] = { 0x60, 0x00 };

static const struct exynos_dsi_cmd ana6707_f10_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x5A, 0x5A),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x10),
	EXYNOS_DSI_CMD_SEQ(0xEF, 0x03, 0x03),
	EXYNOS_DSI_CMD_SEQ(0xF7, 0x07),
	EXYNOS_DSI_CMD_SEQ(0xF0, 0xA5, 0xA5),
	EXYNOS_DSI_CMD(display_off, 20),
	EXYNOS_DSI_CMD(sleep_in, 120),
};
static DEFINE_EXYNOS_CMD_SET(ana6707_f10_off);

static const struct exynos_dsi_cmd ana6707_f10_lp_cmds[] = {
	EXYNOS_DSI_CMD(display_off, 0),
};

static const struct exynos_dsi_cmd_set ana6707_f10_lp_cmd_set = {
	.num_cmd = ARRAY_SIZE(ana6707_f10_lp_cmds),
	.cmds = ana6707_f10_lp_cmds
};

static const struct exynos_dsi_cmd ana6707_f10_lp_off_cmds[] = {
	EXYNOS_DSI_CMD(display_off, 0)
};

static const struct exynos_dsi_cmd ana6707_f10_lp_low_cmds[] = {
	EXYNOS_DSI_CMD0(unlock_cmd_f0),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x93, 0x01),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x01),
	EXYNOS_DSI_CMD_SEQ(0x60, 0x01),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1, 0xB0, 0x4C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1, 0xC8, 0x01, 0x07, 0x67, 0x02),
	EXYNOS_DSI_CMD0_REV(aod_on, PANEL_REV_ALL_BUT(PANEL_REV_PROTO1_1)), /* AOD on */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0x53, 0x25), /* AOD on */

	EXYNOS_DSI_CMD0(early_exit_global_para),
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00), /* early exit on */
	EXYNOS_DSI_CMD_REV(aod_default, 34, PANEL_REV_LT(PANEL_REV_EVT1)),
	EXYNOS_DSI_CMD_REV(aod_10nits, 34, PANEL_REV_GE(PANEL_REV_EVT1)),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x02, 0x02), /* fixed TE */

	EXYNOS_DSI_CMD_SEQ(0xB0, 0x01), /* global para */
	EXYNOS_DSI_CMD_SEQ(0x60, 0x00), /* freq set */
	EXYNOS_DSI_CMD0(update_key),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x04), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0xC6), /* frame insertion HLPM on */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x14), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00, 0x80, 0x08, 0x00, 0x04, 0x04), /* frame insertion 10Hz */

	EXYNOS_DSI_CMD(display_on, 0),
	EXYNOS_DSI_CMD0(update_key),
	EXYNOS_DSI_CMD0(lock_cmd_f0),
};

static const struct exynos_dsi_cmd ana6707_f10_lp_high_cmds[] = {
	EXYNOS_DSI_CMD0(unlock_cmd_f0),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x93, 0x01),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x01),
	EXYNOS_DSI_CMD_SEQ(0x60, 0x01),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xB0, 0x4C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xC8, 0x00),
	EXYNOS_DSI_CMD0(aod_on), /* AOD on */

	EXYNOS_DSI_CMD0(early_exit_global_para),
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00), /* early exit on */
	EXYNOS_DSI_CMD(aod_default, 34),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x02, 0x02), /* fixed TE */

	EXYNOS_DSI_CMD_SEQ(0xB0, 0x01), /* global para */
	EXYNOS_DSI_CMD_SEQ(0x60, 0x00), /* freq set */
	EXYNOS_DSI_CMD0(update_key),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x04), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0xC6), /* frame insertion HLPM on */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x14), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00, 0x80, 0x08, 0x00, 0x04, 0x04), /* frame insertion 10Hz */

	EXYNOS_DSI_CMD(display_on, 0),
	EXYNOS_DSI_CMD0(update_key),
	EXYNOS_DSI_CMD0(lock_cmd_f0),
};

static const struct exynos_binned_lp ana6707_f10_binned_lp[] = {
	BINNED_LP_MODE("off",     0, ana6707_f10_lp_off_cmds),
	BINNED_LP_MODE("low",    80, ana6707_f10_lp_low_cmds),
	BINNED_LP_MODE("high", 2047, ana6707_f10_lp_high_cmds)
};

static const struct exynos_dsi_cmd ana6707_f10_early_exit_enable_cmds[] = {
	EXYNOS_DSI_CMD0(early_exit_global_para),
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00), /* early exit on */
};
static DEFINE_EXYNOS_CMD_SET(ana6707_f10_early_exit_enable);

static const struct exynos_dsi_cmd ana6707_f10_early_exit_post_enable_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x02, 0x02), /* fixed TE */
};
static DEFINE_EXYNOS_CMD_SET(ana6707_f10_early_exit_post_enable);
static const struct exynos_dsi_cmd ana6707_f10_60hz_auto_mode_pre_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x62), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00), /* OSC setting */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x01), /* global para */
	EXYNOS_DSI_CMD0(mode_set_60hz),
	EXYNOS_DSI_CMD0(update_key),
};
static DEFINE_EXYNOS_CMD_SET(ana6707_f10_60hz_auto_mode_pre);
static const struct exynos_dsi_cmd ana6707_f10_120hz_auto_mode_pre_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x62), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00), /* OSC setting */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x01), /* global para */
	EXYNOS_DSI_CMD0(mode_set_120hz),
	EXYNOS_DSI_CMD0(update_key),
};
static DEFINE_EXYNOS_CMD_SET(ana6707_f10_120hz_auto_mode_pre);
static const struct exynos_dsi_cmd ana6707_f10_60hz_manual_mode_cmds[] = {
	/* auto off */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x04),
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x80),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x0E), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),

	EXYNOS_DSI_CMD_SEQ(0xB9, 0x00, 0x00), /* changeable TE */
	EXYNOS_DSI_CMD0(early_exit_global_para),
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x80), /* early exit off */

	EXYNOS_DSI_CMD_SEQ(0xB0, 0x62), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00), /* OSC setting */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x01), /* global para */
	EXYNOS_DSI_CMD0(mode_set_60hz),
	EXYNOS_DSI_CMD0(update_key),
};
static DEFINE_EXYNOS_CMD_SET(ana6707_f10_60hz_manual_mode);
static const struct exynos_dsi_cmd ana6707_f10_120hz_manual_mode_cmds[] = {
	/* auto off */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x04),
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x80),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x0E), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),

	EXYNOS_DSI_CMD_SEQ(0xB9, 0x00, 0x00), /* changeable TE */
	EXYNOS_DSI_CMD0(early_exit_global_para),
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x80), /* early exit off */

	EXYNOS_DSI_CMD_SEQ(0xB0, 0x62), /* global para */
	EXYNOS_DSI_CMD_SEQ(0xBD, 0x00), /* OSC setting */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x01), /* global para */
	EXYNOS_DSI_CMD0(mode_set_120hz),
	EXYNOS_DSI_CMD0(update_key),
};
static DEFINE_EXYNOS_CMD_SET(ana6707_f10_120hz_manual_mode);

static const struct ana6707_f10_mode_data ana6707_f10_mode_120 = {
	.auto_mode_pre_cmd_set = &ana6707_f10_120hz_auto_mode_pre_cmd_set,
	.manual_mode_cmd_set = &ana6707_f10_120hz_manual_mode_cmd_set,
};

static const struct ana6707_f10_mode_data ana6707_f10_mode_60 = {
	.auto_mode_pre_cmd_set = &ana6707_f10_60hz_auto_mode_pre_cmd_set,
	.manual_mode_cmd_set = &ana6707_f10_60hz_manual_mode_cmd_set,
};

static inline bool is_auto_mode_preferred(struct exynos_panel *ctx)
{
	return ctx->panel_idle_enabled;
}

static u32 ana6707_f10_get_min_idle_vrefresh(struct exynos_panel *ctx,
					 const struct exynos_panel_mode *pmode)
{
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	const u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	int idle_vrefresh = ctx->min_vrefresh;

	if ((idle_vrefresh < 0) || !is_auto_mode_preferred(ctx))
		return 0;

	if (idle_vrefresh <= 10)
		idle_vrefresh = 10;
	else if (idle_vrefresh <= 30)
		idle_vrefresh = 30;
	else if (idle_vrefresh <= 60)
		idle_vrefresh = 60;
	else /* 120hz: no idle available */
		return 0;

	if (idle_vrefresh >= vrefresh) {
		dev_dbg(ctx->dev, "idle vrefresh (%u) higher than target (%u)\n",
			idle_vrefresh, vrefresh);
		return 0;
	}

	if (idle_vrefresh && ctx->idle_delay_ms &&
	    (panel_get_idle_time_delta(ctx) < ctx->idle_delay_ms)) {
		spanel->delayed_idle = true;
		idle_vrefresh = 0;
	} else {
		spanel->delayed_idle = false;
	}

	return idle_vrefresh;
}

static void ana6707_f10_set_manual_mode(struct exynos_panel *ctx, const struct exynos_panel_mode *pmode)
{
	const u32 flags = PANEL_CMD_SET_IGNORE_VBLANK | PANEL_CMD_SET_BATCH;
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	const struct ana6707_f10_mode_data *mdata = pmode->priv_data;
	const struct exynos_dsi_cmd_set *cmdset = mdata->manual_mode_cmd_set;

	if (cmdset)
		exynos_panel_send_cmd_set_flags(ctx, cmdset, flags);

	spanel->early_exit.status = EARLY_EXIT_OFF;
	spanel->auto_mode_vrefresh = 0;
}

static void ana6707_f10_early_exit_enable(struct exynos_panel *ctx)
{
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	const u32 flags = PANEL_CMD_SET_IGNORE_VBLANK | PANEL_CMD_SET_BATCH;

	if (spanel->early_exit.status == EARLY_EXIT_ON)
		return;

	dev_info(ctx->dev, "%s\n", __func__);

	DPU_ATRACE_BEGIN(__func__);
	exynos_panel_send_cmd_set_flags(ctx, &ana6707_f10_early_exit_enable_cmd_set, flags);
	DPU_ATRACE_END(__func__);

	spanel->early_exit.status = EARLY_EXIT_IN_PROGRESS;

	/**
	 * Early exit on commands are separated to two parts.
	 * The 1st part is sent in ana6707_f10_early_exit_enable, the 2nd
	 * part is sent in ana6707_f10_early_exit_post_enable.
	 *
	 * There is a HW constraint that we need to wait for the next TE
	 * falling after sending the 1st part. The 2nd part can be sent
	 * in the next commit_done, thus adding delay here makes sure we
	 * send the commands after next TE falling, that is:
	 *
	 * 1st > commit_done > next TE > next commit_done (2nd) > ..
	 */
	atomic_set(&spanel->early_exit.delayed, 2);
}

static void ana6707_f10_early_exit_post_enable(struct exynos_panel *ctx, bool force_update)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	int idle_vrefresh = 0;
	u8 step_cmd[] = {0xBD, 0x00, 0x80, 0x16, 0x00, 0x02, 0x02}; /* 10hz step setting */
	const struct exynos_dsi_cmd auto_mode_cmds[] = {
		EXYNOS_DSI_CMD_SEQ(0xB0, 0x04),
		EXYNOS_DSI_CMD_SEQ(0xBD, 0x82),
		EXYNOS_DSI_CMD_SEQ(0xB0, 0x0E), /* global para */
		/* 10Hz auto frame insertion */
		EXYNOS_DSI_CMD0(step_cmd),
	};
	DEFINE_EXYNOS_CMD_SET(auto_mode);
	const u32 flags = PANEL_CMD_SET_IGNORE_VBLANK | PANEL_CMD_SET_BATCH;

	if (unlikely(!pmode))
		return;

	if (spanel->early_exit.status != EARLY_EXIT_IN_PROGRESS)
		return;

	if (!force_update && atomic_dec_if_positive(&spanel->early_exit.delayed))
		return;

	if (ctx->panel_rev <= PANEL_REV_PROTO1_1)
		step_cmd[5] = 0x03;

	idle_vrefresh = ana6707_f10_get_min_idle_vrefresh(ctx, pmode);
	/* write auto step setting depending on target idle refresh rate */
	if (idle_vrefresh == 30) {
		step_cmd[3] = 0x06;
	} else if (idle_vrefresh == 60) {
		step_cmd[3] = 0x02;
	} else if (idle_vrefresh == 0) {
		dev_err(ctx->dev, "%s: invalid idle fps=%u\n", __func__, idle_vrefresh);
		return;
	}

	dev_info(ctx->dev, "%s\n", __func__);

	EXYNOS_DCS_WRITE_TABLE(ctx, unlock_cmd_f0);

	DPU_ATRACE_BEGIN(__func__);
	exynos_panel_send_cmd_set_flags(ctx, &ana6707_f10_early_exit_post_enable_cmd_set, flags);
	DPU_ATRACE_END(__func__);

	dev_dbg(ctx->dev, "%s: sending step setting (idle_fps=%u)\n",
		__func__, idle_vrefresh);

	exynos_panel_send_cmd_set_flags(ctx, &auto_mode_cmd_set, flags);
	EXYNOS_DCS_WRITE_TABLE(ctx, lock_cmd_f0);

	spanel->early_exit.status = EARLY_EXIT_ON;
	spanel->auto_mode_vrefresh = idle_vrefresh;

	if (force_update)
		atomic_set(&spanel->early_exit.delayed, 0);
}

static void ana6707_f10_flush_pending_early_exit(struct exynos_panel *ctx)
{
	struct ana6707_f10_panel *spanel = to_spanel(ctx);

	if (spanel->early_exit.status == EARLY_EXIT_IN_PROGRESS) {
		atomic_set(&spanel->early_exit.delayed, 1);
		ana6707_f10_early_exit_post_enable(ctx, false);
	}
}

static void ana6707_f10_update_refresh_mode(struct exynos_panel *ctx, const struct ana6707_f10_mode_data *mdata,
					const struct exynos_panel_mode *pmode, int idle_vrefresh)
{
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	const u32 flags = PANEL_CMD_SET_IGNORE_VBLANK | PANEL_CMD_SET_BATCH;

	ana6707_f10_flush_pending_early_exit(ctx);

	EXYNOS_DCS_WRITE_TABLE(ctx, unlock_cmd_f0);
	if (idle_vrefresh) {
		dev_dbg(ctx->dev, "%s: mode: %s with auto mode idle_vrefresh: %d\n", __func__,
			pmode->mode.name, idle_vrefresh);

		if (mdata->auto_mode_pre_cmd_set)
			exynos_panel_send_cmd_set_flags(ctx, mdata->auto_mode_pre_cmd_set, flags);

		if (spanel->early_exit.status == EARLY_EXIT_ON)
			spanel->early_exit.status = EARLY_EXIT_IN_PROGRESS;

		ana6707_f10_early_exit_enable(ctx);

	} else {
		dev_dbg(ctx->dev, "%s: mode: %s in manual mode\n", __func__, pmode->mode.name);

		ana6707_f10_set_manual_mode(ctx, pmode);
	}
	EXYNOS_DCS_WRITE_TABLE(ctx, lock_cmd_f0);

	/* when mode is explicitly set (manual) panel idle effect would be disabled */
	ctx->panel_idle_vrefresh = 0;
}

static void ana6707_f10_update_wrctrld(struct exynos_panel *ctx)
{
	u8 val = ANA6707_F10_WRCTRLD_BCTRL_BIT;

	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
	dev_dbg(ctx->dev, "%s(wrctrld:0x%x)\n", __func__, val);
}

static void ana6707_f10_change_frequency(struct exynos_panel *ctx,
				     const struct exynos_panel_mode *pmode)
{
	const struct ana6707_f10_mode_data *mdata = pmode->priv_data;
	u32 idle_vrefresh = 0;

	if (unlikely(!ctx || !mdata))
		return;

	if (pmode->idle_mode == IDLE_MODE_ON_INACTIVITY)
		idle_vrefresh = ana6707_f10_get_min_idle_vrefresh(ctx, pmode);

	ana6707_f10_update_refresh_mode(ctx, mdata, pmode, idle_vrefresh);

	dev_dbg(ctx->dev, "%s: change to %dhz\n", __func__, drm_mode_vrefresh(&pmode->mode));
}

static void ana6707_f10_set_nolp_mode(struct exynos_panel *ctx,
				  const struct exynos_panel_mode *pmode)
{
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	unsigned int vrefresh = drm_mode_vrefresh(&pmode->mode);
	u32 delay_us = mult_frac(1000, 1020, vrefresh);

	if (!ctx->enabled)
		return;

	EXYNOS_DCS_WRITE_TABLE(ctx, display_off);
	usleep_range(delay_us, delay_us + 10);
	EXYNOS_DCS_WRITE_TABLE(ctx, unlock_cmd_f0);

	if (ctx->panel_rev == PANEL_REV_PROTO1) {
		EXYNOS_DCS_WRITE_SEQ(ctx, 0xB0, 0x4C);
		EXYNOS_DCS_WRITE_SEQ(ctx, 0xC8, 0x00); /* normal mode set */
	}

	if (ctx->panel_rev <= PANEL_REV_PROTO1_1)
		EXYNOS_DCS_WRITE_SEQ(ctx, 0x93, 0x02); /* normal mode on */

	/* disable early exit while exiting AOD mode */
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xB9, 0x00, 0x00); /* changeable TE */
	EXYNOS_DCS_WRITE_TABLE(ctx, early_exit_global_para);
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xBD, 0x80); /* early exit off */
	spanel->early_exit.status = EARLY_EXIT_OFF;

	ana6707_f10_update_wrctrld(ctx); /* backlight control */
	EXYNOS_DCS_WRITE_TABLE(ctx, update_key);
	EXYNOS_DCS_WRITE_TABLE(ctx, lock_cmd_f0);
	ana6707_f10_change_frequency(ctx, pmode);
	usleep_range(delay_us, delay_us + 10);
	EXYNOS_DCS_WRITE_TABLE(ctx, display_on);

	dev_info(ctx->dev, "exit LP mode\n");
}

static void ana6707_f10_panel_reset(struct exynos_panel *ctx)
{
	dev_dbg(ctx->dev, "%s +\n", __func__);

	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10100, 10110);

	dev_dbg(ctx->dev, "%s -\n", __func__);

	exynos_panel_init(ctx);
}

static int ana6707_f10_disable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	struct ana6707_f10_panel *spanel = to_spanel(ctx);

	dev_dbg(ctx->dev, "%s\n", __func__);

	/* clear the flag since early exit is disabled after init */
	spanel->early_exit.status = EARLY_EXIT_OFF;

	return exynos_panel_disable(panel);
}

static int ana6707_f10_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_dbg(ctx->dev, "%s\n", __func__);

	ana6707_f10_panel_reset(ctx);

	EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 10, 0x11); /* sleep out: 10ms delay */

	exynos_dcs_compression_mode(ctx, 0x1); /* DSC_DEC_ON */
	EXYNOS_PPS_LONG_WRITE(ctx); /* PPS_SETTING */
	EXYNOS_DCS_WRITE_TABLE(ctx, update_key);

	EXYNOS_DCS_WRITE_SEQ(ctx, 0x35); /* TE on */
	EXYNOS_DCS_WRITE_TABLE(ctx, unlock_cmd_f0);
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xB0, 0x0A); /* Global para */
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xB9, 0x7C); /* TE2 option3 */
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xB0, 0x0D); /* Global para */
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xB9, 0x00, 0x06, 0xE5); /* Vsync to TE2 setting */

	EXYNOS_DCS_WRITE_SEQ(ctx, 0xB0, 0x08);
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xB9, 0x0A); /* TE pulse width 168us */
	EXYNOS_DCS_WRITE_TABLE(ctx, lock_cmd_f0);

	ana6707_f10_change_frequency(ctx, pmode);

	EXYNOS_DCS_WRITE_SEQ(ctx, 0x2A, 0x00, 0x00, 0x07, 0x2F); /* CASET */
	EXYNOS_DCS_WRITE_SEQ(ctx, 0x2B, 0x00, 0x00, 0x08, 0x9F); /* PASET */

	EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 110, 0x53, 0x20); /* backlight control */

	ctx->enabled = true;
	if (pmode->exynos_mode.is_lp_mode)
		exynos_panel_set_lp_mode(ctx, pmode);
	else
		EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 100, 0x29); /* display on: b/253361485 */

	return 0;
}

static void ana6707_f10_set_hbm_mode(struct exynos_panel *ctx,
				 enum exynos_hbm_mode mode)
{
	const bool irc_update =
		(IS_HBM_ON_IRC_OFF(ctx->hbm_mode) != IS_HBM_ON_IRC_OFF(mode));

	ctx->hbm_mode = mode;

	if (irc_update) {
		EXYNOS_DCS_WRITE_SEQ(ctx, 0xF0, 0x5A, 0x5A);
		EXYNOS_DCS_WRITE_SEQ(ctx, 0xB0, 0x0C,);
		EXYNOS_DCS_WRITE_SEQ(ctx, 0x92, IS_HBM_ON_IRC_OFF(mode) ? 0x85 : 0xA5);
		EXYNOS_DCS_WRITE_SEQ(ctx, 0xF0, 0xA5, 0xA5);
	}
	dev_info(ctx->dev, "IS_HBM_ON=%d IS_HBM_ON_IRC_OFF=%d\n", IS_HBM_ON(ctx->hbm_mode),
		 IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ana6707_f10_mode_set(struct exynos_panel *ctx,
			     const struct exynos_panel_mode *pmode)
{
	if (!ctx->enabled)
		return;

	ana6707_f10_change_frequency(ctx, pmode);
}

static void ana6707_f10_set_lp_mode(struct exynos_panel *ctx, const struct exynos_panel_mode *pmode)
{
	struct ana6707_f10_panel *spanel = to_spanel(ctx);

	dev_dbg(ctx->dev, "%s\n", __func__);

	ana6707_f10_flush_pending_early_exit(ctx);

	exynos_panel_set_lp_mode(ctx, pmode);

	/* early exit is enabled in AOD mode */
	spanel->early_exit.status = EARLY_EXIT_ON;
}

static bool ana6707_f10_is_mode_seamless(const struct exynos_panel *ctx,
				     const struct exynos_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void ana6707_f10_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 rev = (((build_code & 0xE0) >> 3) | (build_code & 0x0C) >> 2);

	switch (rev) {
	case 0x00:
		ctx->panel_rev = PANEL_REV_PROTO1;
		break;
	case 0x01:
		ctx->panel_rev = PANEL_REV_PROTO1_1;
		break;
	case 0x02:
		ctx->panel_rev = PANEL_REV_PROTO1_2;
		break;
	case 0x0C:
		ctx->panel_rev = PANEL_REV_EVT1;
		break;
	case 0x0D:
		ctx->panel_rev = PANEL_REV_EVT1_1;
		break;
	case 0x10:
		ctx->panel_rev = PANEL_REV_DVT1;
		break;
	case 0x11:
		ctx->panel_rev = PANEL_REV_DVT1_1;
		break;
	case 0x14:
		ctx->panel_rev = PANEL_REV_PVT;
		break;
	default:
		dev_warn(ctx->dev,
			 "unknown rev from panel (0x%x), default to latest\n",
			 rev);
		ctx->panel_rev = PANEL_REV_LATEST;
		return;
	}

	dev_info(ctx->dev, "panel_rev: 0x%x\n", ctx->panel_rev);
}

static void ana6707_f10_commit_done(struct exynos_panel *ctx)
{
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	const struct exynos_panel_mode *pmode = ctx->current_mode;

	if (!is_panel_active(ctx) || !pmode)
		return;

	if (pmode->idle_mode == IDLE_MODE_ON_INACTIVITY && spanel->delayed_idle) {
		ana6707_f10_change_frequency(ctx, pmode);
	} else  {
		ana6707_f10_early_exit_post_enable(ctx, false);
	}
}

static bool ana6707_f10_set_self_refresh(struct exynos_panel *ctx, bool enable)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	const struct ana6707_f10_mode_data *mdata;
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	u16 flags = PANEL_CMD_SET_IGNORE_VBLANK | PANEL_CMD_SET_BATCH;
	u32 idle_vrefresh;

	ana6707_f10_flush_pending_early_exit(ctx);

	if (unlikely(!pmode))
		return false;

	mdata = pmode->priv_data;
	if (unlikely(!mdata))
		return false;

	/* self refresh is not supported in lp mode since that always makes use of early exit */
	if (pmode->exynos_mode.is_lp_mode)
		return false;

	idle_vrefresh = ana6707_f10_get_min_idle_vrefresh(ctx, pmode);

	if (pmode->idle_mode != IDLE_MODE_ON_SELF_REFRESH) {
	/*
	 * if idle mode is on inactivity, may need to update the target fps for auto mode,
	 * or switch to manual mode if idle should be disabled (idle_vrefresh=0)
	 */
		if ((pmode->idle_mode == IDLE_MODE_ON_INACTIVITY) &&
			(spanel->auto_mode_vrefresh != idle_vrefresh)) {
			dev_dbg(ctx->dev,
				"early exit update needed for mode: %s (idle_vrefresh: %d)\n",
				pmode->mode.name, idle_vrefresh);
			spanel->early_exit.status = EARLY_EXIT_IN_PROGRESS;
			ana6707_f10_update_refresh_mode(ctx, mdata, pmode, idle_vrefresh);
			return true;
		}
		return false;
	}

	if (!enable)
		idle_vrefresh = 0;

	/* if there's no change in idle state then skip cmds */
	if (ctx->panel_idle_vrefresh == idle_vrefresh)
		return false;

	DPU_ATRACE_BEGIN(__func__);
	ctx->panel_idle_vrefresh = idle_vrefresh;

	dev_dbg(ctx->dev, "change panel idle vrefresh: %u for mode: %s\n", idle_vrefresh,
		pmode->mode.name);

	EXYNOS_DCS_WRITE_TABLE(ctx, unlock_cmd_f0);
	if (idle_vrefresh) {
		unsigned int vrefresh = drm_mode_vrefresh(&pmode->mode);
		u32 delay_us = mult_frac(1000, 1020, vrefresh);
		if (mdata->auto_mode_pre_cmd_set)
			exynos_panel_send_cmd_set_flags(ctx, mdata->auto_mode_pre_cmd_set, flags);

		if (spanel->early_exit.status == EARLY_EXIT_ON)
			spanel->early_exit.status = EARLY_EXIT_IN_PROGRESS;

		ana6707_f10_early_exit_enable(ctx);
		/* Because this panel requires 1 frame delay to enable early
		 * early exit. For the set_self_refresh case, it won't have
		 * subsequent commit_done event to trigger
		 * ana6707_f10_early_exit_post_enable(), so we finish the full
		 * early exit process here directly */
		usleep_range(delay_us, delay_us + 10);
		ana6707_f10_early_exit_post_enable(ctx, true);
	} else {
		dev_dbg(ctx->dev, "%s: mode: %s in manual mode\n", __func__, pmode->mode.name);

		ana6707_f10_set_manual_mode(ctx, pmode);
	}
	EXYNOS_DCS_WRITE_TABLE(ctx, lock_cmd_f0);

	backlight_state_changed(ctx->bl);

	DPU_ATRACE_END(__func__);

	return true;
}

static int ana6707_f10_set_power(struct exynos_panel *ctx, bool enable)
{
	int ret;

	if (enable) {
		if (ctx->vddr) {
			ret = regulator_enable(ctx->vddr);
			if (ret) {
				dev_err(ctx->dev, "vddr enable failed\n");
				return ret;
			}
		}

		if (ctx->vddi) {
			ret = regulator_enable(ctx->vddi);
			if (ret) {
				dev_err(ctx->dev, "vddi enable failed\n");
				return ret;
			}
		}

		if (ctx->vci) {
			ret = regulator_enable(ctx->vci);
			if (ret) {
				dev_err(ctx->dev, "vci enable failed\n");
				return ret;
			}
			usleep_range(10000, 10010);
		}
	} else {
		gpiod_set_value(ctx->reset_gpio, 0);

		if (ctx->vddr) {
			ret = regulator_disable(ctx->vddr);
			if (ret) {
				dev_err(ctx->dev, "vddr disable failed\n");
				return ret;
			}
		}

		if (ctx->vddi) {
			ret = regulator_disable(ctx->vddi);
			if (ret) {
				dev_err(ctx->dev, "vddi disable failed\n");
				return ret;
			}
		}

		if (ctx->vci) {
			ret = regulator_disable(ctx->vci);
			if (ret) {
				dev_err(ctx->dev, "vci disable failed\n");
				return ret;
			}
		}
	}

	return 0;
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

static const struct drm_dsc_config ana6707_f10_dsc_cfg = {
	.initial_dec_delay = 0x31A, /* pps18_19 */
	.scale_increment_interval = 0x387, /* pps22_23 */
	.first_line_bpg_offset = 0xE, /* pps27 */
	.nfl_bpg_offset = 0x39D, /* pps28_29 */
};

#define ANA6707_F10_DSC_CONFIG \
	.dsc = { \
		.enabled = true, \
		.dsc_count = 1, \
		.slice_count = 2, \
		.slice_height = 32, \
		.cfg = &ana6707_f10_dsc_cfg, \
		.is_scrv4 = true, \
	}

static const struct exynos_panel_mode ana6707_f10_modes[] = {
	{
		/* 1840x2208 @ 60Hz */
		.mode = {
			.name = "1840x2208x60",
			.clock = 248400,
			.hdisplay = 1840,
			.hsync_start = 1840 + 0, // add hfp
			.hsync_end = 1840 + 0, // add hsa
			.htotal = 1840 + 0, // add hbp
			.vdisplay = 2208,
			.vsync_start = 2208 + 7, // add vfp
			.vsync_end = 2208 + 7 + 7, // add vsa
			.vtotal = 2208 + 7 + 7 + 28, // add vbp
			.flags = 0,
			.width_mm = 123,
			.height_mm = 148,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			ANA6707_F10_DSC_CONFIG,
			.underrun_param = &underrun_param,
		},
		.priv_data = &ana6707_f10_mode_60,
		.idle_mode = IDLE_MODE_ON_SELF_REFRESH,
	},
	{
		/* 1840x2208 @ 120Hz */
		.mode = {
			.name = "1840x2208x120",
			.clock = 496800,
			.hdisplay = 1840,
			.hsync_start = 1840 + 0, // add hfp
			.hsync_end = 1840 + 0, // add hsa
			.htotal = 1840 + 0, // add hbp
			.vdisplay = 2208,
			.vsync_start = 2208 + 7, // add vfp
			.vsync_end = 2208 + 7 + 7, // add vsa
			.vtotal = 2208 + 7 + 7 + 28, // add vbp
			.flags = 0,
			.width_mm = 123,
			.height_mm = 148,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			ANA6707_F10_DSC_CONFIG,
			.underrun_param = &underrun_param,
		},
		.priv_data = &ana6707_f10_mode_120,
		.idle_mode = IDLE_MODE_ON_INACTIVITY,
	},
};

static const struct exynos_panel_mode ana6707_f10_lp_mode = {
	.mode = {
		/* TE and refresh rate will be 30Hz when early exit is enabled */
		/* 1840x2208 @ 30Hz */
		.name = "1840x2208x30",
		.clock = 124200,
		.hdisplay = 1840,
		.hsync_start = 1840 + 0, // add hfp
		.hsync_end = 1840 + 0, // add hsa
		.htotal = 1840 + 0, // add hbp
		.vdisplay = 2208,
		.vsync_start = 2208 + 7, // add vfp
		.vsync_end = 2208 + 7 + 7, // add vsa
		.vtotal = 2208 + 7 + 7 + 28, // add vbp
		.flags = 0,
		.type = DRM_MODE_TYPE_DRIVER,
		.width_mm = 123,
		.height_mm = 148,
	},
	.exynos_mode = {
		.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
		.vblank_usec = 120,
		.bpc = 8,
		ANA6707_F10_DSC_CONFIG,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	}
};

static void ana6707_f10_panel_mode_create_cmdset(struct exynos_panel *ctx,
					     const struct exynos_panel_mode *pmode)
{
	struct dentry *root;
	const struct ana6707_f10_mode_data *mdata = pmode->priv_data;

	if (!mdata)
		return;

	root = debugfs_create_dir(pmode->mode.name, ctx->debugfs_cmdset_entry);
	if (!root) {
		dev_err(ctx->dev, "unable to create %s mode debugfs dir\n", pmode->mode.name);
		return;
	}

	exynos_panel_debugfs_create_cmdset(ctx, root, mdata->auto_mode_pre_cmd_set,
					   "auto_mode_pre");
	exynos_panel_debugfs_create_cmdset(ctx, root, mdata->manual_mode_cmd_set, "manual_mode");
}

static void ana6707_f10_panel_init(struct exynos_panel *ctx)
{
	struct dentry *csroot = ctx->debugfs_cmdset_entry;
	struct ana6707_f10_panel *spanel = to_spanel(ctx);
	int i;

	exynos_panel_debugfs_create_cmdset(ctx, csroot, &ana6707_f10_early_exit_enable_cmd_set,
					   "early_exit_enable");
	exynos_panel_debugfs_create_cmdset(ctx, csroot, &ana6707_f10_early_exit_post_enable_cmd_set,
					   "early_exit_post_enable");
	for (i = 0; i < ctx->desc->num_modes; i++)
		ana6707_f10_panel_mode_create_cmdset(ctx, &ctx->desc->modes[i]);

	/* early exit is disabled by default */
	spanel->early_exit.status = EARLY_EXIT_OFF;
}

static int ana6707_f10_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ana6707_f10_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->auto_mode_vrefresh = 0;
	spanel->delayed_idle = false;

	return exynos_panel_common_init(dsi, &spanel->base);
}

static const struct drm_panel_funcs ana6707_f10_drm_funcs = {
	.disable = ana6707_f10_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = ana6707_f10_enable,
	.get_modes = exynos_panel_get_modes,
};

static const struct exynos_panel_funcs ana6707_f10_exynos_funcs = {
	.set_brightness = exynos_panel_set_brightness,
	.set_lp_mode = ana6707_f10_set_lp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_nolp_mode = ana6707_f10_set_nolp_mode,
	.set_hbm_mode = ana6707_f10_set_hbm_mode,
	.is_mode_seamless = ana6707_f10_is_mode_seamless,
	.mode_set = ana6707_f10_mode_set,
	.panel_init = ana6707_f10_panel_init,
	.set_power = ana6707_f10_set_power,
	.get_panel_rev = ana6707_f10_get_panel_rev,
	.commit_done = ana6707_f10_commit_done,
	.set_self_refresh = ana6707_f10_set_self_refresh,
};

const struct brightness_capability ana6707_f10_brightness_capability = {
	.normal = {
		.nits = {
			.min = 2,
			.max = 600,
		},
		.level = {
			.min = 7,
			.max = 2047,
		},
		.percentage = {
			.min = 0,
			.max = 60,
		},
	},
	.hbm = {
		.nits = {
			.min = 600,
			.max = 1000,
		},
		.level = {
			.min = 2052,
			.max = 3410,
		},
		.percentage = {
			.min = 60,
			.max = 100,
		},
	},
};

const struct exynos_panel_desc samsung_ana6707_f10 = {
	.dsc_pps = PPS_SETTING,
	.dsc_pps_len = ARRAY_SIZE(PPS_SETTING),
	.data_lane_cnt = 4,
	.max_brightness = 3410,
	.min_brightness = 7,
	.dft_brightness = 1023,
	.brt_capability = &ana6707_f10_brightness_capability,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.modes = ana6707_f10_modes,
	.num_modes = ARRAY_SIZE(ana6707_f10_modes),
	.off_cmd_set = &ana6707_f10_off_cmd_set,
	.lp_mode = &ana6707_f10_lp_mode,
	.lp_cmd_set = &ana6707_f10_lp_cmd_set,
	.binned_lp = ana6707_f10_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ana6707_f10_binned_lp),
	.is_panel_idle_supported = true,
	.panel_func = &ana6707_f10_drm_funcs,
	.exynos_panel_func = &ana6707_f10_exynos_funcs,
};

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "samsung,ana6707-f10", .data = &samsung_ana6707_f10 },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_panel_of_match);

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = ana6707_f10_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-samsung-ana6707-f10",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("YB Chiu <yubinc@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Samsung ana6707-f10 panel driver");
MODULE_LICENSE("GPL");
