/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
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

/**
 * SECTION: tracker-sparql-connection
 * @short_description: Manage connections to the Store
 * @title: TrackerSparqlConnection
 * @stability: Stable
 * @include: tracker-sparql.h
 *
 * <para>
 * #TrackerSparqlConnection is an object which allows setting up read-only
 * connections to the Tracker Store.
 * </para>
 */

// Convenience
public const string TRACKER_DBUS_SERVICE = "org.freedesktop.Tracker1";
public const string TRACKER_DBUS_INTERFACE_RESOURCES = TRACKER_DBUS_SERVICE + ".Resources";
public const string TRACKER_DBUS_OBJECT_RESOURCES = "/org/freedesktop/Tracker1/Resources";
public const string TRACKER_DBUS_INTERFACE_STATISTICS = TRACKER_DBUS_SERVICE + ".Statistics";
public const string TRACKER_DBUS_OBJECT_STATISTICS = "/org/freedesktop/Tracker1/Statistics";
public const string TRACKER_DBUS_INTERFACE_STEROIDS = TRACKER_DBUS_SERVICE + ".Steroids";
public const string TRACKER_DBUS_OBJECT_STEROIDS = "/org/freedesktop/Tracker1/Steroids";

public errordomain Tracker.Sparql.Error {
	PARSE,
	UNKNOWN_CLASS,
	UNKNOWN_PROPERTY,
	TYPE,
	INTERNAL,
	UNSUPPORTED
}

/**
 * TrackerSparqlConnection:
 *
 * The <structname>TrackerSparqlConnection</structname> object represents an
 * connection with the Tracker Store.
 */
public abstract class Tracker.Sparql.Connection : Object {
	static bool direct_only;
	static weak Connection? singleton;
	static int verbosity = 0;

	/**
	 * tracker_sparql_connection_get:
	 *
	 * Returns a new #TrackerSparqlConnection, which will use the best method
	 * available to connect to the Tracker Store.
	 *
	 * Returns: a new #TrackerSparqlConnection. Call g_object_unref() on the
	 * object when no longer used.
	 */
	public static Connection get () throws Sparql.Error {
		if (singleton != null) {
			assert (!direct_only);
			return singleton;
		} else {
			log_init ();

			var result = new PluginLoader ();
			singleton = result;
			result.add_weak_pointer ((void**) (&singleton));
			return result;
		}
	}

	/**
	 * tracker_sparql_connection_get_direct:
	 *
	 * Returns a new #TrackerSparqlConnection, which uses direct-access method
	 * to connect to the Tracker Store.
	 *
	 * Returns: a new #TrackerSparqlConnection. Call g_object_unref() on the
	 * object when no longer used.
	 */
	public static Connection get_direct () throws Sparql.Error {
		if (singleton != null) {
			assert (direct_only);
			return singleton;
		} else {
			log_init ();

			var result = new PluginLoader (true /* direct_only */);
			direct_only = true;
			singleton = result;
			result.add_weak_pointer ((void**) (&singleton));
			return result;
		}
	}

	private static void log_init () {
		// Avoid debug messages
		string env_verbosity = Environment.get_variable ("TRACKER_SPARQL_VERBOSITY");
		if (env_verbosity != null)
			verbosity = env_verbosity.to_int ();

		GLib.Log.set_handler (null, LogLevelFlags.LEVEL_MASK | LogLevelFlags.FLAG_FATAL, log_handler);
		GLib.Log.set_default_handler (log_handler);
	}

	private static bool log_should_handle (LogLevelFlags log_level) {
		switch (verbosity) {
		// Log level 3: EVERYTHING
		case 3:
			break;

		// Log level 2: CRITICAL/ERROR/WARNING/INFO/MESSAGE only
		case 2:
			if (!(LogLevelFlags.LEVEL_MESSAGE in log_level) &&
				!(LogLevelFlags.LEVEL_INFO in log_level) &&
				!(LogLevelFlags.LEVEL_WARNING in log_level) &&
				!(LogLevelFlags.LEVEL_ERROR in log_level) &&
				!(LogLevelFlags.LEVEL_CRITICAL in log_level)) {
				return false;
			}

			break;

		// Log level 1: CRITICAL/ERROR/WARNING/INFO only
		case 1:
			if (!(LogLevelFlags.LEVEL_INFO in log_level) &&
				!(LogLevelFlags.LEVEL_WARNING in log_level) &&
				!(LogLevelFlags.LEVEL_ERROR in log_level) &&
				!(LogLevelFlags.LEVEL_CRITICAL in log_level)) {
				return false;
			}

			break;

		// Log level 0: CRITICAL/ERROR/WARNING only (default)
		default:
		case 0:
			if (!(LogLevelFlags.LEVEL_WARNING in log_level) &&
				!(LogLevelFlags.LEVEL_ERROR in log_level) &&
				!(LogLevelFlags.LEVEL_CRITICAL in log_level)) {
				return false;
			}

			break;
		}

		return true;
	}

	private static void log_handler (string? log_domain, LogLevelFlags log_level, string message) {
		if (!log_should_handle (log_level)) {
			return;
		}

		GLib.Log.default_handler (log_domain, log_level, message);
	}

	/**
	 * tracker_sparql_connection_query:
	 * @self: a #TrackerSparqlConnection
	 * @sparql: string containing the SPARQL query
	 * @cancellable: a #GCancellable used to cancel the operation
	 * @error: #GError for error reporting.
	 *
	 * Executes a SPARQL query on the store. The API call is completely
	 * synchronous, so it may block.
	 *
	 * Returns: a #TrackerSparqlCursor to iterate the reply if successful, #NULL
	 * on error. Call g_object_unref() on the returned cursor when no longer
	 * needed.
	 */
	public abstract Cursor? query (string sparql, Cancellable? cancellable = null) throws Sparql.Error;

	/**
	 * tracker_sparql_connection_query_async:
	 * @self: a #TrackerSparqlConnection
	 * @sparql: string containing the SPARQL query
	 * @_callback_: user-defined #GAsyncReadyCallback to be called when
	 *              asynchronous operation is finished.
	 * @_user_data_: user-defined data to be passed to @_callback_
	 * @cancellable: a #GCancellable used to cancel the operation
	 *
	 * Executes asynchronously a SPARQL query on the store.
	 */

	/**
	 * tracker_sparql_connection_query_finish:
	 * @self: a #TrackerSparqlConnection
	 * @_res_: a #GAsyncResult with the result of the operation
	 * @error: #GError for error reporting.
	 *
	 * Finishes the asynchronous SPARQL query operation.
	 *
	 * Returns: a #TrackerSparqlCursor to iterate the reply if successful, #NULL
	 * on error. Call g_object_unref() on the returned cursor when no longer
	 * needed.
	 */
	public async abstract Cursor? query_async (string sparql, Cancellable? cancellable = null) throws Sparql.Error;

	/**
	 * tracker_sparql_connection_update:
	 * @self: a #TrackerSparqlConnection
	 * @sparql: string containing the SPARQL update query
	 * @cancellable: a #GCancellable used to cancel the operation
	 * @error: #GError for error reporting.
	 *
	 * Executes a SPARQL update on the store. The API call is completely
	 * synchronous, so it may block.
	 */
	public virtual void update (string sparql, Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'update' not implemented");
	}

	/**
	 * tracker_sparql_connection_update_async:
	 * @self: a #TrackerSparqlConnection
	 * @sparql: string containing the SPARQL update query
	 * @priority: the priority for the asynchronous operation
	 * @_callback_: user-defined #GAsyncReadyCallback to be called when
	 *              asynchronous operation is finished.
	 * @_user_data_: user-defined data to be passed to @_callback_
	 * @cancellable: a #GCancellable used to cancel the operation
	 *
	 * Executes asynchronously a SPARQL update on the store.
	 */

	/**
	 * tracker_sparql_connection_update_finish:
	 * @self: a #TrackerSparqlConnection
	 * @_res_: a #GAsyncResult with the result of the operation
	 * @error: #GError for error reporting.
	 *
	 * Finishes the asynchronous SPARQL update operation.
	 */
	public async virtual void update_async (string sparql, int priority = GLib.Priority.DEFAULT, Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'update_async' not implemented");
	}

	/**
	 * tracker_sparql_connection_update_blank:
	 * @self: a #TrackerSparqlConnection
	 * @sparql: string containing the SPARQL update query
	 * @cancellable: a #GCancellable used to cancel the operation
	 * @error: #GError for error reporting.
	 *
	 * Executes a SPARQL update on the store, and returns the URNs of the
	 * generated nodes, if any. The API call is completely synchronous, so it
	 * may block.
	 *
	 * Returns: a #GVariant with the generated URNs, which should be freed with
	 * g_variant_unref() when no longer used.
	 */
	public virtual GLib.Variant? update_blank (string sparql, Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'update_blank' not implemented");
		return null;
	}

	/**
	 * tracker_sparql_connection_update_blank_async:
	 * @self: a #TrackerSparqlConnection
	 * @sparql: string containing the SPARQL update query
	 * @priority: the priority for the asynchronous operation
	 * @_callback_: user-defined #GAsyncReadyCallback to be called when
	 *              asynchronous operation is finished.
	 * @_user_data_: user-defined data to be passed to @_callback_
	 * @cancellable: a #GCancellable used to cancel the operation
	 *
	 * Executes asynchronously a SPARQL update on the store.
	 */

	/**
	 * tracker_sparql_connection_update_blank_finish:
	 * @self: a #TrackerSparqlConnection
	 * @_res_: a #GAsyncResult with the result of the operation
	 * @error: #GError for error reporting.
	 *
	 * Finishes the asynchronous SPARQL update operation, and returns
	 * the URNs of the generated nodes, if any.
	 *
	 * Returns: a #GVariant with the generated URNs, which should be freed with
	 * g_variant_unref() when no longer used.
	 */
	public async virtual GLib.Variant? update_blank_async (string sparql, int priority = GLib.Priority.DEFAULT, Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'update_blank_async' not implemented");
		return null;
	}

	/**
	 * tracker_sparql_connection_update_commit:
	 * @self: a #TrackerSparqlConnection
	 * @cancellable: a #GCancellable used to cancel the operation
	 * @error: #GError for error reporting.
	 *
	 * Makes sure that any previous asynchronous update has been commited
	 * to the store. Only applies to tracker_sparql_connection_update_async()
	 * with the right priority (Priority is used to identify batch updates.)
	 * The API call is completely synchronous, so it may block.
	 */
	public virtual void update_commit (Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'update_commit' not implemented");
	}

	/**
	 * tracker_sparql_connection_update_commit_async:
	 * @self: a #TrackerSparqlConnection
	 * @_callback_: user-defined #GAsyncReadyCallback to be called when
	 *              asynchronous operation is finished.
	 * @_user_data_: user-defined data to be passed to @_callback_
	 * @cancellable: a #GCancellable used to cancel the operation
	 *
	 * Makes sure, asynchronously, that any previous asynchronous update has
	 * been commited to the store. Only applies to
	 * tracker_sparql_connection_update_async() with the right priority
	 * (Priority is used to identify batch updates.)
	 * Executes asynchronously a SPARQL update on the store.
	 */

	/**
	 * tracker_sparql_connection_update_commit_finish:
	 * @self: a #TrackerSparqlConnection
	 * @_res_: a #GAsyncResult with the result of the operation
	 * @error: #GError for error reporting.
	 *
	 * Finishes the asynchronous commit operation.
	 */
	public async virtual void update_commit_async (Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'update_commit_async' not implemented");
	}

	// Import
	public virtual void import (File file, Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'import' not implemented");
	}
	public async virtual void import_async (File file, Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'import_async' not implemented");
	}

	// Statistics
	public virtual Cursor? statistics (Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'statistics' not implemented");
		return null;
	}

	public async virtual Cursor? statistics_async (Cancellable? cancellable = null) throws Sparql.Error {
		warning ("Interface 'statistics_async' not implemented");
		return null;
	}
}
