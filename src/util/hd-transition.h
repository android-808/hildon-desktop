/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author:  Gordon Williams <gordon.williams@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifndef __HD_TRANSITION_H__
#define __HD_TRANSITION_H__

#include "mb/hd-comp-mgr.h"
#include "mb/hd-wm.h"
#include "home/hd-render-manager.h"
#include "util/hd-util.h"

/* The file name of the particle image used in close-app transitions
 * and the number of them to show in the transition. */
#define HDCM_UNMAP_PARTICLES        8

/* What noise to make when a new application is started or when the last
 * window of an application window stack is closed. */
#define HDCM_WINDOW_OPENED_SOUND    "/usr/share/sounds/ui-window_open.wav"
#define HDCM_WINDOW_CLOSED_SOUND    "/usr/share/sounds/ui-window_close.wav"

float
hd_transition_overshoot(float x);

float
hd_transition_smooth_ramp(float amt);

float
hd_transition_ease_in(float amt);

float
hd_transition_ease_out(float amt);

/* For the animated progress indicator in the title bar */
void
on_decor_progress_timeline_new_frame(ClutterTimeline *timeline,
                                     gint frame_num,
                                     ClutterActor *progress_texture);

void
hd_transition_popup(HdCompMgr                  *mgr,
                    MBWindowManagerClient      *c,
                    MBWMCompMgrClientEvent     event);
void
hd_transition_fade(HdCompMgr                  *mgr,
                   MBWindowManagerClient      *c,
                   MBWMCompMgrClientEvent     event);
void
hd_transition_fade_out_loading_screen(ClutterActor *loading_image);
void
hd_transition_close_app (HdCompMgr                  *mgr,
                         MBWindowManagerClient      *c);
void
hd_transition_notification(HdCompMgr                  *mgr,
                           MBWindowManagerClient      *c,
                           MBWMCompMgrClientEvent     event);
void
hd_transition_subview(HdCompMgr                  *mgr,
                      MBWindowManagerClient      *subview,
                      MBWindowManagerClient      *mainview,
                      MBWMCompMgrClientEvent     event);

void
hd_transition_stop(HdCompMgr                  *mgr,
                   MBWindowManagerClient      *client);

gboolean
hd_transition_rotate_screen(MBWindowManager *wm, gboolean goto_portrait);
void
hd_transition_rotate_screen_and_change_state (HDRMStateEnum state);
gboolean
hd_transition_rotation_will_change_state (void);
gboolean
hd_transition_is_rotating (void);
gboolean
hd_transition_is_rotating_to_portrait (void);
gboolean
hd_transition_rotate_ignore_damage(void);

gboolean
hd_transition_actor_will_go_away (ClutterActor *actor);

void
hd_transition_play_sound(const gchar           *fname);

gint
hd_transition_get_int(const gchar *transition,
                      const char *key,
                      gint default_val);

gdouble
hd_transition_get_double(const gchar *transition,
                      const char *key,
                      gdouble default_val);
gchar *
hd_transition_get_string(const gchar *transition, const char *key,
                      gchar *default_val);

HdKeyFrameList *
hd_transition_get_keyframes(const gchar *transition, const char *key,
                            gchar *default_val);

void
hd_transition_set_file_changed(void);

void
hd_transition_play_tactile(gboolean is_map, MBWMClientType c_type);

#endif /* __HD_TRANSITION_H__ */
