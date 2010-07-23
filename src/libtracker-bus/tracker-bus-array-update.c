/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008-2010, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2010, Codeminded BVBA <philip@codeminded.be>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib-object.h>

#include <libtracker-common/tracker-common.h>
#include <libtracker-sparql/tracker-sparql.h>

#include "tracker-bus-array-update.h"
#include "tracker-bus.h"
#include "tracker-bus-shared.h"

void
tracker_bus_array_sparql_update_blank_async (DBusGConnection       *connection,
                                             const gchar           *query,
                                             GCancellable          *cancellable,
                                             GAsyncReadyCallback    callback,
                                             gpointer               user_data)
{
#ifdef HAVE_DBUS_FD_PASSING

	g_critical ("tracker_bus_array_sparql_update_blank_async unimplemented");
	// todo

#else  /* HAVE_DBUS_FD_PASSING */
	g_assert_not_reached ();
#endif /* HAVE_DBUS_FD_PASSING */
}


GVariant *
tracker_bus_array_sparql_update_blank (DBusGConnection *connection,
                                       const gchar     *query,
                                       GError         **error)
{
#ifdef HAVE_DBUS_FD_PASSING
	DBusMessage *reply, *message;
	GVariant *result;
	DBusMessageIter iter;
	DBusPendingCall *call;

	g_return_val_if_fail (query != NULL, NULL);

	message = dbus_message_new_method_call (TRACKER_DBUS_SERVICE,
	                                        TRACKER_DBUS_OBJECT_RESOURCES,
	                                        TRACKER_DBUS_INTERFACE_RESOURCES,
	                                        "UpdateBlank");

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &query);
	dbus_connection_send_with_reply (connection, message, &call, -1);
	dbus_message_unref (message);

	if (!call) {
		g_set_error (error,
		             TRACKER_SPARQL_ERROR,
		             TRACKER_SPARQL_ERROR_UNSUPPORTED,
		             "UpdateBlank Unsupported or connection disconnected");
		return NULL;
	}

	dbus_pending_call_block (call);

	reply = dbus_pending_call_steal_reply (call);

	if (!reply) {
		return NULL;
	}

	if (dbus_message_get_type (reply) == DBUS_MESSAGE_TYPE_ERROR) {
		DBusError dbus_error;

		dbus_error_init (&dbus_error);
		dbus_set_error_from_message (&dbus_error, reply);
		dbus_set_g_error (error, &dbus_error);
		dbus_pending_call_unref (call);
		dbus_error_free (&dbus_error);

		return NULL;
	}

	dbus_pending_call_unref (call);

	if (g_strcmp0 (dbus_message_get_signature (reply), "aaa{ss}")) {
		g_set_error (error,
		             TRACKER_SPARQL_ERROR,
		             TRACKER_SPARQL_ERROR_UNSUPPORTED,
		             "Server returned invalid results");
		dbus_message_unref (reply);
		return NULL;
	}

	result = tracker_bus_message_to_variant (reply);

	dbus_message_unref (reply);

	return result;
#else  /* HAVE_DBUS_FD_PASSING */
	g_assert_not_reached ();
	return NULL;
#endif /* HAVE_DBUS_FD_PASSING */
}

GVariant *
tracker_bus_array_sparql_update_blank_finish (GAsyncResult     *res,
                                              GError          **error)
{
#ifdef HAVE_DBUS_FD_PASSING
	g_return_val_if_fail (res != NULL, NULL);

	if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error)) {
		return NULL;
	}

	return g_object_ref (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
#else /* HAVE_DBUS_FD_PASSING */
	g_assert_not_reached ();
	return NULL;
#endif /* HAVE_DBUS_FD_PASSING */
}
