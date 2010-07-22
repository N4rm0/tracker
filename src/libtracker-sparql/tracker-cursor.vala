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

public interface Tracker.Cursor {
	public abstract bool new_for_query (string query);
	public abstract bool rewind ();
	public abstract int n_columns { get; }
	public abstract bool get_value (uint column, out Value value);
	public abstract bool get_string (uint column, out string value);
	public abstract bool get_double (uint column, out double value);
	public abstract bool iter_next () throws GLib.Error;
}

