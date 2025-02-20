/* linux/drivers/video/exynos_decon/dsim_drv.c
 *
 * Samsung SoC MIPI-DSIM driver.
 *
 * Copyright (c) 2014 Samsung Electronics
 *
 * Jiun Yu <jiun.yu@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/ctype.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/memory.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/regulator/consumer.h>
#include <linux/notifier.h>
#include <linux/pm_runtime.h>
#include <linux/lcd.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

#include <video/mipi_display.h>

#include <plat/cpu.h>

#include <mach/map.h>
#include <mach/exynos5-mipiphy.h>

#include "regs-dsim.h"
#include "dsim.h"
#include "decon.h"

#include "panels/dsim_panel.h"
#include "decon_board.h"

static DEFINE_MUTEX(dsim_rd_wr_mutex);
static DECLARE_COMPLETION(dsim_ph_wr_comp);
static DECLARE_COMPLETION(dsim_wr_comp);
static DECLARE_COMPLETION(dsim_rd_comp);

static int dsim_runtime_suspend(struct device *dev);
static int dsim_runtime_resume(struct device *dev);

#define MIPI_WR_TIMEOUT msecs_to_jiffies(50)
#define MIPI_RD_TIMEOUT msecs_to_jiffies(50)

#ifdef CONFIG_OF
static const struct of_device_id exynos5_dsim[] = {
	{ .compatible = "samsung,exynos5-dsim" },
	{},
};
MODULE_DEVICE_TABLE(of, exynos5_dsim);
#endif

struct dsim_device *dsim0_for_decon;
EXPORT_SYMBOL(dsim0_for_decon);

struct dsim_device *dsim1_for_decon;
EXPORT_SYMBOL(dsim1_for_decon);

static void dsim_dump(struct dsim_device *dsim)
{
	dsim_info("=== DSIM SFR DUMP ===\n");

	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			dsim->reg_base, 0xBC, false);
}

static void dsim_enable_interrupts(struct dsim_device *dsim)
{
	int prev_refcount;

	if (dsim->state != DSIM_STATE_HSCLKEN) {
		dev_err(dsim->dev, "DSIM is not ready. state(%d)\n", dsim->state);
		return;
	}

	if (dsim->lcd_info.mode != DECON_VIDEO_MODE)
		return;

	mutex_lock(&dsim->irq_lock);

	prev_refcount = dsim->irq_refcount++;
	if (!prev_refcount) {
		dsim_reg_set_int(dsim->id, 1);
		DISP_SS_EVENT_LOG(DISP_EVT_DSIM_INTR_ENABLE, &dsim->sd, ktime_set(0, 0));
	}

	mutex_unlock(&dsim->irq_lock);
}

static void dsim_disable_interrupts(struct dsim_device *dsim)
{
	int new_refcount;

	if (dsim->state != DSIM_STATE_HSCLKEN) {
		dev_err(dsim->dev, "DSIM is not ready. state(%d)\n", dsim->state);
		return;
	}

	if (dsim->lcd_info.mode != DECON_VIDEO_MODE)
		return;

	mutex_lock(&dsim->irq_lock);

	new_refcount = --dsim->irq_refcount;
	WARN_ON(new_refcount < 0);
	if (!new_refcount) {
		dsim_reg_set_int(dsim->id, 0);
		DISP_SS_EVENT_LOG(DISP_EVT_DSIM_INTR_DISABLE, &dsim->sd, ktime_set(0, 0));
	}

	mutex_unlock(&dsim->irq_lock);
}

static void dsim_long_data_wr(struct dsim_device *dsim, unsigned long data0, unsigned int data1)
{
	unsigned int data_cnt = 0, payload = 0;

	/* in case that data count is more then 4 */
	for (data_cnt = 0; data_cnt < data1; data_cnt += 4) {
		/*
		 * after sending 4bytes per one time,
		 * send remainder data less then 4.
		 */
		if ((data1 - data_cnt) < 4) {
			if ((data1 - data_cnt) == 3) {
				payload = *(u8 *)(data0 + data_cnt) |
				    (*(u8 *)(data0 + (data_cnt + 1))) << 8 |
					(*(u8 *)(data0 + (data_cnt + 2))) << 16;
			dev_dbg(dsim->dev, "count = 3 payload = %x, %x %x %x\n",
				payload, *(u8 *)(data0 + data_cnt),
				*(u8 *)(data0 + (data_cnt + 1)),
				*(u8 *)(data0 + (data_cnt + 2)));
			} else if ((data1 - data_cnt) == 2) {
				payload = *(u8 *)(data0 + data_cnt) |
					(*(u8 *)(data0 + (data_cnt + 1))) << 8;
			dev_dbg(dsim->dev,
				"count = 2 payload = %x, %x %x\n", payload,
				*(u8 *)(data0 + data_cnt),
				*(u8 *)(data0 + (data_cnt + 1)));
			} else if ((data1 - data_cnt) == 1) {
				payload = *(u8 *)(data0 + data_cnt);
			}

			dsim_reg_wr_tx_payload(dsim->id, payload);
		/* send 4bytes per one time. */
		} else {
			payload = *(u8 *)(data0 + data_cnt) |
				(*(u8 *)(data0 + (data_cnt + 1))) << 8 |
				(*(u8 *)(data0 + (data_cnt + 2))) << 16 |
				(*(u8 *)(data0 + (data_cnt + 3))) << 24;

			dev_dbg(dsim->dev,
				"count = 4 payload = %x, %x %x %x %x\n",
				payload, *(u8 *)(data0 + data_cnt),
				*(u8 *)(data0 + (data_cnt + 1)),
				*(u8 *)(data0 + (data_cnt + 2)),
				*(u8 *)(data0 + (data_cnt + 3)));

			dsim_reg_wr_tx_payload(dsim->id, payload);
		}
	}
}

static int dsim_wait_for_cmd_fifo_empty(struct dsim_device *dsim, int id)
{
	unsigned long wr_timeout = MIPI_WR_TIMEOUT;

#ifdef CONFIG_LCD_ALPM
	if (dsim->alpm)
		wr_timeout *= ALPM_TIMEOUT;
#endif

	switch (id) {
	case 0: /* SFR_PH_FIFO */
		if (!wait_for_completion_timeout(&dsim_ph_wr_comp,
			wr_timeout)) {
			if (dsim_read_mask(dsim->id, DSIM_INTSRC, DSIM_INTSRC_SFR_PH_FIFO_EMPTY)) {
				INIT_COMPLETION(dsim_ph_wr_comp);
				dsim_reg_clear_int(dsim->id, DSIM_INTSRC_SFR_PH_FIFO_EMPTY);
				return 0;
			}
			return -ETIMEDOUT;
		}
		break;
	case 1: /* SFR_PL_FIFO */
		if (!wait_for_completion_timeout(&dsim_wr_comp,
			wr_timeout)) {
			if (dsim_read_mask(dsim->id, DSIM_INTSRC, DSIM_INTSRC_SFR_PL_FIFO_EMPTY)) {
				INIT_COMPLETION(dsim_wr_comp);
				dsim_reg_clear_int(dsim->id, DSIM_INTSRC_SFR_PL_FIFO_EMPTY);
				return 0;
			}
			return -ETIMEDOUT;
		}
		break;
	default:
		break;
	}
	return 0;
}

int dsim_write_data(struct dsim_device *dsim, unsigned int data_id,
	unsigned long data0, unsigned int data1)
{
	int ret = 0;

	if (decon_int_drvdata) {
		/* LPD related: Decon must be enabled for PACKET_GO mode */
		decon_lpd_block_exit(decon_int_drvdata);
	}

	mutex_lock(&dsim_rd_wr_mutex);
	if (dsim->state != DSIM_STATE_HSCLKEN) {
		dev_err(dsim->dev, "DSIM is not ready. state(%d)\n", dsim->state);
		ret = -EINVAL;
		goto err_exit;
	}
	DISP_SS_EVENT_LOG_CMD(&dsim->sd, data_id, data0);

	dsim_enable_interrupts(dsim);

	switch (data_id) {
	/* short packet types of packet types for command. */
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
	case MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE:
		INIT_COMPLETION(dsim_ph_wr_comp);
		dsim_reg_clear_int(dsim->id, DSIM_INTSRC_SFR_PH_FIFO_EMPTY);
		dsim_reg_wr_tx_header(dsim->id, data_id, data0, data1);
		if (dsim_wait_for_cmd_fifo_empty(dsim, 0)) {
			dev_err(dsim->dev, "ID:%d : MIPI DSIM short packet write Timeout! 0x%lx\n",
						data_id, data0);
			ret = -ETIMEDOUT;
			goto exit;
		}
		break;

	/* general command */
	case MIPI_DSI_COLOR_MODE_OFF:
	case MIPI_DSI_COLOR_MODE_ON:
	case MIPI_DSI_SHUTDOWN_PERIPHERAL:
	case MIPI_DSI_TURN_ON_PERIPHERAL:
		INIT_COMPLETION(dsim_ph_wr_comp);
		dsim_reg_clear_int(dsim->id, DSIM_INTSRC_SFR_PH_FIFO_EMPTY);
		dsim_reg_wr_tx_header(dsim->id, data_id, data0, data1);
		if (dsim_wait_for_cmd_fifo_empty(dsim, 0)) {
			dev_err(dsim->dev, "ID: %d : MIPI DSIM short packet write Timeout! 0x%lx\n",
						data_id, data0);
			ret = -ETIMEDOUT;
			goto exit;
		}
		break;

	/* packet types for video data */
	case MIPI_DSI_V_SYNC_START:
	case MIPI_DSI_V_SYNC_END:
	case MIPI_DSI_H_SYNC_START:
	case MIPI_DSI_H_SYNC_END:
	case MIPI_DSI_END_OF_TRANSMISSION:
		break;

	/* short and response packet types for command */
	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM:
	case MIPI_DSI_DCS_READ:
		INIT_COMPLETION(dsim_ph_wr_comp);
		dsim_reg_clear_int(dsim->id, DSIM_INTSRC_SFR_PH_FIFO_EMPTY);
		dsim_reg_wr_tx_header(dsim->id, data_id, data0, data1);
		if (dsim_wait_for_cmd_fifo_empty(dsim, 0)) {
			dev_err(dsim->dev, "ID: %d : MIPI DSIM short packet write Timeout! 0x%lx\n",
						data_id, data0);
			ret = -ETIMEDOUT;
			goto exit;
		}
		break;

	/* long packet type and null packet */
	case MIPI_DSI_NULL_PACKET:
	case MIPI_DSI_BLANKING_PACKET:
		break;

	case MIPI_DSI_GENERIC_LONG_WRITE:
	case MIPI_DSI_DCS_LONG_WRITE:
	{
		unsigned int size;

		size = data1 * 4;
		INIT_COMPLETION(dsim_wr_comp);
		INIT_COMPLETION(dsim_ph_wr_comp);
		dsim_reg_clear_int(dsim->id, DSIM_INTSRC_SFR_PL_FIFO_EMPTY |
						DSIM_INTSRC_SFR_PH_FIFO_EMPTY);
		/* if data count is less then 4, then send 3bytes data.  */
		if (data1 < 4) {
			unsigned int payload = 0;
			payload = *(u8 *)(data0) |
				*(u8 *)(data0 + 1) << 8 |
				*(u8 *)(data0 + 2) << 16;

			dsim_reg_wr_tx_payload(dsim->id, payload);

			dev_dbg(dsim->dev, "count = %d payload = %x,%x %x %x\n",
				data1, payload,
				*(u8 *)(data0),
				*(u8 *)(data0 + 1),
				*(u8 *)(data0 + 2));
		/* in case that data count is more then 4 */
		} else
			dsim_long_data_wr(dsim, data0, data1);

		/* put data into header fifo */
		dsim_reg_wr_tx_header(dsim->id, data_id, data1 & 0xff, (data1 & 0xff00) >> 8);
		if (dsim_wait_for_cmd_fifo_empty(dsim, 1)) {
			dev_err(dsim->dev, "ID: %d : MIPI DSIM write Timeout!  0x%02x\n",
						data_id, *(u8 *)data0);
			ret = -ETIMEDOUT;
			goto exit;
		}
		if (dsim_wait_for_cmd_fifo_empty(dsim, 0)) {
			dev_err(dsim->dev, "ID: %d : MIPI DSIM short packet write Timeout! 0x%lx\n",
						data_id, data0);
			ret = -ETIMEDOUT;
			goto exit;
		}
		break;
	}

	/* packet typo for video data */
	case MIPI_DSI_PACKED_PIXEL_STREAM_16:
	case MIPI_DSI_PACKED_PIXEL_STREAM_18:
	case MIPI_DSI_PIXEL_STREAM_3BYTE_18:
	case MIPI_DSI_PACKED_PIXEL_STREAM_24:
		break;
	default:
		dev_warn(dsim->dev, "data id %x is not supported current DSI spec.\n", data_id);
		ret = -EINVAL;
		goto exit;
	}

exit:
	if ((dsim->state == DSIM_STATE_HSCLKEN) && (ret == -ETIMEDOUT)) {
		dev_err(dsim->dev, "0x%08X, 0x%08X, 0x%08X, 0x%08X\n",
				readl(dsim->reg_base + DSIM_STATUS),
				readl(dsim->reg_base + DSIM_INTSRC),
				readl(dsim->reg_base + DSIM_FIFOCTRL),
				readl(dsim->reg_base + DSIM_MULTI_PKT));
		dsim_reg_set_fifo_ctrl(dsim->id, DSIM_FIFOCTRL_INIT_SFR);
	}

	dsim_disable_interrupts(dsim);

err_exit:
	mutex_unlock(&dsim_rd_wr_mutex);
	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);
	return ret;
}


#ifdef CONFIG_FB_WINDOW_UPDATE
static int dsim_partial_area_command(struct dsim_device *dsim, void *arg)
{
	struct panel_private *priv = &dsim->priv;
	struct decon_win_rect *win_rect = (struct decon_win_rect *)arg;
	char data_2a[5];
	char data_2b[5];
	int retry;

	if (priv->lcdConnected == PANEL_DISCONNEDTED)
		return 0;

	/* w is right & h is bottom */
	data_2a[0] = MIPI_DCS_SET_COLUMN_ADDRESS;
	data_2a[1] = (win_rect->x >> 8) & 0xff;
	data_2a[2] = win_rect->x & 0xff;
	data_2a[3] = (win_rect->w >> 8) & 0xff;
	data_2a[4] = win_rect->w & 0xff;

	data_2b[0] = MIPI_DCS_SET_PAGE_ADDRESS;
	data_2b[1] = (win_rect->y >> 8) & 0xff;
	data_2b[2] = win_rect->y & 0xff;
	data_2b[3] = (win_rect->h >> 8) & 0xff;
	data_2b[4] = win_rect->h & 0xff;
	retry = 2;
	while (dsim_write_data(dsim, MIPI_DSI_DCS_LONG_WRITE,
				(unsigned long)data_2a, ARRAY_SIZE(data_2a)) != 0) {
		pr_info("%s:fail to write window update size a.\n", __func__);
		if (--retry <= 0) {
			pr_err("%s: size-a:failed: exceed retry count\n", __func__);
			return -1;
		}
	}

	retry = 2;
	while (dsim_write_data(dsim, MIPI_DSI_DCS_LONG_WRITE,
				(unsigned long)data_2b, ARRAY_SIZE(data_2b)) != 0) {
		printk(KERN_ERR "fail to write window update size b.\n");
		if (--retry <= 0) {
			pr_err("%s: size-b:failed: exceed retry count\n", __func__);
			return -1;
		}
	}

	return 0;
}

static void dsim_set_lcd_full_screen(struct dsim_device *dsim)
{
	struct decon_win_rect win_rect;

	win_rect.x = 0;
	win_rect.y = 0;
	win_rect.w = dsim->lcd_info.xres - 1;
	win_rect.h = dsim->lcd_info.yres - 1;
	dsim_partial_area_command(dsim, (void *)(&win_rect));

	return;
}
#else
static void dsim_set_lcd_full_screen(struct dsim_device *dsim)
{
	return;
}
#endif

static void dsim_rx_err_handler(struct dsim_device *dsim,
	u32 rx_fifo)
{
	/* Parse error report bit*/
	if (rx_fifo & (1 << 8))
		dev_err(dsim->dev, "SoT error!\n");
	if (rx_fifo & (1 << 9))
		dev_err(dsim->dev, "SoT sync error!\n");
	if (rx_fifo & (1 << 10))
		dev_err(dsim->dev, "EoT error!\n");
	if (rx_fifo & (1 << 11))
		dev_err(dsim->dev, "Escape mode entry command error!\n");
	if (rx_fifo & (1 << 12))
		dev_err(dsim->dev, "Low-power transmit sync error!\n");
	if (rx_fifo & (1 << 13))
		dev_err(dsim->dev, "HS receive timeout error!\n");
	if (rx_fifo & (1 << 14))
		dev_err(dsim->dev, "False control error!\n");
	/* Bit 15 is reserved*/
	if (rx_fifo & (1 << 16))
		dev_err(dsim->dev, "ECC error, single-bit(detected and corrected)!\n");
	if (rx_fifo & (1 << 17))
		dev_err(dsim->dev, "ECC error, multi-bit(detected, not corrected)!\n");
	if (rx_fifo & (1 << 18))
		dev_err(dsim->dev, "Checksum error(long packet only)!\n");
	if (rx_fifo & (1 << 19))
		dev_err(dsim->dev, "DSI data type not recognized!\n");
	if (rx_fifo & (1 << 20))
		dev_err(dsim->dev, "DSI VC ID invalid!\n");
	if (rx_fifo & (1 << 21))
		dev_err(dsim->dev, "Invalid transmission length!\n");
	/* Bit 22 is reserved */
	if (rx_fifo & (1 << 23))
		dev_err(dsim->dev, "DSI protocol violation!\n");
}

int dsim_read_data(struct dsim_device *dsim, u32 data_id,
	 u32 addr, u32 count, u8 *buf)
{
	u32 rx_fifo, rx_size = 0;
	int i, j, ret = 0;

	if (decon_int_drvdata) {
		/* LPD related: Decon must be enabled for PACKET_GO mode */
		decon_lpd_block_exit(decon_int_drvdata);
	}

	if (dsim->state != DSIM_STATE_HSCLKEN) {
		dev_err(dsim->dev, "DSIM is not ready. state(%d)\n", dsim->state);
		if (decon_int_drvdata)
			decon_lpd_unblock(decon_int_drvdata);
		return -EINVAL;
	}

	INIT_COMPLETION(dsim_rd_comp);
	dsim_enable_interrupts(dsim);

	/* Set the maximum packet size returned */
	dsim_write_data(dsim,
		MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE, count, 0);

	/* Read request */
	dsim_write_data(dsim, data_id, addr, 0);
	if (!wait_for_completion_timeout(&dsim_rd_comp,
		MIPI_RD_TIMEOUT)) {
		dsim_disable_interrupts(dsim);
		dev_err(dsim->dev, "MIPI DSIM read Timeout!\n");
		return -ETIMEDOUT;
	}

	dsim_disable_interrupts(dsim);
	mutex_lock(&dsim_rd_wr_mutex);
	DISP_SS_EVENT_LOG_CMD(&dsim->sd, data_id, (char)addr);

	rx_fifo = readl(dsim->reg_base + DSIM_RXFIFO);

	/* Parse the RX packet data types */
	switch (rx_fifo & 0xff) {
	case MIPI_DSI_RX_ACKNOWLEDGE_AND_ERROR_REPORT:
		dsim_rx_err_handler(dsim, rx_fifo);
		goto rx_error;
	case MIPI_DSI_RX_END_OF_TRANSMISSION:
		dev_dbg(dsim->dev, "EoTp was received from LCD module.\n");
		break;
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
		dev_dbg(dsim->dev, "Short Packet was received from LCD module.\n");
		for (i = 0; i < count; i++)
			buf[i] = (rx_fifo >> (8 + i * 8)) & 0xff;
		rx_size = count;
		break;
	case MIPI_DSI_RX_DCS_LONG_READ_RESPONSE:
	case MIPI_DSI_RX_GENERIC_LONG_READ_RESPONSE:
		dev_dbg(dsim->dev, "Long Packet was received from LCD module.\n");
		rx_size = (rx_fifo & 0x00ffff00) >> 8;
		dev_info(dsim->dev, "rx fifo : %8x, response : %x, rx_size : %d\n",
				rx_fifo, rx_fifo & 0xff, rx_size);
		if (rx_size > count)
			rx_size = count;
		/* Read data from RX packet payload */
		for (i = 0; i < rx_size >> 2; i++) {
			rx_fifo = readl(dsim->reg_base + DSIM_RXFIFO);
			for (j = 0; j < 4; j++)
				buf[(i*4)+j] = (u8)(rx_fifo >> (j * 8)) & 0xff;
		}
		if (rx_size % 4) {
			rx_fifo = readl(dsim->reg_base + DSIM_RXFIFO);
			for (j = 0; j < rx_size % 4; j++)
				buf[4 * i + j] =
					(u8)(rx_fifo >> (j * 8)) & 0xff;
		}
		break;
	default:
		dev_err(dsim->dev, "Packet format is invaild.\n");
		goto rx_error;
	}

	rx_fifo = readl(dsim->reg_base + DSIM_RXFIFO);
#if 0
	if (rx_fifo != DSIM_RX_FIFO_READ_DONE) {
		dev_info(dsim->dev, "%s Can't find RX FIFO READ DONE FLAG : %x\n",
			__func__, rx_fifo);
		goto clear_rx_fifo;
	}
#endif
	ret = rx_size;
	goto exit;

#if 0
clear_rx_fifo:
	i = 0;
	while (1) {
		rx_fifo = readl(dsim->reg_base + DSIM_RXFIFO);
		if ((rx_fifo == DSIM_RX_FIFO_READ_DONE) ||
				(i > DSIM_MAX_RX_FIFO))
			break;
		dev_info(dsim->dev, "%s clear rx fifo : %08x\n", __func__, rx_fifo);
		i++;
	}
	ret = 0;
	goto exit;
#endif

rx_error:
	dsim_reg_force_dphy_stop_state(dsim->id, 1);
	usleep_range(3000, 4000);
	dsim_reg_force_dphy_stop_state(dsim->id, 0);
	ret = -EPERM;
	goto exit;

exit:
	mutex_unlock(&dsim_rd_wr_mutex);
	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return ret;
}

#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
void dsim_pkt_go_ready(struct dsim_device *dsim)
{
	if (dsim->pktgo != DSIM_PKTGO_ENABLED) {
		return;
	}

	dsim_reg_set_pkt_go_ready(dsim->id);
}
extern void dsim_pkt_go_enable(struct dsim_device *dsim, bool enable);

void dsim_pkt_go_enable(struct dsim_device *dsim, bool enable)
{
	struct decon_device *decon = decon_int_drvdata;

	if (decon)
		decon_lpd_block(decon);

	if (dsim->state != DSIM_STATE_HSCLKEN)
		goto end;

	if (enable) {
		if (likely(dsim->pktgo == DSIM_PKTGO_ENABLED))
			goto end;

		dsim_reg_set_pkt_go_cnt(dsim->id, 0xff);
		dsim_reg_set_pkt_go_enable(dsim->id, true);
		dsim->pktgo = DSIM_PKTGO_ENABLED;
		dev_dbg(dsim->dev, "%s: DSIM_PKTGO_ENABLED", __func__);
	} else {
		if (unlikely(dsim->pktgo != DSIM_PKTGO_ENABLED))
			goto end;

		dsim_reg_set_pkt_go_cnt(dsim->id, 0x1); /* Do not use 0x0 */
		dsim_reg_set_pkt_go_enable(dsim->id, false);
		dsim->pktgo = DSIM_PKTGO_DISABLED;

		dev_dbg(dsim->dev, "%s: DSIM_PKTGO_DISABLED", __func__);
	}
end:
	if (decon)
		decon_lpd_unblock(decon);
}

#endif

static void dsim_write_test(struct dsim_device *dsim)
{
	int ret = 0, re_try = 4, i = 0;
	u32 test_data[2] = {0xaa, 0xaa};

	for (i = 0; i < re_try; i++) {
		ret = dsim_write_data(dsim, MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM,
				test_data[0], test_data[1]);
		if (ret < 0)
			dsim_dbg("%s: Failed to write test data!\n",
				(dsim->pktgo == DSIM_PKTGO_ENABLED) ? "PKT-GO mode" : "Non PKT-GO mode");
		else
			dsim_dbg("%s: Succeeded to write test data!\n",
				(dsim->pktgo == DSIM_PKTGO_ENABLED) ? "PKT-GO mode" : "Non PKT-GO mode");
	}
}

static void dsim_read_test(struct dsim_device *dsim)
{
	int ret = 0;
	int count = 3;
	u8 buf[4];
	u32 rd_addr = 0x04;

	ret = dsim_read_data(dsim, MIPI_DSI_DCS_READ, rd_addr, count, buf);

	if (ret < 0)
		dsim_dbg("%s: Failed to read test data!\n",
				(dsim->pktgo == DSIM_PKTGO_ENABLED) ? "PKT-GO mode" : "Non PKT-GO mode");
	else
		dsim_dbg("%s: Succeeded to read test data! test data[0] = %#x, test data[1] = %#x, test data[2] = %#x\n",
				((dsim->pktgo == DSIM_PKTGO_ENABLED) ? "PKT-GO mode" : "Non PKT-GO mode"), buf[0], buf[1], buf[3]);
}

static void dsim_d_phy_onoff(struct dsim_device *dsim,
	unsigned int enable)
{
	exynos5_dism_phy_enable(0, enable);
}

static irqreturn_t dsim_interrupt_handler(int irq, void *dev_id)
{
	unsigned int int_src;
	struct dsim_device *dsim = dev_id;
	int active;

	spin_lock(&dsim->slock);

	active = pm_runtime_active(dsim->dev);
	if (!active) {
		dev_warn(dsim->dev, "dsim power is off(%d), state(%d)\n", active, dsim->state);
		spin_unlock(&dsim->slock);
		return IRQ_HANDLED;
	}

	int_src = readl(dsim->reg_base + DSIM_INTSRC);

	/* Test bit */
	if (int_src & DSIM_INTSRC_SFR_PL_FIFO_EMPTY)
		complete(&dsim_wr_comp);
	if (int_src & DSIM_INTSRC_SFR_PH_FIFO_EMPTY)
		complete(&dsim_ph_wr_comp);
	if (int_src & DSIM_INTSRC_RX_DAT_DONE)
		complete(&dsim_rd_comp);
	if (int_src & DSIM_INTSRC_FRAME_DONE) {
		DISP_SS_EVENT_LOG(DISP_EVT_DSIM_FRAMEDONE, &dsim->sd, ktime_set(0, 0));
		if (decon_int_drvdata)
			decon_lpd_trig_reset(decon_int_drvdata);
	}
	if (int_src & DSIM_INTSRC_ERR_RX_ECC)
		dev_err(dsim->dev, "RX ECC Multibit error was detected!\n");
	dsim_reg_clear_int(dsim->id, int_src);

	spin_unlock(&dsim->slock);

	return IRQ_HANDLED;
}

static void dsim_clocks_info(struct dsim_device *dsim)
{
	dsim_info("%s: %ld Mhz\n", __clk_get_name(dsim->res.pclk),
				clk_get_rate(dsim->res.pclk) / MHZ);
	dsim_info("%s: %ld Mhz\n", __clk_get_name(dsim->res.dphy_esc),
				clk_get_rate(dsim->res.dphy_esc) / MHZ);
	dsim_info("%s: %ld Mhz\n", __clk_get_name(dsim->res.dphy_byte),
				clk_get_rate(dsim->res.dphy_byte) / MHZ);
	dsim_info("%s: %ld Mhz\n", __clk_get_name(dsim->res.rgb_vclk0),
				clk_get_rate(dsim->res.rgb_vclk0) / MHZ);
	dsim_info("%s: %ld Mhz\n", __clk_get_name(dsim->res.pclk_disp),
				clk_get_rate(dsim->res.pclk_disp) / MHZ);
}

static int dsim_get_clocks(struct dsim_device *dsim)
{
	struct device *dev = dsim->dev;

	if (!dsim->id) {
		dsim->res.pclk = clk_get(dev, "pclk_dsim0");
		if (IS_ERR_OR_NULL(dsim->res.pclk)) {
			dsim_err("failed to get pclk_dsim0\n");
			return -ENODEV;
		}

		dsim->res.dphy_esc = clk_get(dev, "mipi0_rx");
		if (IS_ERR_OR_NULL(dsim->res.dphy_esc)) {
			dsim_err("failed to get mipi0_rx\n");
			return -ENODEV;
		}

		dsim->res.dphy_byte = clk_get(dev, "mipi0_bit");
		if (IS_ERR_OR_NULL(dsim->res.dphy_byte)) {
			dsim_err("failed to get mipi0_bit\n");
			return -ENODEV;
		}

		/* This clock is used for transferring RGB data from decon-int to dsim0
		 * So, It must be enabled before starting decon-int and disabled after
		 * stopping decon-int */
		dsim->res.rgb_vclk0 = clk_get(dev, "rgb_vclk0");
		if (IS_ERR_OR_NULL(dsim->res.rgb_vclk0)) {
			dsim_err("failed to get rgb_vclk0\n");
			return -ENODEV;
		}

		dsim->res.pclk_disp = clk_get(dev, "pclk_disp");
		if (IS_ERR_OR_NULL(dsim->res.pclk_disp)) {
			decon_err("failed to get pclk_disp\n");
			return -ENODEV;
		}
	} else {
		dsim->res.pclk = clk_get(dev, "pclk_dsim1");
		if (IS_ERR_OR_NULL(dsim->res.pclk)) {
			dsim_err("failed to get pclk_dsim1\n");
			return -ENODEV;
		}

		dsim->res.dphy_esc = clk_get(dev, "mipi1_rx");
		if (IS_ERR_OR_NULL(dsim->res.dphy_esc)) {
			dsim_err("failed to get mipi1_rx\n");
			return -ENODEV;
		}

		dsim->res.dphy_byte = clk_get(dev, "mipi1_bit");
		if (IS_ERR_OR_NULL(dsim->res.dphy_byte)) {
			dsim_err("failed to get mipi1_bit\n");
			return -ENODEV;
		}

		/* This clock is used for transferring RGB data from decon-int to dsim1
		 * So, It must be enabled before starting decon-int and disabled after
		 * stopping decon-int */
		dsim->res.rgb_vclk0 = clk_get(dev, "rgb_vclk1");
		if (IS_ERR_OR_NULL(dsim->res.rgb_vclk0)) {
			dsim_err("failed to get rgb_vclk1\n");
			return -ENODEV;
		}

		dsim->res.pclk_disp = clk_get(dev, "pclk_disp");
		if (IS_ERR_OR_NULL(dsim->res.pclk_disp)) {
			decon_err("failed to get pclk_disp\n");
			return -ENODEV;
		}
	}

	return 0;
}

static void dsim_put_clocks(struct dsim_device *dsim)
{
	clk_put(dsim->res.pclk);
	clk_put(dsim->res.dphy_esc);
	clk_put(dsim->res.dphy_byte);
	clk_put(dsim->res.rgb_vclk0);
	clk_put(dsim->res.pclk_disp);
}

static int dsim_get_gpios(struct dsim_device *dsim)
{
	return 0;
}

static int dsim_reset_panel(struct dsim_device *dsim)
{
	dsim_dbg("%s +\n", __func__);

	run_list(dsim->dev, __func__);

	dsim_dbg("%s -\n", __func__);

	return 0;
}

int dsim_set_panel_pre_power(struct dsim_device *dsim)
{
        dsim_dbg("%s +\n", __func__);

        run_list(dsim->dev, __func__);

        dsim_dbg("%s -\n", __func__);

        return 0;
}


static int dsim_set_panel_power(struct dsim_device *dsim, bool on)
{
	dsim_dbg("%s(%d) +\n", __func__, on);

	if (on)
		run_list(dsim->dev, "dsim_set_panel_power_enable");
	else
		run_list(dsim->dev, "dsim_set_panel_power_disable");

	dsim_dbg("%s(%d) -\n", __func__, on);

	return 0;
}

static int dsim_enable(struct dsim_device *dsim)
{
	pr_info("%s ++\n", __func__);
	if (dsim->state == DSIM_STATE_HSCLKEN) {
#ifdef CONFIG_LCD_DOZE_MODE
                if (IS_DOZE(dsim->doze_state)) {
                        call_panel_ops(dsim, exitalpm, dsim);
                }
#endif
		goto exit;
        }


#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_get_sync(dsim->dev);
#else
	dsim_runtime_resume(dsim->dev);
#endif



        /* Panel power on */
#ifdef CONFIG_LCD_DOZE_MODE
        if (IS_DOZE(dsim->doze_state)) {
                dsim_info("%s: exit doze\n", __func__);
        } else {
                dsim_set_panel_power(dsim, 1);
        }
#else
        dsim_set_panel_power(dsim, 1);
#endif

	dsim_set_panel_pre_power(dsim);

	call_panel_ops(dsim, resume, dsim);

	/* DPHY power on */
	dsim_d_phy_onoff(dsim, 1);

	dsim_reg_init(dsim->id, &dsim->lcd_info, dsim->data_lane_cnt);

	dsim_reg_enable_clocks(dsim->id, &dsim->clks_param,
				DSIM_LANE_CLOCK | dsim->data_lane);

	clk_prepare_enable(dsim->res.dphy_esc);
	clk_prepare_enable(dsim->res.dphy_byte);

	dsim_reg_set_lanes(dsim->id, DSIM_LANE_CLOCK | dsim->data_lane, 1);

#ifdef CONFIG_LCD_DOZE_MODE
        if (IS_DOZE(dsim->doze_state)) {
                dsim_info("%s: exit doze\n", __func__);
        } else {
                dsim_reset_panel(dsim);
        }
#else
        dsim_reset_panel(dsim);
#endif

#ifdef CONFIG_PANEL_LTM184HL01
	call_panel_ops(dsim, display_lvds_init, dsim);
#endif


        dsim_reg_start(dsim->id, &dsim->lcd_info, DSIM_LANE_CLOCK | dsim->data_lane);

	dsim->state = DSIM_STATE_HSCLKEN;

        enable_irq(dsim->irq);

#ifdef CONFIG_LCD_DOZE_MODE
        if (IS_DOZE(dsim->doze_state)) {
                call_panel_ops(dsim, exitalpm, dsim);
        } else {
                call_panel_ops(dsim, displayon, dsim);
        }
#else
        call_panel_ops(dsim, displayon, dsim);
#endif

exit:
#ifdef CONFIG_LCD_DOZE_MODE
        dsim->doze_state = DOZE_STATE_NORMAL;
#endif
        dsim_info("%s: --\n", __func__);
        return 0;

}

static int dsim_disable(struct dsim_device *dsim) {

	if (dsim->state == DSIM_STATE_SUSPEND)
		return 0;

#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	dsim_pkt_go_enable(dsim, false);
#endif
	dsim_set_lcd_full_screen(dsim);
	call_panel_ops(dsim, suspend, dsim);

#ifdef CONFIG_LCD_DOZE_MODE
        dsim->doze_state = DOZE_STATE_SUSPEND;
#endif


	/* Wait for current read & write CMDs. */
	mutex_lock(&dsim_rd_wr_mutex);
	dsim->state = DSIM_STATE_SUSPEND;
	mutex_unlock(&dsim_rd_wr_mutex);

	/* disable interrupts */
	//dsim_reg_set_int(dsim->id, 0);

	/* disable HS clock */
	//dsim_reg_set_hs_clock(dsim->id, &dsim->lcd_info, 0);

	/* make CLK/DATA Lane as LP00 */
	//dsim_reg_set_lanes(dsim->id, DSIM_LANE_CLOCK | dsim->data_lane, 0);

	//dsim_reg_set_clocks(dsim->id, NULL, DSIM_LANE_CLOCK | dsim->data_lane, 0);

	//dsim_reg_sw_reset(dsim->id);

        disable_irq(dsim->irq);
        dsim_reg_stop(dsim->id, &dsim->lcd_info, DSIM_LANE_CLOCK | dsim->data_lane);


	dsim_d_phy_onoff(dsim, 0);

	dsim_set_panel_power(dsim, 0);

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_put_sync(dsim->dev);
#else
	dsim_runtime_suspend(dsim->dev);
#endif
	pr_info("%s --\n", __func__);

	return 0;
}

#ifdef CONFIG_LCD_DOZE_MODE
static int dsim_doze_enable(struct dsim_device *dsim)
{
	if (dsim->state == DSIM_STATE_HSCLKEN) {
		if (dsim->doze_state != DOZE_STATE_DOZE) {
			call_panel_ops(dsim, enteralpm, dsim);
		}
		goto exit;
	}

	dsim_info("%s: ++ %d, %d\n", __func__, dsim->state, dsim->doze_state);

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_get_sync(dsim->dev);
#else
	dsim_runtime_resume(dsim->dev);
#endif

	if (dsim->doze_state == DOZE_STATE_SUSPEND)
		dsim_set_panel_power(dsim, 1);

	/* DPHY power on */
        dsim_d_phy_onoff(dsim, 1);


	dsim_reg_init(dsim->id, &dsim->lcd_info, dsim->data_lane_cnt);

	dsim_reg_enable_clocks(dsim->id, &dsim->clks_param,
				DSIM_LANE_CLOCK | dsim->data_lane);

	clk_prepare_enable(dsim->res.dphy_esc);
	clk_prepare_enable(dsim->res.dphy_byte);

	dsim_reg_set_lanes(dsim->id, DSIM_LANE_CLOCK | dsim->data_lane, 1);


	if (dsim->doze_state == DOZE_STATE_SUSPEND)
		dsim_reset_panel(dsim);

        dsim_reg_start(dsim->id, &dsim->lcd_info, DSIM_LANE_CLOCK | dsim->data_lane);

        dsim->state = DSIM_STATE_HSCLKEN;

        enable_irq(dsim->irq);

	if (dsim->doze_state == DOZE_STATE_SUSPEND || dsim->doze_state == DOZE_STATE_DOZE_SUSPEND)
		call_panel_ops(dsim, enteralpm, dsim);

exit:
	dsim->doze_state = DOZE_STATE_DOZE;

	dsim_info("%s: --\n", __func__);

	return 0;
}

static int dsim_doze_suspend(struct dsim_device *dsim)
{
	if (dsim->state == DSIM_STATE_SUSPEND)
		goto exit;

	dsim_info("%s: ++ %d, %d\n", __func__, dsim->state, dsim->doze_state);

#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	dsim_pkt_go_enable(dsim, false);
#endif
	dsim_set_lcd_full_screen(dsim);

	if (dsim->doze_state == DOZE_STATE_NORMAL)
		call_panel_ops(dsim, enteralpm, dsim);

	dsim->doze_state = DOZE_STATE_DOZE_SUSPEND;

	/* Wait for current read & write CMDs. */
	mutex_lock(&dsim_rd_wr_mutex);
	dsim->state = DSIM_STATE_SUSPEND;
	mutex_unlock(&dsim_rd_wr_mutex);


        /* disable interrupts */
        dsim_reg_set_int(dsim->id, 0);

        disable_irq(dsim->irq);

        /* disable HS clock */
        dsim_reg_set_hs_clock(dsim->id, &dsim->lcd_info, 0);

        /* make CLK/DATA Lane as LP00 */
        dsim_reg_set_lanes(dsim->id, DSIM_LANE_CLOCK | dsim->data_lane, 0);

        dsim_reg_set_clocks(dsim->id, NULL, DSIM_LANE_CLOCK | dsim->data_lane, 0);

        dsim_reg_sw_reset(dsim->id);

        dsim_d_phy_onoff(dsim, 0);


#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_put_sync(dsim->dev);
#else
	dsim_runtime_suspend(dsim->dev);
#endif

exit:
	dsim_info("%s: --\n", __func__);
	return 0;
}
#endif

static int dsim_set_ulps_by_ddi(struct dsim_device *dsim, u32 en)
{
	int ret;

	switch (dsim->lcd_info.ddi_type) {
	case TYPE_OF_SM_DDI:
		ret = dsim_reg_set_smddi_ulps(dsim->id, en, dsim->data_lane);
		break;
	case TYPE_OF_MAGNA_DDI:
		ret = 0;
		break;
	case TYPE_OF_NORMAL_DDI:
	default:
		ret = dsim_reg_set_ulps(dsim->id, en, dsim->data_lane);
		break;
	}

	return ret;
}

static int dsim_enter_ulps(struct dsim_device *dsim)
{
	int ret = 0;

	DISP_SS_EVENT_START();
	dsim_dbg("%s +\n", __func__);
	exynos_ss_printk("%s:state %d: active %d:+\n", __func__,
				dsim->state, pm_runtime_active(dsim->dev));

	if (dsim->state != DSIM_STATE_HSCLKEN) {
		ret = -EBUSY;
		goto err;
	}

#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	dsim_pkt_go_enable(dsim, false);
#endif

	/* Wait for current read & write CMDs. */
	mutex_lock(&dsim_rd_wr_mutex);
	dsim->state = DSIM_STATE_ULPS;
	mutex_unlock(&dsim_rd_wr_mutex);

	/* disable interrupts */
	dsim_reg_set_int(dsim->id, 0);

	disable_irq(dsim->irq);

	/* disable HS clock */
	dsim_reg_set_hs_clock(dsim->id, &dsim->lcd_info, 0);

	/* try to enter ULPS mode. The sequence is depends on DDI type */
	ret = dsim_set_ulps_by_ddi(dsim, 1);
	if (ret < 0) {
		dsim_info("%s: failed to enter ULPS: %d", __func__, ret);
		dsim_dump(dsim);
		ret = 0;
	}

	/* make CLK/DATA Lane as LP00 */
	dsim_reg_set_lanes(dsim->id, DSIM_LANE_CLOCK | dsim->data_lane, 0);

	dsim_reg_set_clocks(dsim->id, NULL, DSIM_LANE_CLOCK | dsim->data_lane, 0);

	dsim_reg_sw_reset(dsim->id);

	dsim_d_phy_onoff(dsim, 0);

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_put_sync(dsim->dev);
#else
	dsim_runtime_suspend(dsim->dev);
#endif

	DISP_SS_EVENT_LOG(DISP_EVT_ENTER_ULPS, &dsim->sd, start);
err:
	dsim_dbg("%s -\n", __func__);
	exynos_ss_printk("%s:state %d: active %d:-\n", __func__,
				dsim->state, pm_runtime_active(dsim->dev));

	return ret;
}

static int dsim_exit_ulps(struct dsim_device *dsim)
{
	int ret = 0;

	DISP_SS_EVENT_START();
	dsim_dbg("%s +\n", __func__);
	exynos_ss_printk("%s:state %d: active %d:+\n", __func__,
				dsim->state, pm_runtime_active(dsim->dev));

	if (dsim->state != DSIM_STATE_ULPS) {
		ret = -EBUSY;
		goto err;
	}

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_get_sync(dsim->dev);
#else
	dsim_runtime_resume(dsim->dev);
#endif

	/* DPHY power on */
	dsim_d_phy_onoff(dsim, 1);

	dsim_reg_init(dsim->id, &dsim->lcd_info, dsim->data_lane_cnt);

	dsim_reg_enable_clocks(dsim->id, &dsim->clks_param,
			DSIM_LANE_CLOCK | dsim->data_lane);

	clk_prepare_enable(dsim->res.dphy_esc);
	clk_prepare_enable(dsim->res.dphy_byte);

	dsim_reg_set_lanes(dsim->id, DSIM_LANE_CLOCK | dsim->data_lane, 1);

	/* try to exit ULPS mode. The sequence is depends on DDI type */
	ret = dsim_set_ulps_by_ddi(dsim, 0);
	if (ret < 0) {
		dsim_info("%s: failed to exit ULPS: %d", __func__, ret);
		dsim_dump(dsim);
	}

	dsim_reg_set_hs_clock(dsim->id, &dsim->lcd_info, 1);

	enable_irq(dsim->irq);

	/* enable interrupts */
	dsim_reg_set_int(dsim->id, 1);

	dsim->state = DSIM_STATE_HSCLKEN;
	DISP_SS_EVENT_LOG(DISP_EVT_EXIT_ULPS, &dsim->sd, start);
err:
	dsim_dbg("%s -\n", __func__);
	exynos_ss_printk("%s:state %d: active %d:-\n", __func__,
				dsim->state, pm_runtime_active(dsim->dev));

	return 0;
}

static struct dsim_device *sd_to_dsim(struct v4l2_subdev *sd)
{
	return container_of(sd, struct dsim_device, sd);
}

static int dsim_s_stream(struct v4l2_subdev *sd, int enable)
{
        struct dsim_device *dsim = sd_to_dsim(sd);
        int ret = 0;

        switch (enable) {
        case DSIM_REQ_POWER_OFF:
                ret = dsim_disable(dsim);
                break;
        case DSIM_REQ_POWER_ON:
                ret = dsim_enable(dsim);
                break;
#ifdef CONFIG_LCD_DOZE_MODE
        case DSIM_REQ_DOZE_MODE:
                dsim_info("decon: dsim_doze_enable\n");
                ret = dsim_doze_enable(dsim);
                break;
        case DSIM_REQ_DOZE_SUSPEND:
                dsim_info("decon: dsim_doze_suspend\n");
                ret = dsim_doze_suspend(dsim);
                break;
#endif
        }

        return ret;

}

static long dsim_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct dsim_device *dsim = sd_to_dsim(sd);
	int ret = 0;

	switch (cmd) {
	case DSIM_IOC_GET_LCD_INFO:
		v4l2_set_subdev_hostdata(sd, &dsim->lcd_info);
		break;
	case DSIM_IOC_ENTER_ULPS:
		if ((unsigned long)arg)
			ret = dsim_enter_ulps(dsim);
		else
			ret = dsim_exit_ulps(dsim);
		break;
	case DSIM_IOC_LCD_OFF:
		ret = dsim_set_panel_power(dsim, 0);
		break;

#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	case DSIM_IOC_PKT_GO_ENABLE:
		dsim_pkt_go_enable(dsim, true);
		break;

	case DSIM_IOC_PKT_GO_DISABLE:
		dsim_pkt_go_enable(dsim, false);
		break;

	case DSIM_IOC_PKT_GO_READY:
		dsim_pkt_go_ready(dsim);
		break;
#endif
#ifdef CONFIG_FB_WINDOW_UPDATE
	case DSIM_IOC_PARTIAL_CMD:
		ret = dsim_partial_area_command(dsim, arg);
		break;
	case DSIM_IOC_SET_PORCH:
		dsim_reg_set_porch(dsim->id, (struct decon_lcd *)v4l2_get_subdev_hostdata(sd));
		break;
#endif
	case DSIM_IOC_DUMP:
		dsim_dump(dsim);
		break;
	case DSIM_IOC_VSYNC:
		ret = 0;
		if ((unsigned long)arg)
			dsim_enable_interrupts(dsim);
		else
			dsim_disable_interrupts(dsim);
		break;
	default:
		dev_err(dsim->dev, "unsupported ioctl");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_subdev_core_ops dsim_sd_core_ops = {
	.ioctl = dsim_ioctl,
};

static const struct v4l2_subdev_video_ops dsim_sd_video_ops = {
	.s_stream = dsim_s_stream,
};

static const struct v4l2_subdev_ops dsim_subdev_ops = {
	.core = &dsim_sd_core_ops,
	.video = &dsim_sd_video_ops,
};

static int dsim_link_setup(struct media_entity *entity,
			      const struct media_pad *local,
			      const struct media_pad *remote, u32 flags)
{
	if (flags & MEDIA_LNK_FL_ENABLED)
		dev_info(NULL, "Link is enabled\n");
	else
		dev_info(NULL, "Link is disabled\n");

	return 0;
}

static const struct media_entity_operations dsim_entity_ops = {
	.link_setup = dsim_link_setup,
};

static int dsim_register_entity(struct dsim_device *dsim)
{
	struct v4l2_subdev *sd = &dsim->sd;
	struct v4l2_device *v4l2_dev;
	struct device *dev = dsim->dev;
	struct media_pad *pads = &dsim->pad;
	struct media_entity *me = &sd->entity;
	struct exynos_md *md;
	int ret;

	v4l2_subdev_init(sd, &dsim_subdev_ops);
	sd->owner = THIS_MODULE;
	snprintf(sd->name, sizeof(sd->name), "exynos-mipi-dsi%d-subdev", dsim->id);

	dev_set_drvdata(dev, sd);
	pads[DSIM_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	me->ops = &dsim_entity_ops;
	ret = media_entity_init(me, DSIM_PADS_NUM, pads, 0);
	if (ret) {
		dev_err(dev, "failed to initialize media entity\n");
		return ret;
	}

	md = (struct exynos_md *)module_name_to_driver_data(MDEV_MODULE_NAME);
	if (!md) {
		dev_err(dev, "failed to get output media device\n");
		return -ENODEV;
	}

	v4l2_dev = &md->v4l2_dev;

	ret = v4l2_device_register_subdev(v4l2_dev, sd);
	if (ret) {
		dev_err(dev, "failed to register HDMI subdev\n");
		return ret;
	}

	/* 0: DSIM_0, 1: DSIM_1 */
	md->dsim_sd[dsim->id] = &dsim->sd;

	return 0;
}

static ssize_t dsim_rw_test_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t decon_rw_test_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long cmd;
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct dsim_device *dsim = sd_to_dsim(sd);

	ret = kstrtoul(buf, 0, &cmd);
	if (ret)
		return ret;

	ret = -ENXIO;

	if (cmd == 0x1) {
		dsim_info("Dsim is trying to do write test\n");
		dsim_write_test(dsim);
	} else {
		dsim_info("Dsim is trying to do read test\n");
		dsim_read_test(dsim);
	}

	return ret;
}
static DEVICE_ATTR(rw_test, 0644, dsim_rw_test_show, decon_rw_test_store);

int dsim_create_rw_test_sysfs(struct dsim_device *dsim)
{
	int ret = 0;

	ret = device_create_file(dsim->dev, &dev_attr_rw_test);
	if (ret) {
		dsim_err("failed to create read & write test sysfs\n");
	}

	return ret;
}

static int dsim_parse_lcd_info(struct dsim_device *dsim)
{
	u32 res[3];
	struct device_node *node;

	node = of_parse_phandle(dsim->dev->of_node, "lcd_info", 0);
	dsim_info("%s is founded\n", of_node_full_name(node));

	of_property_read_u32(node, "mode", &dsim->lcd_info.mode);
	dsim_dbg("%s mode\n", dsim->lcd_info.mode ? "command" : "video");

	of_property_read_u32_array(node, "resolution", res, 2);
	dsim->lcd_info.xres = res[0];
	dsim->lcd_info.yres = res[1];
	dsim_info("LCD resolution: xres(%d), yres(%d)\n", res[0], res[1]);

	of_property_read_u32_array(node, "size", res, 2);
	dsim->lcd_info.width = res[0];
	dsim->lcd_info.height = res[1];
	dsim_dbg("LCD size: width(%d), height(%d)\n", res[0], res[1]);

	of_property_read_u32(node, "timing,refresh", &dsim->lcd_info.fps);
	dsim_dbg("LCD refresh rate(%d)\n", dsim->lcd_info.fps);

	of_property_read_u32_array(node, "timing,h-porch", res, 3);
	dsim->lcd_info.hbp = res[0];
	dsim->lcd_info.hfp = res[1];
	dsim->lcd_info.hsa = res[2];
	dsim_dbg("hbp(%d), hfp(%d), hsa(%d)\n", res[0], res[1], res[2]);

	of_property_read_u32_array(node, "timing,v-porch", res, 3);
	dsim->lcd_info.vbp = res[0];
	dsim->lcd_info.vfp = res[1];
	dsim->lcd_info.vsa = res[2];
	dsim_dbg("vbp(%d), vfp(%d), vsa(%d)\n", res[0], res[1], res[2]);

	of_property_read_u32(node, "timing,dsi-hs-clk", &dsim->lcd_info.hs_clk);
	dsim->clks_param.clks.hs_clk = dsim->lcd_info.hs_clk;
	dsim_dbg("requested hs clock(%d)\n", dsim->lcd_info.hs_clk);

	of_property_read_u32(node, "timing,dsi-escape-clk", &dsim->lcd_info.esc_clk);
	dsim->clks_param.clks.esc_clk = dsim->lcd_info.esc_clk;
	dsim_dbg("requested escape clock(%d)\n", dsim->lcd_info.esc_clk);

	of_property_read_u32(node, "mic", &dsim->lcd_info.mic_enabled);
	dsim_info("mic is %s\n", dsim->lcd_info.mic_enabled ? "enabled" : "disabled");

	of_property_read_u32(node, "mic_ver", &dsim->lcd_info.mic_ver);
	dsim_dbg("mic version(%d)\n", dsim->lcd_info.mic_ver);

	of_property_read_u32(node, "type_of_ddi", &dsim->lcd_info.ddi_type);
	dsim_info("ddi type(%d)\n", dsim->lcd_info.ddi_type);

	return 0;
}

static int dsim_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct dsim_device *dsim = NULL;

	dsim = devm_kzalloc(dev, sizeof(struct dsim_device), GFP_KERNEL);
	if (!dsim) {
		dev_err(dev, "failed to allocate dsim device.\n");
		return -ENOMEM;
	}

	dsim->id = of_alias_get_id(dev->of_node, "dsim");
	dsim_info("dsim(%d) probe start..\n", dsim->id);

	if (!dsim->id)
		dsim0_for_decon = dsim;
	else
		dsim1_for_decon = dsim;

	dsim->dev = &pdev->dev;

	dsim_parse_lcd_info(dsim);
	ret = dsim_panel_ops_init(dsim);
	if (ret) {
		dsim_err("%s : failed to set panel ops\n", __func__);
		goto fail_free;
	}

	call_panel_ops(dsim, early_probe, dsim);

	dsim_get_gpios(dsim);

	dsim_get_clocks(dsim);
	spin_lock_init(&dsim->slock);

	of_property_read_u32(dev->of_node, "data_lane_cnt", &dsim->data_lane_cnt);
	dev_info(dev, "using data lane count(%d)\n", dsim->data_lane_cnt);

	/* added */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "failed to get resource");
		ret = -ENOENT;
		goto fail_clock;
	}

	dsim_info("res: start(0x%x), end(0x%x)\n", (u32)res->start, (u32)res->end);
	dsim->reg_base = devm_request_and_ioremap(dev, res);
	if (!dsim->reg_base) {
		dev_err(&pdev->dev, "mipi-dsi: failed to remap io region\n");
		ret = -EINVAL;
		goto fail_clock;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dev_err(dev, "failed to get resource");
		ret = -EINVAL;
		goto fail_clock;
	}

	ret = dsim_register_entity(dsim);
	if (ret)
		goto fail_irq;

	dsim->timing.bps = 0;

	mutex_init(&dsim_rd_wr_mutex);
	mutex_init(&dsim->irq_lock);

	pm_runtime_enable(dev);

	/* set using lanes */
	switch (dsim->data_lane_cnt) {
	case 1:
		dsim->data_lane = DSIM_LANE_DATA0;
		break;
	case 2:
		dsim->data_lane = DSIM_LANE_DATA0 | DSIM_LANE_DATA1;
		break;
	case 3:
		dsim->data_lane = DSIM_LANE_DATA0 | DSIM_LANE_DATA1 |
			DSIM_LANE_DATA2;
		break;
	case 4:
		dsim->data_lane = DSIM_LANE_DATA0 | DSIM_LANE_DATA1 |
			DSIM_LANE_DATA2 | DSIM_LANE_DATA3;
		break;
	default:
		dev_info(dsim->dev, "data lane is invalid.\n");
		ret = -EINVAL;
		goto fail_mutex;
	};

	dsim->state = DSIM_STATE_SUSPEND;

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_get_sync(dsim->dev);
#else
	dsim_runtime_resume(dsim->dev);
#endif

	dsim->irq = res->start;
	ret = devm_request_irq(dev, res->start,
			dsim_interrupt_handler, 0, pdev->name, dsim);
	if (ret) {
		dev_err(dev, "failed to install irq\n");
		goto fail_irq;
	}

	dsim_reg_prepare_clocks(&dsim->clks_param);

	/* DSIM does not need to start, if DSIM is already
	 * becomes HS status (enabled on LCD_ON_UBOOT) */
	if (dsim_reg_is_hs_clk_ready(dsim->id)) {
		/* Display power on: required to maintain ref_count of regulators */
		dsim_set_panel_power(dsim, 1);

		/* DPHY power on: it is required to maintain ref_count */
		dsim_d_phy_onoff(dsim, 1);

		if (dsim->lcd_info.mode != DECON_VIDEO_MODE)
			dsim_reg_init_probe(dsim->id, &dsim->lcd_info, dsim->data_lane_cnt);

		clk_prepare_enable(dsim->res.dphy_esc);
		clk_prepare_enable(dsim->res.dphy_byte);

		if (dsim->lcd_info.mode == DECON_VIDEO_MODE)
			goto dsim_init_done;

		ret = dsim_reg_set_lanes(dsim->id, DSIM_LANE_CLOCK | dsim->data_lane, 1);
		if (ret) {
			dsim_err("%s : failed to set lanes\n" , __func__);
			goto fail_mutex;
		}

		ret = dsim_reg_set_hs_clock(dsim->id, &dsim->lcd_info, 1);
		if (ret) {
			dsim_err("%s : failed to set hs clock\n", __func__);
			goto fail_mutex;
		}
		dsim_reg_set_int(dsim->id, 1);

		goto dsim_init_done;
	}

	ret = dsim_set_panel_power(dsim, 1);
	if (ret) {
		dsim_err("%s : failed to panel power\n", __func__);
		goto fail_mutex;
	}
	/* DPHY power on */
	dsim_d_phy_onoff(dsim, 1);

	dsim_reg_init(dsim->id, &dsim->lcd_info, dsim->data_lane_cnt);

	dsim_reg_enable_clocks(dsim->id, &dsim->clks_param,
			DSIM_LANE_CLOCK | dsim->data_lane);

	clk_prepare_enable(dsim->res.dphy_esc);
	clk_prepare_enable(dsim->res.dphy_byte);

	ret = dsim_reg_set_lanes(dsim->id, DSIM_LANE_CLOCK | dsim->data_lane, 1);
	if (ret) {
		dsim_err("%s : failed dsim set lane\n", __func__);
		goto fail_mutex;
	}
	ret = dsim_reset_panel(dsim);
	if (ret) {
		dsim_err("%s : failed dsim reset panel\n", __func__);
		goto fail_mutex;
	}
	ret = dsim_reg_set_hs_clock(dsim->id, &dsim->lcd_info, 1);
	if (ret) {
		dsim_err("%s : failed dsim hs clock\n", __func__);
		goto fail_mutex;
	}
	/* enable interrupts */
	dsim_reg_set_int(dsim->id, 1);

dsim_init_done:

	dsim->state = DSIM_STATE_HSCLKEN;

	call_panel_ops(dsim, probe, dsim);
	/* TODO: displayon is moved to decon probe only in case of lcd on probe */
	/* dsim->panel_ops->displayon(dsim); */

	dsim_clocks_info(dsim);

	dsim_create_rw_test_sysfs(dsim);

	dev_info(dev, "mipi-dsi driver(%s mode) has been probed.\n",
		dsim->lcd_info.mode == DECON_MIPI_COMMAND_MODE ? "CMD" : "VIDEO");

	return 0;

fail_mutex:
	mutex_destroy(&dsim_rd_wr_mutex);

fail_irq:
	release_resource(res);

fail_clock:
	dsim_put_clocks(dsim);

fail_free:
	kfree(dsim);

	dsim_err("%s : dsim prove failed\n", __func__);
	return ret;
}

static int dsim_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct dsim_device *dsim = sd_to_dsim(sd);

	pm_runtime_disable(dev);
	dsim_put_clocks(dsim);
	mutex_destroy(&dsim_rd_wr_mutex);
	kfree(dsim);
	dev_info(dev, "mipi-dsi driver removed\n");

	return 0;
}
static const struct of_device_id dsim_match[] = {
	{ .compatible = "samsung,exynos5-mipi-dsi" },
	{},
};
MODULE_DEVICE_TABLE(of, dsim_match);

static struct platform_device_id dsim_ids[] = {
	{
		.name		= "exynos-mipi-dsi"
	},
	{},
};
MODULE_DEVICE_TABLE(platform, dsim_ids);

static int dsim_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct dsim_device *dsim = sd_to_dsim(sd);

	DISP_SS_EVENT_LOG(DISP_EVT_DSIM_SUSPEND, sd, ktime_set(0, 0));
	dsim_dbg("%s +\n", __func__);

	clk_disable_unprepare(dsim->res.pclk);
	clk_disable_unprepare(dsim->res.rgb_vclk0);
	clk_disable_unprepare(dsim->res.pclk_disp);

	clk_disable_unprepare(dsim->res.dphy_esc);
	clk_disable_unprepare(dsim->res.dphy_byte);

	dsim_dbg("%s -\n", __func__);
	return 0;
}

static int dsim_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct dsim_device *dsim = sd_to_dsim(sd);

	DISP_SS_EVENT_LOG(DISP_EVT_DSIM_RESUME, sd, ktime_set(0, 0));
	dsim_dbg("%s: +\n", __func__);

	clk_prepare_enable(dsim->res.pclk);
	clk_prepare_enable(dsim->res.rgb_vclk0);
	clk_prepare_enable(dsim->res.pclk_disp);

	dsim_dbg("%s -\n", __func__);
	return 0;
}

static const struct dev_pm_ops dsim_pm_ops = {
	.runtime_suspend	= dsim_runtime_suspend,
	.runtime_resume		= dsim_runtime_resume,
};

static struct platform_driver dsim_driver __refdata = {
	.probe			= dsim_probe,
	.remove			= dsim_remove,
	.id_table		= dsim_ids,
	.driver = {
		.name		= "exynos-mipi-dsi",
		.owner		= THIS_MODULE,
		.pm		= &dsim_pm_ops,
		.of_match_table	= of_match_ptr(dsim_match),
	}
};

static int __init dsim_init(void)
{
	int ret = platform_driver_register(&dsim_driver);
	if (ret)
		printk(KERN_ERR "mipi_dsi driver register failed\n");

	return ret;
}
late_initcall(dsim_init);

static void __exit dsim_exit(void)
{
	platform_driver_unregister(&dsim_driver);
}

module_exit(dsim_exit);
MODULE_AUTHOR("Jiun Yu <jiun.yu@samsung.com>");
MODULE_DESCRIPTION("Samusung MIPI-DSI driver");
