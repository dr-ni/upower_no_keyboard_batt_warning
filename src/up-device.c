/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 David Zeuthen <davidz@redhat.com>
 * Copyright (C) 2008-2009 Richard Hughes <richard@hughsie.com>
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

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>

#include "up-native.h"
#include "up-device.h"
#include "up-history.h"
#include "up-history-item.h"
#include "up-stats-item.h"

struct UpDevicePrivate
{
	UpDaemon		*daemon;
	UpHistory		*history;
	GObject			*native;
	gboolean		 has_ever_refresh;
};

G_DEFINE_TYPE (UpDevice, up_device, UP_TYPE_EXPORTED_DEVICE_SKELETON)
#define UP_DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), UP_TYPE_DEVICE, UpDevicePrivate))

#define UP_DEVICES_DBUS_PATH "/org/freedesktop/UPower/devices"

/* This needs to be called when one of those properties changes:
 * state
 * power_supply
 * percentage
 * time_to_empty
 *
 * type should not change for non-display devices
 */
static void
update_warning_level (UpDevice *device)
{
	UpDeviceLevel warning_level;
	UpExportedDevice *skeleton = UP_EXPORTED_DEVICE (device);

        /* UN: Disable warning notifications for keyboard with rechargeable batteries */
        int type = up_exported_device_get_type_ (skeleton);
        int state = up_exported_device_get_state(skeleton);
        if (type == UP_DEVICE_KIND_KEYBOARD && state == UP_DEVICE_STATE_DISCHARGING) {
            warning_level = UP_DEVICE_LEVEL_NONE;
            up_exported_device_set_warning_level (skeleton, warning_level);
            return;
        }


	/* Not finished setting up the object? */
	if (device->priv->daemon == NULL)
		return;

	warning_level = up_daemon_compute_warning_level (device->priv->daemon,
							 up_exported_device_get_state (skeleton),
							 up_exported_device_get_type_ (skeleton),
							 up_exported_device_get_power_supply (skeleton),
							 up_exported_device_get_percentage (skeleton),
							 up_exported_device_get_time_to_empty (skeleton));

	up_exported_device_set_warning_level (skeleton, warning_level);
}

static const gchar *
get_device_charge_icon (gdouble  percentage,
			gboolean charging)
{
	if (percentage < 10)
		return charging ? "battery-caution-charging-symbolic" : "battery-caution-symbolic";
	else if (percentage < 30)
		return charging ? "battery-low-charging-symbolic" : "battery-low-symbolic";
	else if (percentage < 60)
		return charging ? "battery-good-charging-symbolic" : "battery-good-symbolic";
	return charging ? "battery-full-charging-symbolic" : "battery-full-symbolic";
}

/* This needs to be called when one of those properties changes:
 * type
 * state
 * percentage
 * is-present
 */
static void
update_icon_name (UpDevice *device)
{
	const gchar *icon_name = NULL;
	UpExportedDevice *skeleton = UP_EXPORTED_DEVICE (device);

	/* get the icon from some simple rules */
	if (up_exported_device_get_type_ (skeleton) == UP_DEVICE_KIND_LINE_POWER) {
		icon_name = "ac-adapter-symbolic";
	} else {

		if (!up_exported_device_get_is_present (skeleton)) {
			icon_name = "battery-missing-symbolic";

		} else {
			switch (up_exported_device_get_state (skeleton)) {
			case UP_DEVICE_STATE_EMPTY:
				icon_name = "battery-empty-symbolic";
				break;
			case UP_DEVICE_STATE_FULLY_CHARGED:
				icon_name = "battery-full-charged-symbolic";
				break;
			case UP_DEVICE_STATE_CHARGING:
			case UP_DEVICE_STATE_PENDING_CHARGE:
				icon_name = get_device_charge_icon (up_exported_device_get_percentage (skeleton), TRUE);
				break;
			case UP_DEVICE_STATE_DISCHARGING:
			case UP_DEVICE_STATE_PENDING_DISCHARGE:
				icon_name = get_device_charge_icon (up_exported_device_get_percentage (skeleton), FALSE);
				break;
			default:
				icon_name = "battery-missing-symbolic";
			}
		}
	}

	up_exported_device_set_icon_name (skeleton, icon_name);
}

static void
update_history (UpDevice *device)
{
	UpExportedDevice *skeleton = UP_EXPORTED_DEVICE (device);

	/* save new history */
	up_history_set_state (device->priv->history, up_exported_device_get_state (skeleton));
	up_history_set_charge_data (device->priv->history, up_exported_device_get_percentage (skeleton));
	up_history_set_rate_data (device->priv->history, up_exported_device_get_energy_rate (skeleton));
	up_history_set_time_full_data (device->priv->history, up_exported_device_get_time_to_full (skeleton));
	up_history_set_time_empty_data (device->priv->history, up_exported_device_get_time_to_empty (skeleton));
}

/**
 * up_device_notify:
 **/
static void
up_device_notify (GObject *object, GParamSpec *pspec)
{
	UpDevice *device = UP_DEVICE (object);

	G_OBJECT_CLASS (up_device_parent_class)->notify (object, pspec);

	if (g_strcmp0 (pspec->name, "type") == 0 ||
	    g_strcmp0 (pspec->name, "is-present") == 0) {
		update_icon_name (device);
	} else if (g_strcmp0 (pspec->name, "power-supply") == 0 ||
		   g_strcmp0 (pspec->name, "time-to-empty") == 0) {
		update_warning_level (device);
	} else if (g_strcmp0 (pspec->name, "state") == 0 ||
		   g_strcmp0 (pspec->name, "percentage") == 0) {
		update_warning_level (device);
		update_icon_name (device);
	} else if (g_strcmp0 (pspec->name, "update-time") == 0) {
		update_history (device);
	}
}

/**
 * up_device_get_on_battery:
 *
 * Note: Only implement for system devices, i.e. ones supplying the system
 **/
gboolean
up_device_get_on_battery (UpDevice *device, gboolean *on_battery)
{
	UpDeviceClass *klass = UP_DEVICE_GET_CLASS (device);

	g_return_val_if_fail (UP_IS_DEVICE (device), FALSE);

	/* no support */
	if (klass->get_on_battery == NULL)
		return FALSE;

	return klass->get_on_battery (device, on_battery);
}

/**
 * up_device_get_online:
 *
 * Note: Only implement for system devices, i.e. devices supplying the system
 **/
gboolean
up_device_get_online (UpDevice *device, gboolean *online)
{
	UpDeviceClass *klass = UP_DEVICE_GET_CLASS (device);

	g_return_val_if_fail (UP_IS_DEVICE (device), FALSE);

	/* no support */
	if (klass->get_online == NULL)
		return FALSE;

	return klass->get_online (device, online);
}

/**
 * up_device_get_id:
 **/
static gchar *
up_device_get_id (UpDevice *device)
{
	GString *string;
	gchar *id = NULL;
	const char *model;
	const char *serial;
	UpExportedDevice *skeleton;

	skeleton = UP_EXPORTED_DEVICE (device);
	model = up_exported_device_get_model (skeleton);
	serial = up_exported_device_get_serial (skeleton);

	/* line power */
	if (up_exported_device_get_type_ (skeleton) == UP_DEVICE_KIND_LINE_POWER) {
		goto out;

	/* batteries */
	} else if (up_exported_device_get_type_ (skeleton) == UP_DEVICE_KIND_BATTERY) {
		/* we don't have an ID if we are not present */
		if (!up_exported_device_get_is_present (skeleton))
			goto out;

		string = g_string_new ("");

		/* in an ideal world, model-capacity-serial */
		if (model != NULL && strlen (model) > 2) {
			g_string_append (string, model);
			g_string_append_c (string, '-');
		}
		if (up_exported_device_get_energy_full_design (skeleton) > 0) {
			/* FIXME: this may not be stable if we are using voltage_now */
			g_string_append_printf (string, "%i", (guint) up_exported_device_get_energy_full_design (skeleton));
			g_string_append_c (string, '-');
		}
		if (serial != NULL && strlen (serial) > 2) {
			g_string_append (string, serial);
			g_string_append_c (string, '-');
		}

		/* make sure we are sane */
		if (string->len == 0) {
			/* just use something generic */
			g_string_append (string, "generic_id");
		} else {
			/* remove trailing '-' */
			g_string_set_size (string, string->len - 1);
		}

		/* the id may have invalid chars that need to be replaced */
		id = g_string_free (string, FALSE);

	} else {
		/* generic fallback, get what data we can */
		string = g_string_new ("");
		if (up_exported_device_get_vendor (skeleton) != NULL) {
			g_string_append (string, up_exported_device_get_vendor (skeleton));
			g_string_append_c (string, '-');
		}
		if (model != NULL) {
			g_string_append (string, model);
			g_string_append_c (string, '-');
		}
		if (serial != NULL) {
			g_string_append (string, serial);
			g_string_append_c (string, '-');
		}

		/* make sure we are sane */
		if (string->len == 0) {
			/* just use something generic */
			g_string_append (string, "generic_id");
		} else {
			/* remove trailing '-' */
			g_string_set_size (string, string->len - 1);
		}

		/* the id may have invalid chars that need to be replaced */
		id = g_string_free (string, FALSE);
	}

	g_strdelimit (id, "\\\t\"?' /,.", '_');

out:
	return id;
}

/**
 * up_device_get_daemon:
 *
 * Returns a refcounted #UpDaemon instance, or %NULL
 **/
UpDaemon *
up_device_get_daemon (UpDevice *device)
{
	if (device->priv->daemon == NULL)
		return NULL;
	return g_object_ref (device->priv->daemon);
}

static void
up_device_export_skeleton (UpDevice *device,
			   const gchar *object_path)
{
	GError *error = NULL;

	g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (device),
					  g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (device->priv->daemon)),
					  object_path,
					  &error);

	if (error != NULL) {
		g_critical ("error registering device on system bus: %s", error->message);
		g_error_free (error);
	}
}

/**
 * up_device_compute_object_path:
 **/
static gchar *
up_device_compute_object_path (UpDevice *device)
{
	gchar *basename;
	gchar *id;
	gchar *object_path;
	const gchar *native_path;
	const gchar *type;
	guint i;

	type = up_device_kind_to_string (up_exported_device_get_type_ (UP_EXPORTED_DEVICE (device)));
	native_path = up_exported_device_get_native_path (UP_EXPORTED_DEVICE (device));
	basename = g_path_get_basename (native_path);
	id = g_strjoin ("_", type, basename, NULL);

	/* make DBUS valid path */
	for (i=0; id[i] != '\0'; i++) {
		if (id[i] == '-')
			id[i] = '_';
		if (id[i] == '.')
			id[i] = 'x';
		if (id[i] == ':')
			id[i] = 'o';
	}
	object_path = g_build_filename (UP_DEVICES_DBUS_PATH, id, NULL);

	g_free (basename);
	g_free (id);

	return object_path;
}

/**
 * up_device_register_device:
 **/
static gboolean
up_device_register_device (UpDevice *device)
{
	char *object_path = up_device_compute_object_path (device);
	g_debug ("object path = %s", object_path);
	up_device_export_skeleton (device, object_path);
	g_free (object_path);

	return TRUE;
}

/**
 * up_device_coldplug:
 *
 * Return %TRUE on success, %FALSE if we failed to get data and should be removed
 **/
gboolean
up_device_coldplug (UpDevice *device, UpDaemon *daemon, GObject *native)
{
	gboolean ret;
	const gchar *native_path;
	UpDeviceClass *klass = UP_DEVICE_GET_CLASS (device);
	gchar *id = NULL;

	g_return_val_if_fail (UP_IS_DEVICE (device), FALSE);

	/* save */
	device->priv->native = g_object_ref (native);
	device->priv->daemon = g_object_ref (daemon);

	native_path = up_native_get_native_path (native);
	up_exported_device_set_native_path (UP_EXPORTED_DEVICE (device), native_path);

	/* coldplug source */
	if (klass->coldplug != NULL) {
		ret = klass->coldplug (device);
		if (!ret) {
			g_debug ("failed to coldplug %s", native_path);
			goto bail;
		}
	}

	/* force a refresh, although failure isn't fatal */
	ret = up_device_refresh_internal (device);
	if (!ret) {
		g_debug ("failed to refresh %s", native_path);

		/* TODO: refresh should really have separate
		 *       success _and_ changed parameters */
		goto out;
	}

	/* get the id so we can load the old history */
	id = up_device_get_id (device);
	if (id != NULL) {
		up_history_set_id (device->priv->history, id);
		g_free (id);
	}
out:
	/* only put on the bus if we succeeded */
	ret = up_device_register_device (device);
	if (!ret) {
		g_warning ("failed to register device %s", native_path);
		goto out;
	}
bail:
	return ret;
}

/**
 * up_device_unplug:
 *
 * Initiates destruction of %UpDevice, undoing the effects of
 * up_device_coldplug.
 */
void
up_device_unplug (UpDevice *device)
{
	/* break circular dependency */
	if (device->priv->daemon != NULL) {
		g_object_unref (device->priv->daemon);
		device->priv->daemon = NULL;
	}
}

/**
 * up_device_get_statistics:
 **/
static gboolean
up_device_get_statistics (UpExportedDevice *skeleton,
			  GDBusMethodInvocation *invocation,
			  const gchar *type,
			  UpDevice *device)
{
	GPtrArray *array = NULL;
	UpStatsItem *item;
	guint i;
	GVariantBuilder builder;

	/* doesn't even try to support this */
	if (!up_exported_device_get_has_statistics (skeleton)) {
		g_dbus_method_invocation_return_error_literal (invocation,
							       UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
							       "device does not support getting stats");
		goto out;
	}

	/* get the correct data */
	if (g_strcmp0 (type, "charging") == 0)
		array = up_history_get_profile_data (device->priv->history, TRUE);
	else if (g_strcmp0 (type, "discharging") == 0)
		array = up_history_get_profile_data (device->priv->history, FALSE);

	/* maybe the device doesn't support histories */
	if (array == NULL) {
		g_dbus_method_invocation_return_error_literal (invocation,
							       UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
							       "device has no statistics");
		goto out;
	}

	/* always 101 items of data */
	if (array->len != 101) {
		g_dbus_method_invocation_return_error (invocation,
						       UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
						       "statistics invalid as have %i items", array->len);
		goto out;
	}

	/* copy data to dbus struct */
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(dd)"));
	for (i = 0; i < array->len; i++) {
		item = (UpStatsItem *) g_ptr_array_index (array, i);
		g_variant_builder_add (&builder, "(dd)",
				       up_stats_item_get_value (item),
				       up_stats_item_get_accuracy (item));
	}

	up_exported_device_complete_get_statistics (skeleton, invocation,
						    g_variant_builder_end (&builder));
out:
	if (array != NULL)
		g_ptr_array_unref (array);
	return TRUE;
}

/**
 * up_device_get_history:
 **/
static gboolean
up_device_get_history (UpExportedDevice *skeleton,
		       GDBusMethodInvocation *invocation,
		       const gchar *type_string,
		       guint timespan,
		       guint resolution,
		       UpDevice *device)
{
	GPtrArray *array = NULL;
	UpHistoryItem *item;
	guint i;
	UpHistoryType type = UP_HISTORY_TYPE_UNKNOWN;
	GVariantBuilder builder;

	/* doesn't even try to support this */
	if (!up_exported_device_get_has_history (skeleton)) {
		g_dbus_method_invocation_return_error_literal (invocation,
							       UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
							       "device does not support getting history");
		goto out;
	}

	/* get the correct data */
	if (g_strcmp0 (type_string, "rate") == 0)
		type = UP_HISTORY_TYPE_RATE;
	else if (g_strcmp0 (type_string, "charge") == 0)
		type = UP_HISTORY_TYPE_CHARGE;
	else if (g_strcmp0 (type_string, "time-full") == 0)
		type = UP_HISTORY_TYPE_TIME_FULL;
	else if (g_strcmp0 (type_string, "time-empty") == 0)
		type = UP_HISTORY_TYPE_TIME_EMPTY;

	/* something recognised */
	if (type != UP_HISTORY_TYPE_UNKNOWN)
		array = up_history_get_data (device->priv->history, type, timespan, resolution);

	/* maybe the device doesn't have any history */
	if (array == NULL) {
		g_dbus_method_invocation_return_error_literal (invocation,
							       UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
							       "device has no history");
		goto out;
	}

	/* copy data to dbus struct */
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(udu)"));
	for (i = 0; i < array->len; i++) {
		item = (UpHistoryItem *) g_ptr_array_index (array, i);
		g_variant_builder_add (&builder, "(udu)",
				       up_history_item_get_time (item),
				       up_history_item_get_value (item),
				       up_history_item_get_state (item));
	}

	up_exported_device_complete_get_history (skeleton, invocation,
						 g_variant_builder_end (&builder));

out:
	if (array != NULL)
		g_ptr_array_unref (array);
	return TRUE;
}

/**
 * up_device_refresh:
 *
 * Return %TRUE on success, %FALSE if we failed to refresh or no data
 **/
static gboolean
up_device_refresh (UpExportedDevice *skeleton,
		   GDBusMethodInvocation *invocation,
		   UpDevice *device)
{
	up_device_refresh_internal (device);
	up_exported_device_complete_refresh (skeleton, invocation);
	return TRUE;
}

/**
 * up_device_register_display_device:
 **/
gboolean
up_device_register_display_device (UpDevice *device,
				   UpDaemon *daemon)
{
	char *object_path;

	g_return_val_if_fail (UP_IS_DEVICE (device), FALSE);

	device->priv->daemon = g_object_ref (daemon);
	object_path = g_build_filename (UP_DEVICES_DBUS_PATH, "DisplayDevice", NULL);
	up_device_export_skeleton (device, object_path);
	g_free (object_path);

	return TRUE;
}

/**
 * up_device_refresh_internal:
 *
 * NOTE: if you're calling this function you have to ensure you're doing the
 * the changed signals on the right interfaces, although by monitoring
 * notify::update-time this should be mostly done.
 **/
gboolean
up_device_refresh_internal (UpDevice *device)
{
	gboolean ret = FALSE;
	UpDeviceClass *klass = UP_DEVICE_GET_CLASS (device);

	/* not implemented */
	if (klass->refresh == NULL)
		goto out;

	/* do the refresh */
	ret = klass->refresh (device);
	if (!ret) {
		g_debug ("no changes");
		goto out;
	}

	/* the first time, print all properties */
	if (!device->priv->has_ever_refresh) {
		g_debug ("added native-path: %s\n", up_exported_device_get_native_path (UP_EXPORTED_DEVICE (device)));
		device->priv->has_ever_refresh = TRUE;
		goto out;
	}
out:
	return ret;
}

/**
 * up_device_get_object_path:
 **/
const gchar *
up_device_get_object_path (UpDevice *device)
{
	g_return_val_if_fail (UP_IS_DEVICE (device), NULL);
	return g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (device));
}

GObject *
up_device_get_native (UpDevice *device)
{
	g_return_val_if_fail (UP_IS_DEVICE (device), NULL);
	return device->priv->native;
}

/**
 * up_device_init:
 **/
static void
up_device_init (UpDevice *device)
{
	device->priv = UP_DEVICE_GET_PRIVATE (device);
	device->priv->history = up_history_new ();

	g_signal_connect (device, "handle-get-history",
			  G_CALLBACK (up_device_get_history), device);
	g_signal_connect (device, "handle-get-statistics",
			  G_CALLBACK (up_device_get_statistics), device);
	g_signal_connect (device, "handle-refresh",
			  G_CALLBACK (up_device_refresh), device);
}

/**
 * up_device_finalize:
 **/
static void
up_device_finalize (GObject *object)
{
	UpDevice *device;

	g_return_if_fail (object != NULL);
	g_return_if_fail (UP_IS_DEVICE (object));

	device = UP_DEVICE (object);
	g_return_if_fail (device->priv != NULL);
	if (device->priv->native != NULL)
		g_object_unref (device->priv->native);
	if (device->priv->daemon != NULL)
		g_object_unref (device->priv->daemon);
	g_object_unref (device->priv->history);

	G_OBJECT_CLASS (up_device_parent_class)->finalize (object);
}

/**
 * up_device_class_init:
 **/
static void
up_device_class_init (UpDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->notify = up_device_notify;
	object_class->finalize = up_device_finalize;

	g_type_class_add_private (klass, sizeof (UpDevicePrivate));
}

/**
 * up_device_new:
 **/
UpDevice *
up_device_new (void)
{
	return UP_DEVICE (g_object_new (UP_TYPE_DEVICE, NULL));
}
