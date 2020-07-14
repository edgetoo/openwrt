// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for MikroTik RouterBoot hard config.
 *
 * Copyright (C) 2020 Thibaut VARÈNE <hacks+kernel@slashdirt.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This driver exposes the data encoded in the "hard_config" flash segment of
 * MikroTik RouterBOARDs devices. It presents the data in a sysfs folder
 * named "hard_config". The WLAN calibration data is available on demand via
 * the 'wlan_data' sysfs file in that folder.
 *
 * This driver permanently allocates a chunk of RAM as large as the hard_config
 * MTD partition, although it is technically possible to operate entirely from
 * the MTD device without using a local buffer (except when requesting WLAN
 * calibration data), at the cost of a performance penalty.
 *
 * Note: PAGE_SIZE is assumed to be >= 4K, hence the device attribute show
 * routines need not check for output overflow.
 *
 * Some constant defines extracted from routerboot.{c,h} by Gabor Juhos
 * <juhosg@openwrt.org>
 */

#include <linux/types.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/bitops.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <linux/sysfs.h>
#include <linux/lzo.h>

#include "routerboot.h"

#define RB_HARDCONFIG_VER		"0.05"
#define RB_HC_PR_PFX			"[rb_hardconfig] "

/* ID values for hardware settings */
#define RB_ID_FLASH_INFO		0x03
#define RB_ID_MAC_ADDRESS_PACK		0x04
#define RB_ID_BOARD_PRODUCT_CODE	0x05
#define RB_ID_BIOS_VERSION		0x06
#define RB_ID_SDRAM_TIMINGS		0x08
#define RB_ID_DEVICE_TIMINGS		0x09
#define RB_ID_SOFTWARE_ID		0x0A
#define RB_ID_SERIAL_NUMBER		0x0B
#define RB_ID_MEMORY_SIZE		0x0D
#define RB_ID_MAC_ADDRESS_COUNT		0x0E
#define RB_ID_HW_OPTIONS		0x15
#define RB_ID_WLAN_DATA			0x16
#define RB_ID_BOARD_IDENTIFIER		0x17
#define RB_ID_PRODUCT_NAME		0x21
#define RB_ID_DEFCONF			0x26
#define RB_ID_BOARD_REVISION		0x27

/* Bit definitions for hardware options */
#define RB_HW_OPT_NO_UART		BIT(0)
#define RB_HW_OPT_HAS_VOLTAGE		BIT(1)
#define RB_HW_OPT_HAS_USB		BIT(2)
#define RB_HW_OPT_HAS_ATTINY		BIT(3)
#define RB_HW_OPT_PULSE_DUTY_CYCLE	BIT(9)
#define RB_HW_OPT_NO_NAND		BIT(14)
#define RB_HW_OPT_HAS_LCD		BIT(15)
#define RB_HW_OPT_HAS_POE_OUT		BIT(16)
#define RB_HW_OPT_HAS_uSD		BIT(17)
#define RB_HW_OPT_HAS_SIM		BIT(18)
#define RB_HW_OPT_HAS_SFP		BIT(20)
#define RB_HW_OPT_HAS_WIFI		BIT(21)
#define RB_HW_OPT_HAS_TS_FOR_ADC	BIT(22)
#define RB_HW_OPT_HAS_PLC		BIT(29)

static struct kobject *hc_kobj;
static u8 *hc_buf;		// ro buffer after init(): no locking required
static size_t hc_buflen;

/*
 * For LZOR style WLAN data unpacking.
 * This binary blob is prepended to the data encoded on some devices as
 * RB_ID_WLAN_DATA, the result is then first decompressed with LZO, and then
 * finally RLE-decoded.
 * This binary blob has been extracted from RouterOS by
 * https://forum.openwrt.org/u/ius
 */
static const u8 hc_lzor_prefix[] = {
	0x00, 0x05, 0x4c, 0x4c, 0x44, 0x00, 0x34, 0xfe,
	0xfe, 0x34, 0x11, 0x3c, 0x1e, 0x3c, 0x2e, 0x3c,
	0x4c, 0x34, 0x00, 0x52, 0x62, 0x92, 0xa2, 0xb2,
	0xc3, 0x2a, 0x14, 0x00, 0x00, 0x05, 0xfe, 0x6a,
	0x3c, 0x16, 0x32, 0x16, 0x11, 0x1e, 0x12, 0x46,
	0x32, 0x46, 0x11, 0x4e, 0x12, 0x36, 0x32, 0x36,
	0x11, 0x3e, 0x12, 0x5a, 0x9a, 0x64, 0x00, 0x04,
	0xfe, 0x10, 0x3c, 0x00, 0x01, 0x00, 0x00, 0x28,
	0x0c, 0x00, 0x0f, 0xfe, 0x14, 0x00, 0x24, 0x24,
	0x23, 0x24, 0x24, 0x23, 0x25, 0x22, 0x21, 0x21,
	0x23, 0x22, 0x21, 0x22, 0x21, 0x2d, 0x38, 0x00,
	0x0c, 0x25, 0x25, 0x24, 0x25, 0x25, 0x24, 0x23,
	0x22, 0x21, 0x20, 0x23, 0x21, 0x21, 0x22, 0x21,
	0x2d, 0x38, 0x00, 0x28, 0xb0, 0x00, 0x00, 0x22,
	0x00, 0x00, 0xc0, 0xfe, 0x03, 0x00, 0xc0, 0x00,
	0x62, 0xff, 0x62, 0xff, 0xfe, 0x06, 0x00, 0xbb,
	0xff, 0xba, 0xff, 0xfe, 0x08, 0x00, 0x9e, 0xff,
	0xfe, 0x0a, 0x00, 0x53, 0xff, 0xfe, 0x02, 0x00,
	0x20, 0xff, 0xb1, 0xfe, 0xfe, 0xb2, 0xfe, 0xfe,
	0xed, 0xfe, 0xfe, 0xfe, 0x04, 0x00, 0x3a, 0xff,
	0x3a, 0xff, 0xde, 0xfd, 0x5f, 0x04, 0x33, 0xff,
	0x4c, 0x74, 0x03, 0x05, 0x05, 0xff, 0x6d, 0xfe,
	0xfe, 0x6d, 0xfe, 0xfe, 0xaf, 0x08, 0x63, 0xff,
	0x64, 0x6f, 0x08, 0xac, 0xff, 0xbf, 0x6d, 0x08,
	0x7a, 0x6d, 0x08, 0x96, 0x74, 0x04, 0x00, 0x08,
	0x79, 0xff, 0xda, 0xfe, 0xfe, 0xdb, 0xfe, 0xfe,
	0x56, 0xff, 0xfe, 0x04, 0x00, 0x5e, 0xff, 0x5e,
	0xff, 0x6c, 0xfe, 0xfe, 0xfe, 0x06, 0x00, 0x41,
	0xff, 0x7f, 0x74, 0x03, 0x00, 0x11, 0x44, 0xff,
	0xa9, 0xfe, 0xfe, 0xa9, 0xfe, 0xfe, 0xa5, 0x8f,
	0x01, 0x00, 0x08, 0x01, 0x01, 0x02, 0x04, 0x08,
	0x02, 0x04, 0x08, 0x08, 0x01, 0x01, 0xfe, 0x22,
	0x00, 0x4c, 0x60, 0x64, 0x8c, 0x90, 0xd0, 0xd4,
	0xd8, 0x5c, 0x10, 0x09, 0xd8, 0xff, 0xb0, 0xff,
	0x00, 0x00, 0xba, 0xff, 0x14, 0x00, 0xba, 0xff,
	0x64, 0x00, 0x00, 0x08, 0xfe, 0x06, 0x00, 0x74,
	0xff, 0x42, 0xff, 0xce, 0xff, 0x60, 0xff, 0x0a,
	0x00, 0xb4, 0x00, 0xa0, 0x00, 0xa0, 0xfe, 0x07,
	0x00, 0x0a, 0x00, 0xb0, 0xff, 0x96, 0x4d, 0x00,
	0x56, 0x57, 0x18, 0xa6, 0xff, 0x92, 0x70, 0x11,
	0x00, 0x12, 0x90, 0x90, 0x76, 0x5a, 0x54, 0x54,
	0x4c, 0x46, 0x38, 0x00, 0x10, 0x10, 0x08, 0xfe,
	0x05, 0x00, 0x38, 0x29, 0x25, 0x23, 0x22, 0x22,
	0x1f, 0x00, 0x00, 0x00, 0xf6, 0xe1, 0xdd, 0xf8,
	0xfe, 0x00, 0xfe, 0x15, 0x00, 0x00, 0xd0, 0x02,
	0x74, 0x02, 0x08, 0xf8, 0xe5, 0xde, 0x02, 0x04,
	0x04, 0xfd, 0x00, 0x00, 0x00, 0x07, 0x50, 0x2d,
	0x01, 0x90, 0x90, 0x76, 0x60, 0xb0, 0x07, 0x07,
	0x0c, 0x0c, 0x04, 0xfe, 0x05, 0x00, 0x66, 0x66,
	0x5a, 0x56, 0xbc, 0x01, 0x06, 0xfc, 0xfc, 0xf1,
	0xfe, 0x07, 0x00, 0x24, 0x95, 0x70, 0x64, 0x18,
	0x06, 0x2c, 0xff, 0xb5, 0xfe, 0xfe, 0xb5, 0xfe,
	0xfe, 0xe2, 0x8c, 0x24, 0x02, 0x2f, 0xff, 0x2f,
	0xff, 0xb4, 0x78, 0x02, 0x05, 0x73, 0xff, 0xed,
	0xfe, 0xfe, 0x4f, 0xff, 0x36, 0x74, 0x1e, 0x09,
	0x4f, 0xff, 0x50, 0xff, 0xfe, 0x16, 0x00, 0x70,
	0xac, 0x70, 0x8e, 0xac, 0x40, 0x0e, 0x01, 0x70,
	0x7f, 0x8e, 0xac, 0x6c, 0x00, 0x0b, 0xfe, 0x02,
	0x00, 0xfe, 0x0a, 0x2c, 0x2a, 0x2a, 0x28, 0x26,
	0x1e, 0x1e, 0xfe, 0x02, 0x20, 0x65, 0x20, 0x00,
	0x00, 0x05, 0x12, 0x00, 0x11, 0x1e, 0x11, 0x11,
	0x41, 0x1e, 0x41, 0x11, 0x31, 0x1e, 0x31, 0x11,
	0x70, 0x75, 0x7a, 0x7f, 0x84, 0x89, 0x8e, 0x93,
	0x98, 0x30, 0x20, 0x00, 0x02, 0x00, 0xfe, 0x06,
	0x3c, 0xbc, 0x32, 0x0c, 0x00, 0x00, 0x2a, 0x12,
	0x1e, 0x12, 0x2e, 0x12, 0xcc, 0x12, 0x11, 0x1a,
	0x1e, 0x1a, 0x2e, 0x1a, 0x4c, 0x10, 0x1e, 0x10,
	0x11, 0x18, 0x1e, 0x42, 0x1e, 0x42, 0x2e, 0x42,
	0xcc, 0x42, 0x11, 0x4a, 0x1e, 0x4a, 0x2e, 0x4a,
	0x4c, 0x40, 0x1e, 0x40, 0x11, 0x48, 0x1e, 0x32,
	0x1e, 0x32, 0x2e, 0x32, 0xcc, 0x32, 0x11, 0x3a,
	0x1e, 0x3a, 0x2e, 0x3a, 0x4c, 0x30, 0x1e, 0x30,
	0x11, 0x38, 0x1e, 0x27, 0x9a, 0x01, 0x9d, 0xa2,
	0x2f, 0x28, 0x00, 0x00, 0x46, 0xde, 0xc4, 0xbf,
	0xa6, 0x9d, 0x81, 0x7b, 0x5c, 0x61, 0x40, 0xc7,
	0xc0, 0xae, 0xa9, 0x8c, 0x83, 0x6a, 0x62, 0x50,
	0x3e, 0xce, 0xc2, 0xae, 0xa3, 0x8c, 0x7b, 0x6a,
	0x5a, 0x50, 0x35, 0xd7, 0xc2, 0xb7, 0xa4, 0x95,
	0x7e, 0x72, 0x5a, 0x59, 0x37, 0xfe, 0x02, 0xf8,
	0x8c, 0x95, 0x90, 0x8f, 0x00, 0xd7, 0xc0, 0xb7,
	0xa2, 0x95, 0x7b, 0x72, 0x56, 0x59, 0x32, 0xc7,
	0xc3, 0xae, 0xad, 0x8c, 0x85, 0x6a, 0x63, 0x50,
	0x3e, 0xce, 0xc3, 0xae, 0xa4, 0x8c, 0x7c, 0x6a,
	0x59, 0x50, 0x34, 0xd7, 0xc2, 0xb7, 0xa5, 0x95,
	0x7e, 0x72, 0x59, 0x59, 0x36, 0xfc, 0x05, 0x00,
	0x02, 0xce, 0xc5, 0xae, 0xa5, 0x95, 0x83, 0x72,
	0x5c, 0x59, 0x36, 0xbf, 0xc6, 0xa5, 0xab, 0x8c,
	0x8c, 0x6a, 0x67, 0x50, 0x41, 0x64, 0x07, 0x00,
	0x02, 0x95, 0x8c, 0x72, 0x65, 0x59, 0x3f, 0xce,
	0xc7, 0xae, 0xa8, 0x95, 0x86, 0x72, 0x5f, 0x59,
	0x39, 0xfe, 0x02, 0xf8, 0x8b, 0x7c, 0x0b, 0x09,
	0xb7, 0xc2, 0x9d, 0xa4, 0x83, 0x85, 0x6a, 0x6b,
	0x50, 0x44, 0xb7, 0xc1, 0x64, 0x01, 0x00, 0x06,
	0x61, 0x5d, 0x48, 0x3d, 0xae, 0xc4, 0x9d, 0xad,
	0x7b, 0x85, 0x61, 0x66, 0x48, 0x46, 0xae, 0xc3,
	0x95, 0xa3, 0x72, 0x7c, 0x59, 0x56, 0x38, 0x31,
	0x7c, 0x0b, 0x00, 0x0c, 0x96, 0x91, 0x8f, 0x00,
	0xb7, 0xc0, 0xa5, 0xab, 0x8c, 0x8a, 0x6a, 0x64,
	0x50, 0x3c, 0xb7, 0xc0, 0x9d, 0xa0, 0x83, 0x80,
	0x6a, 0x64, 0x50, 0x3d, 0xb7, 0xc5, 0x9d, 0xa5,
	0x83, 0x87, 0x6c, 0x08, 0x07, 0xae, 0xc0, 0x9d,
	0xa8, 0x83, 0x88, 0x6a, 0x6d, 0x50, 0x46, 0xfc,
	0x05, 0x00, 0x16, 0xbf, 0xc0, 0xa5, 0xa2, 0x8c,
	0x7f, 0x6a, 0x57, 0x50, 0x2f, 0xb7, 0xc7, 0xa5,
	0xb1, 0x8c, 0x8e, 0x72, 0x6d, 0x59, 0x45, 0xbf,
	0xc6, 0xa5, 0xa8, 0x8c, 0x87, 0x6a, 0x5f, 0x50,
	0x37, 0xbf, 0xc2, 0xa5, 0xa4, 0x8c, 0x83, 0x6a,
	0x5c, 0x50, 0x34, 0xbc, 0x05, 0x00, 0x0e, 0x90,
	0x00, 0xc7, 0xc2, 0xae, 0xaa, 0x95, 0x82, 0x7b,
	0x60, 0x61, 0x3f, 0xb7, 0xc6, 0xa5, 0xb1, 0x8c,
	0x8d, 0x72, 0x6b, 0x61, 0x51, 0xbf, 0xc4, 0xa5,
	0xa5, 0x8c, 0x82, 0x72, 0x61, 0x59, 0x39, 0x6c,
	0x26, 0x03, 0x95, 0x82, 0x7b, 0x61, 0x61, 0x40,
	0xfc, 0x05, 0x00, 0x00, 0x7e, 0xd7, 0xc3, 0xb7,
	0xa8, 0x9d, 0x80, 0x83, 0x5d, 0x6a, 0x3f, 0xbf,
	0xc7, 0xa5, 0xa8, 0x8c, 0x84, 0x72, 0x60, 0x61,
	0x46, 0xbf, 0xc2, 0xae, 0xb0, 0x9d, 0x92, 0x83,
	0x6f, 0x6a, 0x50, 0xd7, 0xc3, 0xb7, 0xa7, 0x9d,
	0x80, 0x83, 0x5e, 0x6a, 0x40, 0xfe, 0x02, 0xf8,
	0x8d, 0x96, 0x90, 0x90, 0xfe, 0x05, 0x00, 0x8a,
	0xc4, 0x63, 0xb8, 0x3c, 0xa6, 0x29, 0x97, 0x16,
	0x81, 0x84, 0xb7, 0x5b, 0xa9, 0x33, 0x94, 0x1e,
	0x83, 0x11, 0x70, 0xb8, 0xc2, 0x70, 0xb1, 0x4d,
	0xa3, 0x2a, 0x8d, 0x1b, 0x7b, 0xa8, 0xbc, 0x68,
	0xab, 0x47, 0x9d, 0x27, 0x87, 0x18, 0x75, 0xae,
	0xc6, 0x7d, 0xbb, 0x4d, 0xaa, 0x1c, 0x84, 0x11,
	0x72, 0xa3, 0xbb, 0x6e, 0xad, 0x3c, 0x97, 0x24,
	0x85, 0x16, 0x71, 0x80, 0xb2, 0x57, 0xa4, 0x30,
	0x8e, 0x1c, 0x7c, 0x10, 0x68, 0xbb, 0xbd, 0x75,
	0xac, 0x4f, 0x9e, 0x2b, 0x87, 0x1a, 0x76, 0x96,
	0xc5, 0x5e, 0xb5, 0x3e, 0xa5, 0x1f, 0x8c, 0x12,
	0x7a, 0xc1, 0xc6, 0x42, 0x9f, 0x27, 0x8c, 0x16,
	0x77, 0x0f, 0x67, 0x9d, 0xbc, 0x68, 0xad, 0x36,
	0x95, 0x20, 0x83, 0x11, 0x6d, 0x9b, 0xb8, 0x67,
	0xa8, 0x34, 0x90, 0x1f, 0x7c, 0x10, 0x67, 0x9e,
	0xc9, 0x6a, 0xbb, 0x37, 0xa4, 0x20, 0x90, 0x11,
	0x7b, 0xc6, 0xc8, 0x47, 0xa4, 0x2a, 0x90, 0x18,
	0x7b, 0x10, 0x6c, 0xae, 0xc4, 0x5d, 0xad, 0x37,
	0x9a, 0x1f, 0x85, 0x13, 0x75, 0x70, 0xad, 0x42,
	0x99, 0x25, 0x84, 0x17, 0x74, 0x0b, 0x56, 0x87,
	0xc8, 0x57, 0xb8, 0x2b, 0x9e, 0x19, 0x8a, 0x0d,
	0x74, 0xa7, 0xc8, 0x6e, 0xb9, 0x36, 0xa0, 0x1f,
	0x8b, 0x11, 0x75, 0x94, 0xbe, 0x4b, 0xa5, 0x2a,
	0x92, 0x18, 0x7c, 0x0f, 0x6b, 0xaf, 0xc0, 0x58,
	0xa8, 0x34, 0x94, 0x1d, 0x7d, 0x12, 0x6d, 0x82,
	0xc0, 0x52, 0xb0, 0x25, 0x94, 0x14, 0x7f, 0x0c,
	0x68, 0x84, 0xbf, 0x3e, 0xa4, 0x22, 0x8e, 0x10,
	0x76, 0x0b, 0x65, 0x88, 0xb6, 0x42, 0x9b, 0x26,
	0x87, 0x14, 0x70, 0x0c, 0x5f, 0xc5, 0xc2, 0x3e,
	0x97, 0x23, 0x83, 0x13, 0x6c, 0x0c, 0x5c, 0xb1,
	0xc9, 0x76, 0xbc, 0x4a, 0xaa, 0x20, 0x8d, 0x12,
	0x78, 0x93, 0xbf, 0x46, 0xa3, 0x26, 0x8d, 0x14,
	0x74, 0x0c, 0x62, 0xc8, 0xc4, 0x3b, 0x97, 0x21,
	0x82, 0x11, 0x6a, 0x0a, 0x59, 0xa3, 0xb9, 0x68,
	0xa9, 0x30, 0x8d, 0x1a, 0x78, 0x0f, 0x61, 0xa0,
	0xc9, 0x73, 0xbe, 0x50, 0xb1, 0x30, 0x9f, 0x14,
	0x80, 0x83, 0xb7, 0x3c, 0x9a, 0x20, 0x84, 0x0e,
	0x6a, 0x0a, 0x57, 0xac, 0xc2, 0x68, 0xb0, 0x2e,
	0x92, 0x19, 0x7c, 0x0d, 0x63, 0x93, 0xbe, 0x62,
	0xb0, 0x3c, 0x9e, 0x1a, 0x80, 0x0e, 0x6b, 0xbb,
	0x02, 0xa0, 0x02, 0xa0, 0x02, 0x6f, 0x00, 0x75,
	0x00, 0x75, 0x00, 0x00, 0x00, 0xad, 0x02, 0xb3,
	0x02, 0x6f, 0x00, 0x87, 0x00, 0x85, 0xfe, 0x03,
	0x00, 0xc2, 0x02, 0x82, 0x4d, 0x92, 0x6e, 0x4d,
	0xb1, 0xa8, 0x84, 0x01, 0x00, 0x07, 0x7e, 0x00,
	0xa8, 0x02, 0xa4, 0x02, 0xa4, 0x02, 0xa2, 0x00,
	0xa6, 0x00, 0xa6, 0x00, 0x00, 0x00, 0xb4, 0x02,
	0xb4, 0x02, 0x92, 0x00, 0x96, 0x00, 0x96, 0x46,
	0x04, 0xb0, 0x02, 0x64, 0x02, 0x0a, 0x8c, 0x00,
	0x90, 0x02, 0x98, 0x02, 0x98, 0x02, 0x0e, 0x01,
	0x11, 0x01, 0x11, 0x50, 0xc3, 0x08, 0x88, 0x02,
	0x88, 0x02, 0x19, 0x01, 0x02, 0x01, 0x02, 0x01,
	0xf3, 0x2d, 0x00, 0x00
};

/* Array of known hw_options bits with human-friendly parsing */
static struct hc_hwopt {
	const u32 bit;
	const char *str;
} const hc_hwopts[] = {
	{
		.bit = RB_HW_OPT_NO_UART,
		.str = "no UART\t\t",
	}, {
		.bit = RB_HW_OPT_HAS_VOLTAGE,
		.str = "has Vreg\t",
	}, {
		.bit = RB_HW_OPT_HAS_USB,
		.str = "has usb\t\t",
	}, {
		.bit = RB_HW_OPT_HAS_ATTINY,
		.str = "has ATtiny\t",
	}, {
		.bit = RB_HW_OPT_NO_NAND,
		.str = "no NAND\t\t",
	}, {
		.bit = RB_HW_OPT_HAS_LCD,
		.str = "has LCD\t\t",
	}, {
		.bit = RB_HW_OPT_HAS_POE_OUT,
		.str = "has POE out\t",
	}, {
		.bit = RB_HW_OPT_HAS_uSD,
		.str = "has MicroSD\t",
	}, {
		.bit = RB_HW_OPT_HAS_SIM,
		.str = "has SIM\t\t",
	}, {
		.bit = RB_HW_OPT_HAS_SFP,
		.str = "has SFP\t\t",
	}, {
		.bit = RB_HW_OPT_HAS_WIFI,
		.str = "has WiFi\t",
	}, {
		.bit = RB_HW_OPT_HAS_TS_FOR_ADC,
		.str = "has TS ADC\t",
	}, {
		.bit = RB_HW_OPT_HAS_PLC,
		.str = "has PLC\t\t",
	},
};

/*
 * The MAC is stored network-endian on all devices, in 2 32-bit segments:
 * <XX:XX:XX:XX> <XX:XX:00:00>. Kernel print has us covered.
 */
static ssize_t hc_tag_show_mac(const u8 *pld, u16 pld_len, char *buf)
{
	if (8 != pld_len)
		return -EINVAL;

	return sprintf(buf, "%pM\n", pld);
}

/*
 * Print HW options in a human readable way:
 * The raw number and in decoded form
 */
static ssize_t hc_tag_show_hwoptions(const u8 *pld, u16 pld_len, char *buf)
{
	char *out = buf;
	u32 data;	// cpu-endian
	int i;

	if (sizeof(data) != pld_len)
		return -EINVAL;

	data = *(u32 *)pld;
	out += sprintf(out, "raw\t\t: 0x%08x\n\n", data);

	for (i = 0; i < ARRAY_SIZE(hc_hwopts); i++)
		out += sprintf(out, "%s: %s\n", hc_hwopts[i].str,
			       (data & hc_hwopts[i].bit) ? "true" : "false");

	return out - buf;
}

static ssize_t hc_wlan_data_bin_read(struct file *filp, struct kobject *kobj,
				     struct bin_attribute *attr, char *buf,
				     loff_t off, size_t count);

static struct hc_wlan_attr {
	struct bin_attribute battr;
	u16 pld_ofs;
	u16 pld_len;
} hc_wlandata_battr = {
	.battr = __BIN_ATTR(wlan_data, S_IRUSR, hc_wlan_data_bin_read, NULL, 0),
};

static ssize_t hc_attr_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf);

/* Array of known tags to publish in sysfs */
static struct hc_attr {
	const u16 tag_id;
	ssize_t (* const tshow)(const u8 *pld, u16 pld_len, char *buf);
	struct kobj_attribute kattr;
	u16 pld_ofs;
	u16 pld_len;
} hc_attrs[] = {
	{
		.tag_id = RB_ID_FLASH_INFO,
		.tshow = routerboot_tag_show_u32s,
		.kattr = __ATTR(flash_info, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_MAC_ADDRESS_PACK,
		.tshow = hc_tag_show_mac,
		.kattr = __ATTR(mac_base, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_BOARD_PRODUCT_CODE,
		.tshow = routerboot_tag_show_string,
		.kattr = __ATTR(board_product_code, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_BIOS_VERSION,
		.tshow = routerboot_tag_show_string,
		.kattr = __ATTR(booter_version, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_SERIAL_NUMBER,
		.tshow = routerboot_tag_show_string,
		.kattr = __ATTR(board_serial, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_MEMORY_SIZE,
		.tshow = routerboot_tag_show_u32s,
		.kattr = __ATTR(mem_size, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_MAC_ADDRESS_COUNT,
		.tshow = routerboot_tag_show_u32s,
		.kattr = __ATTR(mac_count, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_HW_OPTIONS,
		.tshow = hc_tag_show_hwoptions,
		.kattr = __ATTR(hw_options, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_WLAN_DATA,
		.tshow = NULL,
	}, {
		.tag_id = RB_ID_BOARD_IDENTIFIER,
		.tshow = routerboot_tag_show_string,
		.kattr = __ATTR(board_identifier, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_PRODUCT_NAME,
		.tshow = routerboot_tag_show_string,
		.kattr = __ATTR(product_name, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_DEFCONF,
		.tshow = routerboot_tag_show_string,
		.kattr = __ATTR(defconf, S_IRUSR, hc_attr_show, NULL),
	}, {
		.tag_id = RB_ID_BOARD_REVISION,
		.tshow = routerboot_tag_show_string,
		.kattr = __ATTR(board_revision, S_IRUSR, hc_attr_show, NULL),
	}
};

/*
 * If the RB_ID_WLAN_DATA payload starts with RB_MAGIC_ERD, then past
 * that magic number the payload itself contains a routerboot tag node
 * locating the LZO-compressed calibration data at id 0x1.
 */
static int hc_wlan_data_unpack_erd(const u8 *inbuf, size_t inlen,
				   void *outbuf, size_t *outlen)
{
	u16 lzo_ofs, lzo_len;
	int ret;

	/* Find embedded tag */
	ret = routerboot_tag_find(inbuf, inlen, 0x1,	// always id 1
				  &lzo_ofs, &lzo_len);
	if (ret) {
		pr_debug(RB_HC_PR_PFX "ERD data not found\n");
		goto fail;
	}

	if (lzo_len > inlen) {
		pr_debug(RB_HC_PR_PFX "Invalid ERD data length\n");
		ret = -EINVAL;
		goto fail;
	}

	ret = lzo1x_decompress_safe(inbuf+lzo_ofs, lzo_len, outbuf, outlen);
	if (ret)
		pr_debug(RB_HC_PR_PFX "LZO decompression error (%d)\n", ret);

fail:
	return ret;
}

/*
 * If the RB_ID_WLAN_DATA payload starts with RB_MAGIC_LZOR, then past
 * that magic number is a payload that must be appended to the hc_lzor_prefix,
 * the resulting blob is LZO-compressed. In the LZO decompression result,
 * the RB_MAGIC_ERD magic number (aligned) must be located. Following that
 * magic, there is a routerboot tag node (id 0x1) locating the RLE-encoded
 * calibration data payload.
 */
static int hc_wlan_data_unpack_lzor(const u8 *inbuf, size_t inlen,
				    void *outbuf, size_t *outlen)
{
	u16 rle_ofs, rle_len;
	const u32 *needle;
	u8 *tempbuf;
	size_t templen, lzo_len;
	int ret;

	lzo_len = inlen + sizeof(hc_lzor_prefix);
	if (lzo_len > *outlen)
		return -EFBIG;

	/* Temporary buffer same size as the outbuf */
	templen = *outlen;
	tempbuf = kmalloc(templen, GFP_KERNEL);
	if (!outbuf)
		return -ENOMEM;

	/* Concatenate into the outbuf */
	memcpy(outbuf, hc_lzor_prefix, sizeof(hc_lzor_prefix));
	memcpy(outbuf + sizeof(hc_lzor_prefix), inbuf, inlen);

	/* LZO-decompress lzo_len bytes of outbuf into the tempbuf */
	ret = lzo1x_decompress_safe(outbuf, lzo_len, tempbuf, &templen);
	if (ret) {
		if (LZO_E_INPUT_NOT_CONSUMED == ret) {
			/*
			 * The tag length appears to always be aligned (probably
			 * because it is the "root" RB_ID_WLAN_DATA tag), thus
			 * the LZO payload may be padded, which can trigger a
			 * spurious error which we ignore here.
			 */
			pr_debug(RB_HC_PR_PFX "LZOR: LZO EOF before buffer end - this may be harmless\n");
		} else {
			pr_debug(RB_HC_PR_PFX "LZOR: LZO decompression error (%d)\n", ret);
			goto fail;
		}
	}

	/*
	 * Post decompression we have a blob (possibly byproduct of the lzo
	 * dictionary). We need to find RB_MAGIC_ERD. The magic number seems to
	 * be 32bit-aligned in the decompression output.
	 */
	needle = (const u32 *)tempbuf;
	while (RB_MAGIC_ERD != *needle++) {
		if ((u8 *)needle >= tempbuf+templen) {
			pr_debug(RB_HC_PR_PFX "LZOR: ERD magic not found\n");
			ret = -ENODATA;
			goto fail;
		}
	};
	templen -= (u8 *)needle - tempbuf;

	/* Past magic. Look for tag node */
	ret = routerboot_tag_find((u8 *)needle, templen, 0x1, &rle_ofs, &rle_len);
	if (ret) {
		pr_debug(RB_HC_PR_PFX "LZOR: RLE data not found\n");
		goto fail;
	}

	if (rle_len > templen) {
		pr_debug(RB_HC_PR_PFX "LZOR: Invalid RLE data length\n");
		ret = -EINVAL;
		goto fail;
	}

	/* RLE-decode tempbuf from needle back into the outbuf */
	ret = routerboot_rle_decode((u8 *)needle+rle_ofs, rle_len, outbuf, outlen);
	if (ret)
		pr_debug(RB_HC_PR_PFX "LZOR: RLE decoding error (%d)\n", ret);

fail:
	kfree(tempbuf);
	return ret;
}

static int hc_wlan_data_unpack(const size_t tofs, size_t tlen,
			       void *outbuf, size_t *outlen)
{
	const u8 *lbuf;
	u32 magic;
	int ret;

	/* Caller ensure tlen > 0. tofs is aligned */
	if ((tofs + tlen) > hc_buflen)
		return -EIO;

	lbuf = hc_buf + tofs;
	magic = *(u32 *)lbuf;

	ret = -ENODATA;
	switch (magic) {
	case RB_MAGIC_LZOR:
		/* Skip magic */
		lbuf += sizeof(magic);
		tlen -= sizeof(magic);
		ret = hc_wlan_data_unpack_lzor(lbuf, tlen, outbuf, outlen);
		break;
	case RB_MAGIC_ERD:
		/* Skip magic */
		lbuf += sizeof(magic);
		tlen -= sizeof(magic);
		ret = hc_wlan_data_unpack_erd(lbuf, tlen, outbuf, outlen);
		break;
	default:
		/*
		 * If the RB_ID_WLAN_DATA payload doesn't start with a
		 * magic number, the payload itself is the raw RLE-encoded
		 * calibration data.
		 */
		ret = routerboot_rle_decode(lbuf, tlen, outbuf, outlen);
		if (ret)
			pr_debug(RB_HC_PR_PFX "RLE decoding error (%d)\n", ret);
		break;
	}

	return ret;
}

static ssize_t hc_attr_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	const struct hc_attr *hc_attr;
	const u8 *pld;
	u16 pld_len;

	hc_attr = container_of(attr, typeof(*hc_attr), kattr);

	if (!hc_attr->pld_len)
		return -ENOENT;

	pld = hc_buf + hc_attr->pld_ofs;
	pld_len = hc_attr->pld_len;

	return hc_attr->tshow(pld, pld_len, buf);
}

/*
 * This function will allocate and free memory every time it is called. This
 * is not the fastest way to do this, but since the data is rarely read (mainly
 * at boot time to load wlan caldata), this makes it possible to save memory for
 * the system.
 */
static ssize_t hc_wlan_data_bin_read(struct file *filp, struct kobject *kobj,
				     struct bin_attribute *attr, char *buf,
				     loff_t off, size_t count)
{
	struct hc_wlan_attr *hc_wattr;
	size_t outlen;
	void *outbuf;
	int ret;

	hc_wattr = container_of(attr, typeof(*hc_wattr), battr);

	if (!hc_wattr->pld_len)
		return -ENOENT;

	outlen = RB_ART_SIZE;

	/* Don't bother unpacking if the source is already too large */
	if (hc_wattr->pld_len > outlen)
		return -EFBIG;

	outbuf = kmalloc(outlen, GFP_KERNEL);
	if (!outbuf)
		return -ENOMEM;

	ret = hc_wlan_data_unpack(hc_wattr->pld_ofs, hc_wattr->pld_len, outbuf, &outlen);
	if (ret) {
		kfree(outbuf);
		return ret;
	}

	if (off >= outlen) {
		kfree(outbuf);
		return 0;
	}

	if (off + count > outlen)
		count = outlen - off;

	memcpy(buf, outbuf + off, count);

	kfree(outbuf);
	return count;
}

int __init rb_hardconfig_init(struct kobject *rb_kobj)
{
	struct mtd_info *mtd;
	size_t bytes_read, buflen;
	const u8 *buf;
	int i, ret;
	u32 magic;

	hc_buf = NULL;
	hc_kobj = NULL;

	// TODO allow override
	mtd = get_mtd_device_nm(RB_MTD_HARD_CONFIG);
	if (IS_ERR(mtd))
		return -ENODEV;

	hc_buflen = mtd->size;
	hc_buf = kmalloc(hc_buflen, GFP_KERNEL);
	if (!hc_buf)
		return -ENOMEM;

	ret = mtd_read(mtd, 0, hc_buflen, &bytes_read, hc_buf);

	if (ret)
		goto fail;

	if (bytes_read != hc_buflen) {
		ret = -EIO;
		goto fail;
	}

	/* Check we have what we expect */
	magic = *(const u32 *)hc_buf;
	if (RB_MAGIC_HARD != magic) {
		ret = -EINVAL;
		goto fail;
	}

	/* Skip magic */
	buf = hc_buf + sizeof(magic);
	buflen = hc_buflen - sizeof(magic);

	/* Populate sysfs */
	ret = -ENOMEM;
	hc_kobj = kobject_create_and_add(RB_MTD_HARD_CONFIG, rb_kobj);
	if (!hc_kobj)
		goto fail;

	/* Locate and publish all known tags */
	for (i = 0; i < ARRAY_SIZE(hc_attrs); i++) {
		ret = routerboot_tag_find(buf, buflen, hc_attrs[i].tag_id,
					  &hc_attrs[i].pld_ofs, &hc_attrs[i].pld_len);
		if (ret) {
			hc_attrs[i].pld_ofs = hc_attrs[i].pld_len = 0;
			continue;
		}

		/* Account for skipped magic */
		hc_attrs[i].pld_ofs += sizeof(magic);

		/* Special case RB_ID_WLAN_DATA to prep and create the binary attribute */
		if ((RB_ID_WLAN_DATA == hc_attrs[i].tag_id) && hc_attrs[i].pld_len) {
			hc_wlandata_battr.pld_ofs = hc_attrs[i].pld_ofs;
			hc_wlandata_battr.pld_len = hc_attrs[i].pld_len;

			ret = sysfs_create_bin_file(hc_kobj, &hc_wlandata_battr.battr);
			if (ret)
				pr_warn(RB_HC_PR_PFX "Could not create %s sysfs entry (%d)\n",
				       hc_wlandata_battr.battr.attr.name, ret);
		}
		/* All other tags are published via standard attributes */
		else {
			ret = sysfs_create_file(hc_kobj, &hc_attrs[i].kattr.attr);
			if (ret)
				pr_warn(RB_HC_PR_PFX "Could not create %s sysfs entry (%d)\n",
				       hc_attrs[i].kattr.attr.name, ret);
		}
	}

	pr_info("MikroTik RouterBOARD hardware configuration sysfs driver v" RB_HARDCONFIG_VER "\n");

	return 0;

fail:
	kfree(hc_buf);
	hc_buf = NULL;
	return ret;
}

void __exit rb_hardconfig_exit(void)
{
	kobject_put(hc_kobj);
	kfree(hc_buf);
}
