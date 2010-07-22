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

public unowned string tracker_sparql_escape_string (string literal) {
	StringBuilder str = new StringBuilder ();
	char *p = literal;

	while (*p != '\0') {
		size_t len = Posix.strcspn ((string) p, "\t\n\r\"\\");
		str.append_len ((string) p, (long) len);
		p += len;

		switch (*p) {
		case '\t':
			str.append ("\\t");
			break;
		case '\n':
			str.append ("\\n");
			break;
		case '\r':
			str.append ("\\r");
			break;
		case '\b':
			str.append ("\\b");
			break;
		case '\f':
			str.append ("\\f");
			break;
		case '"':
			str.append ("\\\"");
			break;
		case '\\':
			str.append ("\\\\");
			break;
		default:
			continue;
		}

		p++;
	}

	return str.str;
}
