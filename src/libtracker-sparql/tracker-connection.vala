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

// Convenience
public const string TRACKER_DBUS_SERVICE = "org.freedesktop.Tracker1";
public const string TRACKER_DBUS_INTERFACE_RESOURCES = TRACKER_DBUS_SERVICE + ".Resources";
public const string TRACKER_DBUS_OBJECT_RESOURCES = "/org/freedesktop/Tracker1/Resources";
public const string TRACKER_DBUS_INTERFACE_STEROIDS = TRACKER_DBUS_SERVICE + ".Steroids";
public const string TRACKER_DBUS_OBJECT_STEROIDS = "/org/freedesktop/Tracker1/Steroids";

public abstract class Tracker.Sparql.Connection : Object {
	static bool direct_only;
	static weak Connection? singleton;

	public static Connection get () throws GLib.Error {
		if (singleton != null) {
			assert (!direct_only);
			return singleton;
		} else {
			var result = new PluginLoader ();
			singleton = result;
			result.add_weak_pointer ((void**) (&singleton));
			return result;
		}
	}

	public static Connection get_direct () throws GLib.Error {
		if (singleton != null) {
			assert (direct_only);
			return singleton;
		} else {
			var result = new PluginLoader (true /* direct_only */);
			direct_only = true;
			singleton = result;
			result.add_weak_pointer ((void**) (&singleton));
			return result;
		}
	}

	// Query
	public abstract Cursor? query (string sparql, Cancellable? cancellable = null) throws GLib.Error;
	public async abstract Cursor? query_async (string sparql, Cancellable? cancellable = null) throws GLib.Error;

	// Update
	public virtual void update (string sparql, Cancellable? cancellable = null) throws GLib.Error {
		warning ("Interface 'update' not implemented");
	}
	public async virtual void update_async (string sparql, int? priority = GLib.Priority.DEFAULT, Cancellable? cancellable = null) throws GLib.Error {
		warning ("Interface 'update_async' not implemented");
	}

	// Only applies to update_async with the right priority. 
	// Priority is used to identify batch updates.
	public virtual void update_commit (Cancellable? cancellable = null) throws GLib.Error {
		warning ("Interface 'update_commit' not implemented");
	}
	public async virtual void update_commit_async (Cancellable? cancellable = null) throws GLib.Error {
		warning ("Interface 'update_commit_async' not implemented");
	}
	
	// Import
	public virtual void import (File file, Cancellable? cancellable = null) throws GLib.Error {
		warning ("Interface 'import' not implemented");
	}
	public async virtual void import_async (File file, Cancellable? cancellable = null) throws GLib.Error {
		warning ("Interface 'import_async' not implemented");
	}
}
