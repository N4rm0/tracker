/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008, Nokia
 * Authors: Philip Van Hoof (pvanhoof@gnome.org)
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

#ifndef _TRACKER_XESAM_SESSION_H_
#define _TRACKER_XESAM_SESSION_H_

#include <glib.h>
#include <glib-object.h>
#include <stdlib.h>
#include <string.h>

#include "tracker-xesam-live-search.h"
#include "tracker-xesam-search.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_XESAM_SESSION (tracker_xesam_session_get_type ())
#define TRACKER_XESAM_SESSION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), TRACKER_TYPE_XESAM_SESSION, TrackerXesamSession))
#define TRACKER_XESAM_SESSION_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), TRACKER_TYPE_XESAM_SESSION, TrackerXesamSessionClass))
#define TRACKER_IS_XESAM_SESSION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), TRACKER_TYPE_XESAM_SESSION))
#define TRACKER_IS_XESAM_SESSION_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), TRACKER_TYPE_XESAM_SESSION))
#define TRACKER_XESAM_SESSION_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), TRACKER_TYPE_XESAM_SESSION, TrackerXesamSessionClass))

typedef struct _TrackerXesamSession TrackerXesamSession;
typedef struct _TrackerXesamSessionClass TrackerXesamSessionClass;
typedef struct _TrackerXesamSessionPriv TrackerXesamSessionPriv;

struct _TrackerXesamSession {
	GObject parent_instance;
	TrackerXesamSessionPriv *priv;
};

struct _TrackerXesamSessionClass {
	GObjectClass parent_class;
};

void                    tracker_xesam_session_set_property  (TrackerXesamSession  *self,
							     const gchar          *prop,
							     const GValue         *val,
							     GValue              **new_val,
							     GError              **error);
void                    tracker_xesam_session_get_property  (TrackerXesamSession  *self,
							     const gchar          *prop,
							     GValue              **value,
							     GError              **error);
TrackerXesamLiveSearch* tracker_xesam_session_create_search (TrackerXesamSession  *self,
							     const gchar          *query_xml,
							     gchar               **search_id,
							     GError              **error);
TrackerXesamLiveSearch* tracker_xesam_session_get_search    (TrackerXesamSession  *self,
							     const gchar          *search_id,
							     GError              **error);
GList *                 tracker_xesam_session_get_searches  (TrackerXesamSession  *self);
void                    tracker_xesam_session_set_id        (TrackerXesamSession  *self,
							     const gchar          *session_id);
const gchar*            tracker_xesam_session_get_id        (TrackerXesamSession  *self);
TrackerXesamSession*    tracker_xesam_session_new           (void);
GType                   tracker_xesam_session_get_type      (void);

G_END_DECLS

#endif
