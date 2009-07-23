/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2008, Nokia
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */
#include "config.h"

#include <glib-object.h>
#include <glib/gstdio.h>

#include <libtracker-common/tracker-dbus.h>

#include <libtracker-db/tracker-db-backup.h>
#include <libtracker-data/tracker-data-update.h>

#include "tracker-dbus.h"
#include "tracker-backup.h"
#include "tracker-store.h"

typedef struct {
	DBusGMethodInvocation *context;
	guint request_id;
} TrackerDBusMethodInfo;

G_DEFINE_TYPE (TrackerBackup, tracker_backup, G_TYPE_OBJECT)

static void
tracker_backup_class_init (TrackerBackupClass *klass)
{
}

static void
tracker_backup_init (TrackerBackup *backup)
{
}

TrackerBackup *
tracker_backup_new (void)
{
	return g_object_new (TRACKER_TYPE_BACKUP, NULL);
}

typedef struct {
	GFile *file;
	guint request_id;
	DBusGMethodInvocation *context;
} BackupInfo;

static void
on_backup_destroy (gpointer user_data)
{
	BackupInfo *info = user_data;
	g_object_unref (info->file);
	g_free (info);
}

static void
on_backup_finished (GError *error, gpointer user_data)
{
	BackupInfo *info = user_data;

	if (error) {
		GError *actual_error = NULL;

		tracker_dbus_request_failed (info->request_id,
		                             &actual_error,
		                             error->message);

		dbus_g_method_return_error (info->context, actual_error);

		g_error_free (actual_error);
	} else {
		dbus_g_method_return (info->context);
		tracker_dbus_request_success (info->request_id);
	}
}

void
tracker_backup_save (TrackerBackup          *object,
                     const gchar            *uri,
                     DBusGMethodInvocation  *context,
                     GError                **error)
{
	guint request_id;
	GFile *file;
	BackupInfo *info;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_request_new (request_id,
				  "DBus request to save backup into '%s'",
				  uri);

	/* Previous DBus API accepted paths. For this reason I decided to try
	 * to support both paths and uris. Perhaps we should just remove the
	 * support for paths here? */

	if (!strchr (uri, ':')) {
		file = g_file_new_for_path (uri);
	} else {
		file = g_file_new_for_uri (uri);
	}

	info = g_new (BackupInfo, 1);

	info->request_id = request_id;
	info->file = file;
	info->context = context;

	tracker_db_backup_save (file, 
	                        on_backup_finished,
	                        info,
	                        on_backup_destroy);

}
#if 0
static void
destroy_method_info (gpointer user_data)
{
	g_slice_free (TrackerDBusMethodInfo, user_data);
}

static void
backup_callback (GError *error, gpointer user_data)
{
	TrackerDBusMethodInfo *info = user_data;

	if (error) {
		tracker_dbus_request_failed (info->request_id,
		                             &error,
		                             NULL);
		dbus_g_method_return_error (info->context, error);
		return;
	}

	dbus_g_method_return (info->context);

	tracker_dbus_request_success (info->request_id);
}
#endif

void
tracker_backup_restore (TrackerBackup          *object,
                        const gchar            *uri,
                        DBusGMethodInvocation  *context,
                        GError                **error)
{
#if 0
	guint request_id;
	GError *actual_error = NULL;
	TrackerDBusMethodInfo *info;
	GFile *file;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_request_new (request_id,
	                          "DBus request to restore backup from '%s'",
	                          uri);

	/* First check we have disk space */
#if 0
	/* FIXME: MJR */
	if (tracker_status_get_is_paused_for_space ()) {
		tracker_dbus_request_failed (request_id,
		                             &actual_error,
		                             "No disk space left to write to"
		                             " the databases");
		dbus_g_method_return_error (context, actual_error);
		g_error_free (actual_error);
		return;
	}
#endif

	tracker_dbus_request_new (request_id,
	                          "DBus request to restore backup '%s'",
	                          uri);

	/* Previous DBus API accepted paths. For this reason I decided to try
	 * to support both paths and uris. Perhaps we should just remove the
	 * support for paths here? */

	if (!strchr (uri, ':')) {
		file = g_file_new_for_path (uri);
	} else {
		file = g_file_new_for_uri (uri);
	}

	info = g_slice_new (TrackerDBusMethodInfo);

	info->request_id = request_id;
	info->context = context;

	tracker_store_queue_turtle_import (file, backup_callback,
	                                   info, destroy_method_info);

	g_object_unref (file);
#endif
}
