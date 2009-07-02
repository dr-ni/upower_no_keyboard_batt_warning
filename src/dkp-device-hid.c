/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006-2008 Richard Hughes <richard@hughsie.com>
 *
 * Based on hid-ups.c: Copyright (c) 2001 Vojtech Pavlik <vojtech@ucw.cz>
 *                     Copyright (c) 2001 Paul Stewart <hiddev@wetlogic.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <math.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>
#include <devkit-gobject/devkit-gobject.h>

/* asm/types.h required for __s32 in linux/hiddev.h */
#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hiddev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "sysfs-utils.h"
#include "egg-debug.h"

#include "dkp-enum.h"
#include "dkp-device-hid.h"

#define DKP_DEVICE_HID_REFRESH_TIMEOUT			30l

#define DKP_DEVICE_HID_USAGE				0x840000
#define DKP_DEVICE_HID_SERIAL				0x8400fe
#define DKP_DEVICE_HID_CHEMISTRY			0x850089
#define DKP_DEVICE_HID_CAPACITY_MODE			0x85002c
#define DKP_DEVICE_HID_BATTERY_VOLTAGE			0x840030
#define DKP_DEVICE_HID_BELOW_RCL			0x840042
#define DKP_DEVICE_HID_SHUTDOWN_IMMINENT		0x840069
#define DKP_DEVICE_HID_PRODUCT				0x8400fe
#define DKP_DEVICE_HID_SERIAL_NUMBER			0x8400ff
#define DKP_DEVICE_HID_CHARGING				0x850044
#define DKP_DEVICE_HID_DISCHARGING 			0x850045
#define DKP_DEVICE_HID_REMAINING_CAPACITY		0x850066
#define DKP_DEVICE_HID_RUNTIME_TO_EMPTY			0x850068
#define DKP_DEVICE_HID_AC_PRESENT			0x8500d0
#define DKP_DEVICE_HID_BATTERY_PRESENT			0x8500d1
#define DKP_DEVICE_HID_DESIGN_CAPACITY			0x850083
#define DKP_DEVICE_HID_DEVICE_NAME			0x850088
#define DKP_DEVICE_HID_DEVICE_CHEMISTRY			0x850089
#define DKP_DEVICE_HID_RECHARGEABLE			0x85008b
#define DKP_DEVICE_HID_OEM_INFORMATION			0x85008f

#define DKP_DEVICE_HID_PAGE_GENERIC_DESKTOP		0x01
#define DKP_DEVICE_HID_PAGE_CONSUMER_PRODUCT		0x0c
#define DKP_DEVICE_HID_PAGE_USB_MONITOR			0x80
#define DKP_DEVICE_HID_PAGE_USB_ENUMERATED_VALUES	0x81
#define DKP_DEVICE_HID_PAGE_VESA_VIRTUAL_CONTROLS	0x82
#define DKP_DEVICE_HID_PAGE_RESERVED_MONITOR		0x83
#define DKP_DEVICE_HID_PAGE_POWER_DEVICE		0x84
#define DKP_DEVICE_HID_PAGE_BATTERY_SYSTEM		0x85

struct DkpDeviceHidPrivate
{
	guint			 poll_timer_id;
	int			 fd;
};

static void	dkp_device_hid_class_init	(DkpDeviceHidClass	*klass);

G_DEFINE_TYPE (DkpDeviceHid, dkp_device_hid, DKP_TYPE_DEVICE)
#define DKP_DEVICE_HID_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_HID, DkpDeviceHidPrivate))

static gboolean		 dkp_device_hid_refresh	 	(DkpDevice *device);

/**
 * dkp_device_hid_is_ups:
 **/
static gboolean
dkp_device_hid_is_ups (DkpDeviceHid *hid)
{
	guint i;
	int retval;
	gboolean ret = FALSE;
	struct hiddev_devinfo device_info;

	/* get device info */
	retval = ioctl (hid->priv->fd, HIDIOCGDEVINFO, &device_info);
	if (retval < 0) {
		egg_debug ("HIDIOCGDEVINFO failed: %s", strerror (errno));
		goto out;
	}

	/* can we use the hid device as a UPS? */
	for (i = 0; i < device_info.num_applications; i++) {
		retval = ioctl (hid->priv->fd, HIDIOCAPPLICATION, i);
		if (retval >> 16 == DKP_DEVICE_HID_PAGE_POWER_DEVICE) {
			ret = TRUE;
			goto out;
		}
	}
out:
	return ret;
}

/**
 * dkp_device_hid_poll:
 **/
static gboolean
dkp_device_hid_poll (DkpDeviceHid *hid)
{
	gboolean ret;
	DkpDevice *device = DKP_DEVICE (hid);

	egg_debug ("Polling: %s", dkp_device_get_object_path (device));
	ret = dkp_device_hid_refresh (device);
	if (ret)
		dkp_device_emit_changed (device);
	return TRUE;
}

/**
 * dkp_device_hid_get_string:
 **/
static const gchar *
dkp_device_hid_get_string (DkpDeviceHid *hid, int sindex)
{
	static struct hiddev_string_descriptor sdesc;

	/* nothing to get */
	if (sindex == 0)
		return "";

	sdesc.index = sindex;

	/* failed */
	if (ioctl (hid->priv->fd, HIDIOCGSTRING, &sdesc) < 0)
		return "";

	egg_debug ("value: '%s'", sdesc.value);
	return sdesc.value;
}

/**
 * dkp_device_hid_convert_device_technology:
 **/
static DkpDeviceTechnology
dkp_device_hid_convert_device_technology (const gchar *type)
{
	if (type == NULL)
		return DKP_DEVICE_TECHNOLOGY_UNKNOWN;
	if (strcasecmp (type, "pb") == 0 ||
	    strcasecmp (type, "pbac") == 0)
		return DKP_DEVICE_TECHNOLOGY_LEAD_ACID;
	return DKP_DEVICE_TECHNOLOGY_UNKNOWN;
}

/**
 * dkp_device_hid_set_obj:
 **/
static gboolean
dkp_device_hid_set_obj (DkpDeviceHid *hid, int code, int value)
{
	const gchar *type;
	gboolean ret = TRUE;
	DkpDevice *device = DKP_DEVICE (hid);

	switch (code) {
	case DKP_DEVICE_HID_REMAINING_CAPACITY:
		g_object_set (device, "percentage", value, NULL);
		break;
	case DKP_DEVICE_HID_RUNTIME_TO_EMPTY:
		g_object_set (device, "time-to-empty", value, NULL);
		break;
	case DKP_DEVICE_HID_CHARGING:
		if (value != 0)
			g_object_set (device, "state", DKP_DEVICE_STATE_CHARGING, NULL);
		break;
	case DKP_DEVICE_HID_DISCHARGING:
		if (value != 0)
			g_object_set (device, "state", DKP_DEVICE_STATE_DISCHARGING, NULL);
		break;
	case DKP_DEVICE_HID_BATTERY_PRESENT:
		g_object_set (device, "is-present", (value != 0), NULL);
		break;
	case DKP_DEVICE_HID_DEVICE_NAME:
		g_object_set (device, "device-name", dkp_device_hid_get_string (hid, value), NULL);
		break;
	case DKP_DEVICE_HID_CHEMISTRY:
		type = dkp_device_hid_get_string (hid, value);
		g_object_set (device, "technology", dkp_device_hid_convert_device_technology (type), NULL);
		break;
	case DKP_DEVICE_HID_RECHARGEABLE:
		g_object_set (device, "is-rechargeable", (value != 0), NULL);
		break;
	case DKP_DEVICE_HID_OEM_INFORMATION:
		g_object_set (device, "vendor", dkp_device_hid_get_string (hid, value), NULL);
		break;
	case DKP_DEVICE_HID_PRODUCT:
		g_object_set (device, "model", dkp_device_hid_get_string (hid, value), NULL);
		break;
	case DKP_DEVICE_HID_SERIAL_NUMBER:
		g_object_set (device, "serial", dkp_device_hid_get_string (hid, value), NULL);
		break;
	case DKP_DEVICE_HID_DESIGN_CAPACITY:
		g_object_set (device, "energy-full-design", value, NULL);
		break;
	default:
		ret = FALSE;
		break;
	}
	return ret;
}

/**
 * dkp_device_hid_get_all_data:
 **/
static gboolean
dkp_device_hid_get_all_data (DkpDeviceHid *hid)
{
	struct hiddev_report_info rinfo;
	struct hiddev_field_info finfo;
	struct hiddev_usage_ref uref;
	int rtype;
	guint i, j;

	/* get all results */
	for (rtype = HID_REPORT_TYPE_MIN; rtype <= HID_REPORT_TYPE_MAX; rtype++) {
		rinfo.report_type = rtype;
		rinfo.report_id = HID_REPORT_ID_FIRST;
		while (ioctl (hid->priv->fd, HIDIOCGREPORTINFO, &rinfo) >= 0) {
			for (i = 0; i < rinfo.num_fields; i++) { 
				memset (&finfo, 0, sizeof (finfo));
				finfo.report_type = rinfo.report_type;
				finfo.report_id = rinfo.report_id;
				finfo.field_index = i;
				ioctl (hid->priv->fd, HIDIOCGFIELDINFO, &finfo);

				memset (&uref, 0, sizeof (uref));
				for (j = 0; j < finfo.maxusage; j++) {
					uref.report_type = finfo.report_type;
					uref.report_id = finfo.report_id;
					uref.field_index = i;
					uref.usage_index = j;
					ioctl (hid->priv->fd, HIDIOCGUCODE, &uref);
					ioctl (hid->priv->fd, HIDIOCGUSAGE, &uref);

					/* process each */
					dkp_device_hid_set_obj (hid, uref.usage_code, uref.value);
				}
			}
			rinfo.report_id |= HID_REPORT_ID_NEXT;
		}
	}
	return TRUE;
}

/**
 * dkp_device_hid_coldplug:
 *
 * Return %TRUE on success, %FALSE if we failed to get data and should be removed
 **/
static gboolean
dkp_device_hid_coldplug (DkpDevice *device)
{
	DkpDeviceHid *hid = DKP_DEVICE_HID (device);
	DevkitDevice *d;
	gboolean ret = FALSE;
	const gchar *device_file;
	const gchar *type;
	const gchar *vendor;

	/* detect what kind of device we are */
	d = dkp_device_get_d (device);
	if (d == NULL)
		egg_error ("could not get device");

	/* get the type */
	type = devkit_device_get_property (d, "DKP_BATTERY_TYPE");
	if (type == NULL || g_strcmp0 (type, "ups") != 0)
		goto out;

	/* get the device file */
	device_file = devkit_device_get_device_file (d);
	if (device_file == NULL) {
		egg_debug ("could not get device file for HID device");
		goto out;
	}

	/* connect to the device */
	hid->priv->fd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (hid->priv->fd < 0) {
		egg_debug ("cannot open device file %s", device_file);
		goto out;
	}

	/* first check that we are an UPS */
	ret = dkp_device_hid_is_ups (hid);
	if (!ret) {
		egg_debug ("not a HID device: %s", device_file);
		goto out;
	}

	/* prefer DKP names */
	vendor = devkit_device_get_property (d, "DKP_VENDOR");
	if (vendor == NULL)
		vendor = devkit_device_get_property (d, "ID_VENDOR");

	/* hardcode some values */
	g_object_set (device,
		      "type", DKP_DEVICE_TYPE_UPS,
		      "is-rechargeable", TRUE,
		      "power-supply", TRUE,
		      "is-present", TRUE,
		      "vendor", vendor,
		      "has-history", TRUE,
		      "has-statistics", TRUE,
		      NULL);

	/* coldplug everything */
	dkp_device_hid_get_all_data (hid);

	/* coldplug */
	ret = dkp_device_hid_refresh (device);

out:
	return ret;
}

/**
 * dkp_device_hid_refresh:
 *
 * Return %TRUE on success, %FALSE if we failed to refresh or no data
 **/
static gboolean
dkp_device_hid_refresh (DkpDevice *device)
{
	gboolean set = FALSE;
	gboolean ret = FALSE;
	GTimeVal time;
	guint i;
	struct hiddev_event ev[64];
	int rd;
	DkpDeviceHid *hid = DKP_DEVICE_HID (device);

	/* reset time */
	g_get_current_time (&time);
	g_object_set (device, "update-time", (guint64) time.tv_sec, NULL);

	/* read any data -- it's okay if there's nothing as we are non-blocking */
	rd = read (hid->priv->fd, ev, sizeof (ev));
	if (rd < (int) sizeof (ev[0])) {
		ret = FALSE;
		goto out;
	}

	/* process each event */
	for (i=0; i < rd / sizeof (ev[0]); i++) {
		set = dkp_device_hid_set_obj (hid, ev[i].hid, ev[i].value);

		/* if only takes one match to make refresh a success */
		if (set)
			ret = TRUE;
	}
out:
	return ret;
}

/**
 * dkp_device_hid_init:
 **/
static void
dkp_device_hid_init (DkpDeviceHid *hid)
{
	hid->priv = DKP_DEVICE_HID_GET_PRIVATE (hid);
	hid->priv->fd = -1;
	hid->priv->poll_timer_id = g_timeout_add_seconds (DKP_DEVICE_HID_REFRESH_TIMEOUT,
							  (GSourceFunc) dkp_device_hid_poll, hid);
}

/**
 * dkp_device_hid_finalize:
 **/
static void
dkp_device_hid_finalize (GObject *object)
{
	DkpDeviceHid *hid;

	g_return_if_fail (object != NULL);
	g_return_if_fail (DKP_IS_HID (object));

	hid = DKP_DEVICE_HID (object);
	g_return_if_fail (hid->priv != NULL);

	if (hid->priv->fd > 0)
		close (hid->priv->fd);
	if (hid->priv->poll_timer_id > 0)
		g_source_remove (hid->priv->poll_timer_id);

	G_OBJECT_CLASS (dkp_device_hid_parent_class)->finalize (object);
}

/**
 * dkp_device_hid_class_init:
 **/
static void
dkp_device_hid_class_init (DkpDeviceHidClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	DkpDeviceClass *device_class = DKP_DEVICE_CLASS (klass);

	object_class->finalize = dkp_device_hid_finalize;
	device_class->coldplug = dkp_device_hid_coldplug;
	device_class->refresh = dkp_device_hid_refresh;

	g_type_class_add_private (klass, sizeof (DkpDeviceHidPrivate));
}

/**
 * dkp_device_hid_new:
 **/
DkpDeviceHid *
dkp_device_hid_new (void)
{
	return g_object_new (DKP_TYPE_HID, NULL);
}

