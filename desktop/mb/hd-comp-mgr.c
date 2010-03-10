/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author:  Johan Bilien <johan.bilien@nokia.com>
 *          Tomas Frydrych <tf@o-hand.com>
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

#include "hd-comp-mgr.h"
#include "hd-switcher.h"
#include "hd-task-navigator.h"
#include "hd-home.h"
#include "hd-dbus.h"
#include "hd-atoms.h"
#include "hd-util.h"
#include "hd-transition.h"
#include "hd-wm.h"
#include "hd-home-applet.h"
#include "hd-app.h"
#include "hd-desktop-config.h"
#include "hd-note.h"
#include "hd-animation-actor.h"
#include "hd-render-manager.h"
#include "hd-title-bar.h"
#include "launcher/hd-app-mgr.h"

#include <matchbox/core/mb-wm.h>
#include <matchbox/core/mb-window-manager.h>
#include <matchbox/core/mb-wm-client.h>
#include <matchbox/theme-engines/mb-wm-theme.h>

#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/shape.h>

#include <clutter/clutter.h>
#include <clutter/x11/clutter-x11.h>

#include "../tidy/tidy-blur-group.h"

#include <dbus/dbus-glib-bindings.h>

#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>

#define OPERATOR_APPLET_ID "_HILDON_OPERATOR_APPLET"
#define STAMP_DIR          "/tmp/hildon-desktop/"
#define STAMP_FILE         STAMP_DIR "desktop-started.stamp"

#if 0
# define PORTRAIT       g_debug
#else
# define PORTRAIT(...)  /* NOP */
#endif

struct HdCompMgrPrivate
{
  MBWindowManagerClient *desktop;
  HdRenderManager       *render_manager;
  HdAppMgr              *app_mgr;

  HdSwitcher            *switcher_group;
  ClutterActor          *home;
  HdTaskNavigator	*task_nav;

  GHashTable            *shown_apps;
  GHashTable            *hibernating_apps;

  Atom                   atoms[_HD_ATOM_LAST];

  DBusConnection        *dbus_connection;

  /* g_idle_add() event source, set by hd_comp_mgr_sync_stacking()
   * to call hd_comp_mgr_restack() some time. */
  guint                  stack_sync;

  /* Do Not Disturb flag */
  gboolean               do_not_disturb_flag : 1;

  MBWindowManagerClient *status_area_client;
  MBWindowManagerClient *status_menu_client;

  MBWindowManagerClient *keyboard;

  HdCompMgrClient       *current_hclient;

  /* Track changes to the PORTRAIT properties. */
  unsigned long          property_changed_cb_id;

  /* Time of last mapped window */
  struct timeval         last_map_time;
};

/*
 * A helper object to store manager's per-client data
 */

struct HdCompMgrClientPrivate
{
  HdRunningApp *app;

  guint                 hibernation_key;
  gboolean              can_hibernate : 1;

  /*
   * Portrait flags are the values of the according X window properties.
   * If a property doesn't exist the flag is inherited from the client
   * that it is transient for.  This case the flags in this structure
   * are cached values.  The validity of the cache is delimited by the
   * timestamp.  This is used to decide whether it is necessary to
   * recalculate the inherited flags of a client.
   *
   * Possible values of @portrait_requested are:
   * -- 0: not requested
   * -- 1: requested, but this can be ignored if other, nonsupporting
   *       clients are visible
   * -- 2: demanded, switch to portrait whatever clients are visible.
   *       In return the client promises to maximize itself so the
   *       other clients wouldn't matter anyway.  Like everything
   *       else this is a hack.
   * -- other values: treated like 2
   */
  gboolean              portrait_supported;
  gboolean              portrait_supported_inherited;
  guint                 portrait_requested;
  gboolean              portrait_requested_inherited;
  guint                 portrait_timestamp;
};

MBWindowManager *hd_mb_wm;

HdRunningApp *hd_comp_mgr_client_get_app_key (HdCompMgrClient *client,
                                               HdCompMgr *hmgr);

static void hd_comp_mgr_check_do_not_disturb_flag (HdCompMgr *hmgr);

static gboolean
hd_comp_mgr_should_be_portrait_ignoring (HdCompMgr *hmgr,
                                         MBWindowManagerClient *ignore);

static void hd_comp_mgr_portrait_or_not_portrait (MBWMCompMgr *mgr,
                                                  MBWindowManagerClient *c);

static gboolean
hd_comp_mgr_client_prefers_compositing (MBWindowManagerClient *c);

static gboolean
hd_comp_mgr_is_non_composited (MBWindowManagerClient *client,
                               gboolean force_re_read);

static MBWindowManagerClient *hd_comp_mgr_determine_current_app (void);

static MBWMCompMgrClient *
hd_comp_mgr_client_new (MBWindowManagerClient * client)
{
  MBWMObject *c;

  c = mb_wm_object_new (HD_TYPE_COMP_MGR_CLIENT,
			MBWMObjectPropClient, client,
			NULL);

  return MB_WM_COMP_MGR_CLIENT (c);
}

static void
hd_comp_mgr_client_class_init (MBWMObjectClass *klass)
{
#if MBWM_WANT_DEBUG
  klass->klass_name = "HdCompMgrClient";
#endif
}

static void
hd_comp_mgr_client_process_hibernation_prop (HdCompMgrClient * hc)
{
  HdCompMgrClientPrivate * priv = hc->priv;
  MBWindowManagerClient  * wm_client = MB_WM_COMP_MGR_CLIENT (hc)->wm_client;
  HdCompMgr              * hmgr = HD_COMP_MGR (wm_client->wmref->comp_mgr);
  Atom                   * hibernable = NULL;

  /* NOTE:
   *       the prop has no 'value'; if set the app is killable (hibernatable),
   *       deletes to unset.
   */
  hibernable = hd_util_get_win_prop_data_and_validate
                     (wm_client->wmref->xdpy,
		      wm_client->window->xwindow,
                      hmgr->priv->atoms[HD_ATOM_HILDON_APP_KILLABLE],
                      XA_STRING,
                      8,
                      0,
                      NULL);

  if (!hibernable)
    {
      /*try the alias*/
      hibernable = hd_util_get_win_prop_data_and_validate
	            (wm_client->wmref->xdpy,
		     wm_client->window->xwindow,
                     hmgr->priv->atoms[HD_ATOM_HILDON_ABLE_TO_HIBERNATE],
                     XA_STRING,
                     8,
                     0,
                     NULL);
    }

  if (hibernable)
      priv->can_hibernate = TRUE;
  else
    priv->can_hibernate = FALSE;

  if (hibernable)
    XFree (hibernable);
}

HdRunningApp *
hd_comp_mgr_client_get_app_key (HdCompMgrClient *client, HdCompMgr *hmgr)
{
  MBWindowManagerClient *wm_client;
  MBWindowManager       *wm;
  XClassHint             class_hint;
  Status                 status = 0;
  HdRunningApp          *app = NULL;
  HdCompMgrClientPrivate *priv = client->priv;

  wm = MB_WM_COMP_MGR (hmgr)->wm;
  wm_client = MB_WM_COMP_MGR_CLIENT (client)->wm_client;

  /* We only lookup the app for main windows and dialogs. */
  if (MB_WM_CLIENT_CLIENT_TYPE (wm_client) != MBWMClientTypeApp &&
      MB_WM_CLIENT_CLIENT_TYPE (wm_client) != MBWMClientTypeDialog)
    return NULL;

  memset(&class_hint, 0, sizeof(XClassHint));

  /* We don't care about X errors here, because they will be reported
   * in the return value of XGetWindowAttributes */
  mb_wm_util_async_trap_x_errors (wm->xdpy);

  status = XGetClassHint(wm->xdpy, wm_client->window->xwindow, &class_hint);

  mb_wm_util_async_untrap_x_errors();
  if (!status)
    goto out;

  app = hd_app_mgr_match_window (class_hint.res_name,
                                 class_hint.res_class,
                                 wm_client->window->pid);

  if (app)
    {
      /* Calculate an hibernation key from:
       * - The app name.
       * - The role, if present.
       * - The window name.
       */
      gchar *role = NULL;
      gchar *key = NULL;
      gint level = 0;
      role = hd_util_get_win_prop_data_and_validate
                         (wm_client->wmref->xdpy,
                          wm_client->window->xwindow,
                          hmgr->priv->atoms[HD_ATOM_WM_WINDOW_ROLE],
                          XA_STRING,
                          8,
                          0,
                          NULL);

      if (MB_WM_CLIENT_CLIENT_TYPE (wm_client) == MBWMClientTypeApp)
        {
          HdApp *hdapp = HD_APP (wm_client);
          level = hdapp->stack_index;
        }

      key = g_strdup_printf ("%s/%s/%s/%d",
              hd_running_app_get_id (app),
              class_hint.res_class ? class_hint.res_class : "",
              role ? role : "",
              level);
      g_debug ("%s: app %s, window key: %s\n", __FUNCTION__,
                hd_running_app_get_id (app),
                key);
      priv->hibernation_key = g_str_hash (key);
      if (role)
        XFree (role);
      g_free (key);
    }

 out:
  if (class_hint.res_class)
    XFree(class_hint.res_class);

  if (class_hint.res_name)
    XFree(class_hint.res_name);

  return app;
}

static int
hd_comp_mgr_client_init (MBWMObject *obj, va_list vap)
{
  HdCompMgrClient        *client = HD_COMP_MGR_CLIENT (obj);
  HdCompMgrClientPrivate *priv;
  HdCompMgr              *hmgr;
  MBWindowManagerClient  *wm_client = MB_WM_COMP_MGR_CLIENT (obj)->wm_client;
  HdRunningApp          *app;
  guint32                *prop;

  hmgr = HD_COMP_MGR (wm_client->wmref->comp_mgr);

  priv = client->priv = g_new0 (HdCompMgrClientPrivate, 1);

  app = hd_comp_mgr_client_get_app_key (client, hmgr);
  if (app)
    {
      priv->app = g_object_ref (app);
      hd_comp_mgr_client_process_hibernation_prop (client);

      /* Look up if there were already windows for this app. */
      guint windows = (guint)g_hash_table_lookup (hmgr->priv->shown_apps,
                                                  (gpointer)app);
      if (!windows)
        hd_app_mgr_app_opened (app);

      g_hash_table_insert (hmgr->priv->shown_apps,
                           (gpointer)app,
                           (gpointer)++windows);
    }

  /* Set portrait_* initially. */
  prop = hd_util_get_win_prop_data_and_validate (
                              wm_client->wmref->xdpy,
                              wm_client->window->xwindow,
                              hmgr->priv->atoms[HD_ATOM_WM_PORTRAIT_OK],
                              XA_CARDINAL, 32, 1, NULL);

  /* All windows support portrait */
  priv->portrait_supported = TRUE;

  prop = hd_util_get_win_prop_data_and_validate (
                       wm_client->wmref->xdpy,
                       wm_client->window->xwindow,
                       hmgr->priv->atoms[HD_ATOM_WM_PORTRAIT_REQUESTED],
                       XA_CARDINAL, 32, 1, NULL);
  if (prop)
    {
      priv->portrait_requested = *prop;
      XFree (prop);
    }
  else
    priv->portrait_requested_inherited = TRUE;

  g_debug ("portrait properties of %p: supported=%d requested=%d", wm_client,
           priv->portrait_supported_inherited
             ? -1 : priv->portrait_supported,
           priv->portrait_requested_inherited
             ? -1 : priv->portrait_requested);

  return 1;
}

static void
hd_comp_mgr_client_destroy (MBWMObject* obj)
{
  HdCompMgrClientPrivate *priv = HD_COMP_MGR_CLIENT (obj)->priv;

  if (priv->app)
    {
      g_object_unref (priv->app);
      priv->app = NULL;
    }

  g_free (priv);
}

int
hd_comp_mgr_client_class_type ()
{
  static int type = 0;

  if (UNLIKELY(type == 0))
    {
      static MBWMObjectClassInfo info = {
	sizeof (HdCompMgrClientClass),
	sizeof (HdCompMgrClient),
	hd_comp_mgr_client_init,
	hd_comp_mgr_client_destroy,
	hd_comp_mgr_client_class_init
      };

      type =
	mb_wm_object_register_class (&info,
				     MB_WM_TYPE_COMP_MGR_CLUTTER_CLIENT, 0);
    }

  return type;
}

gboolean
hd_comp_mgr_client_is_hibernating (HdCompMgrClient *hclient)
{
  HdCompMgrClientPrivate * priv = hclient->priv;

  if (priv->app)
    return (hd_running_app_is_hibernating (priv->app));

  return FALSE;
}

gboolean
hd_comp_mgr_client_can_hibernate (HdCompMgrClient *hclient)
{
  HdCompMgrClientPrivate * priv = hclient->priv;

  return priv->can_hibernate;
}

GObject *
hd_comp_mgr_client_get_app (HdCompMgrClient *hclient)
{
  if (!hclient) return NULL;
  return G_OBJECT (hclient->priv->app);
}

GObject *
hd_comp_mgr_client_get_launcher (HdCompMgrClient *hclient)
{
  if (!hclient || !hclient->priv->app) return NULL;
  return G_OBJECT (hd_running_app_get_launcher_app(hclient->priv->app));
}

const gchar *
hd_comp_mgr_client_get_app_local_name (HdCompMgrClient *hclient)
{
  HdRunningApp *app = hclient->priv->app;
  if (app)
    {
      HdLauncherApp *launcher = hd_running_app_get_launcher_app (app);
      if (launcher)
        return hd_launcher_item_get_local_name (HD_LAUNCHER_ITEM (launcher));
    }
  return NULL;
}

static int  hd_comp_mgr_init (MBWMObject *obj, va_list vap);
static void hd_comp_mgr_class_init (MBWMObjectClass *klass);
static void hd_comp_mgr_destroy (MBWMObject *obj);
static void hd_comp_mgr_register_client (MBWMCompMgr *mgr,
                                         MBWindowManagerClient *c);
static void hd_comp_mgr_unregister_client (MBWMCompMgr *mgr,
                                           MBWindowManagerClient *c);
static void hd_comp_mgr_map_notify
                        (MBWMCompMgr *mgr, MBWindowManagerClient *c);
static void hd_comp_mgr_unmap_notify
                        (MBWMCompMgr *mgr, MBWindowManagerClient *c);
static void hd_comp_mgr_turn_on (MBWMCompMgr *mgr);
static void hd_comp_mgr_effect (MBWMCompMgr *mgr, MBWindowManagerClient *c,
                                MBWMCompMgrClientEvent event);
static Bool hd_comp_mgr_client_property_changed (XPropertyEvent *event,
                                                 HdCompMgr *hmgr);
static Bool hd_comp_mgr_portrait_forecast (MBWindowManager *wm);

int
hd_comp_mgr_class_type ()
{
  static int type = 0;

  if (type == 0)
    {
      static MBWMObjectClassInfo info = {
        sizeof (HdCompMgrClass),
        sizeof (HdCompMgr),
        hd_comp_mgr_init,
        hd_comp_mgr_destroy,
        hd_comp_mgr_class_init
      };

      type = mb_wm_object_register_class (&info,
					  MB_WM_TYPE_COMP_MGR_CLUTTER, 0);
    }

  return type;
}

static void
hd_comp_mgr_class_init (MBWMObjectClass *klass)
{
  MBWMCompMgrClass *cm_klass = MB_WM_COMP_MGR_CLASS (klass);
  MBWMCompMgrClutterClass * clutter_klass =
    MB_WM_COMP_MGR_CLUTTER_CLASS (klass);

  cm_klass->unregister_client = hd_comp_mgr_unregister_client;
  cm_klass->register_client   = hd_comp_mgr_register_client;
  cm_klass->client_event      = hd_comp_mgr_effect;
  cm_klass->turn_on           = hd_comp_mgr_turn_on;
  cm_klass->map_notify        = hd_comp_mgr_map_notify;
  cm_klass->unmap_notify      = hd_comp_mgr_unmap_notify;
  cm_klass->restack           = (void (*)(MBWMCompMgr*))hd_comp_mgr_restack;

  clutter_klass->client_new   = hd_comp_mgr_client_new;

#if MBWM_WANT_DEBUG
  klass->klass_name = "HDCompMgr";
#endif
}

static int
hd_comp_mgr_init (MBWMObject *obj, va_list vap)
{
  MBWMCompMgr          *cmgr = MB_WM_COMP_MGR (obj);
  MBWindowManager      *wm = cmgr->wm;
  HdCompMgr            *hmgr = HD_COMP_MGR (obj);
  HdCompMgrPrivate     *priv;
  ClutterActor         *stage;
  ClutterActor         *arena;

  priv = hmgr->priv = g_new0 (HdCompMgrPrivate, 1);

  hd_mb_wm = wm;

  hd_atoms_init (wm->xdpy, priv->atoms);

  priv->dbus_connection = hd_dbus_init (hmgr);

  stage = clutter_stage_get_default ();

  /*
   * Create the home group before the switcher, so the switcher can
   * connect it's signals to it.
   */
  priv->home = g_object_new (HD_TYPE_HOME, "comp-mgr", cmgr, NULL);

  clutter_actor_set_reactive (priv->home, TRUE);

  clutter_actor_show (priv->home);

  priv->task_nav = hd_task_navigator_new ();

  priv->render_manager = 
    HD_RENDER_MANAGER (g_object_new (HD_TYPE_RENDER_MANAGER,
		  		     "comp-mgr", hmgr,
		  		     "launcher", hd_launcher_get (),
		  		     "home", HD_HOME (priv->home),
		  		     "task-navigator", priv->task_nav,
		  		     NULL));
 
  g_object_ref (priv->render_manager);
 
  clutter_container_add_actor (CLUTTER_CONTAINER (stage),
                               CLUTTER_ACTOR (priv->render_manager));

  /* Pass the render manager to the app mgr so it knows when it can't
   * prestart apps.
   */
  priv->app_mgr = g_object_ref (hd_app_mgr_get ());
  hd_app_mgr_set_render_manager (G_OBJECT (priv->render_manager));

  /* NB -- home must be constructed before constructing the switcher;
   */
  priv->switcher_group = g_object_new (HD_TYPE_SWITCHER,
				       "comp-mgr", cmgr,
				       "task-nav", priv->task_nav,
				       NULL);

  /* When a MBWMCompMgrClutterClient is first created, it is added to the arena.
   * This will cause a redraw unless we stop the arena from causing a screen
   * redraw. When we want a window rendered, it is pulled out into
   * hd-render-manager.*/
  arena = mb_wm_comp_mgr_clutter_get_arena(MB_WM_COMP_MGR_CLUTTER(cmgr));
  if (arena)
    {
#ifdef MAEMO_CHANGES
      clutter_actor_set_allow_redraw(arena, FALSE);
#endif
      clutter_actor_hide(arena);
      g_object_unref(arena); /* mb_wm_comp_mgr_clutter_get_arena refs us */
    }

  /*
   * Create hash tables for keeping active apps and hibernating windows.
   */
  priv->shown_apps =
    g_hash_table_new_full (g_direct_hash,
                           g_direct_equal,
                           NULL,
                           NULL);
  priv->hibernating_apps =
    g_hash_table_new_full (g_direct_hash,
			   g_direct_equal,
			   NULL,
			   (GDestroyNotify)mb_wm_object_unref);

  /* Be notified about all X window property changes around here. */
  priv->property_changed_cb_id = mb_wm_main_context_x_event_handler_add (
                   cmgr->wm->main_ctx, None, PropertyNotify,
                   (MBWMXEventFunc)hd_comp_mgr_client_property_changed, cmgr);

  /* Rotate the desktop if matchobox thinks a new client
   * will request it soon. */
  mb_wm_object_signal_connect (MB_WM_OBJECT (cmgr->wm),
                  MBWindowManagerSignalPortraitForecast,
                  (MBWMObjectCallbackFunc)hd_comp_mgr_portrait_forecast,
                  NULL);

  hd_render_manager_set_state(HDRM_STATE_HOME);
  
  clutter_stage_show_cursor (CLUTTER_STAGE (stage));

  return 1;
}

static void
hd_comp_mgr_destroy (MBWMObject *obj)
{
  HdCompMgrPrivate * priv = HD_COMP_MGR (obj)->priv;

  if (priv->shown_apps)
    g_hash_table_destroy (priv->shown_apps);
  if (priv->hibernating_apps)
    g_hash_table_destroy (priv->hibernating_apps);
  if (priv->app_mgr)
    {
      g_object_unref (priv->app_mgr);
      priv->app_mgr = NULL;
    }

  g_object_unref (priv->render_manager);

  mb_wm_main_context_x_event_handler_remove (
                                     MB_WM_COMP_MGR (obj)->wm->main_ctx,
                                     PropertyNotify,
                                     priv->property_changed_cb_id);

  if (priv->stack_sync)
    g_source_remove (priv->stack_sync);
}

HdCompMgrClient *
hd_comp_mgr_get_current_client (HdCompMgr *hmgr)
{
  HdCompMgrPrivate * priv = hmgr->priv;

  return priv->current_hclient;
}

static inline gboolean
hd_comp_mgr_client_looks_better_composited (MBWindowManagerClient *c)
{
  if (MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeStatusArea
      || MB_WM_CLIENT_CLIENT_TYPE (c) == MBWMClientTypeOverride
      || MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeHomeApplet
      || HD_IS_INCOMING_EVENT_NOTE (c)
      /* ...or application that wants non-composited mode */
      || (HD_IS_APP (c) && hd_comp_mgr_is_non_composited (c, FALSE)))
    {
      return FALSE;
    }
  return TRUE;
}

static gboolean
hd_comp_mgr_client_prefers_compositing (MBWindowManagerClient *c)
{
  if (MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeStatusArea
      || MB_WM_CLIENT_CLIENT_TYPE (c) == MBWMClientTypeMenu
      || MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeAppMenu
      || MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeStatusMenu
      || MB_WM_CLIENT_CLIENT_TYPE (c) == MBWMClientTypeOverride
      || MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeHomeApplet
      || HD_IS_INFO_NOTE (c)
      || HD_IS_BANNER_NOTE (c)
      || HD_IS_INCOMING_EVENT_NOTE (c)
      /* ...or application that wants non-composited mode */
      || (HD_IS_APP (c) && hd_comp_mgr_is_non_composited (c, FALSE)))
    {
      return hd_comp_mgr_client_looks_better_composited (c);
    }
  return TRUE;
}

/* Called on #PropertyNotify to handle changes to
 * _HILDON_PORTRAIT_MODE_SUPPORT and _HILDON_PORTRAIT_MODE_REQUEST
 * and _HILDON_APP_KILLABLE and _HILDON_ABLE_TO_HIBERNATE
 * and _HILDON_DO_NOT_DISTURB and _HILDON_NOTIFICATION_THREAD. */
Bool
hd_comp_mgr_client_property_changed (XPropertyEvent *event, HdCompMgr *hmgr)
{
  static gint32 idontcare[] = { -1 };
  Atom pok, prq, killable, able_to_hibernate, dnd, nothread;
  gboolean non_comp_changed;
  gint32 *value;
  MBWindowManager *wm;
  HdCompMgrClient *cc;
  MBWindowManagerClient *c;

  if (event->type != PropertyNotify)
    return True;

  killable = hd_comp_mgr_get_atom (hmgr, HD_ATOM_HILDON_APP_KILLABLE);
  able_to_hibernate = hd_comp_mgr_get_atom (hmgr,
                          HD_ATOM_HILDON_ABLE_TO_HIBERNATE);
  dnd = hd_comp_mgr_get_atom (hmgr, HD_ATOM_HILDON_DO_NOT_DISTURB);

  wm = MB_WM_COMP_MGR (hmgr)->wm;

  if (event->atom==
    hd_comp_mgr_get_atom (hmgr, HD_ATOM_HILDON_WM_WINDOW_PROGRESS_INDICATOR) ||
      event->atom==
    hd_comp_mgr_get_atom (hmgr, HD_ATOM_HILDON_WM_WINDOW_MENU_INDICATOR))
    {
      /* Redraw the title to display/remove the progress indicator or app
       * menu indicator. The title itself will check what the new state should
       * be. NOTE: we have to redo dialog titles here too, so we can't just
       * use hd_title_bar_update. */
      MBWindowManagerClient *top;
      /* previous mb_wm_client_decor_mark_dirty didn't actually cause a redraw,
       * so mark the decor itself dirty */
      top = mb_wm_managed_client_from_xwindow(MB_WM_COMP_MGR (hmgr)->wm,
                                              event->window);
      if (top)
        {
          MBWMList *l = top->decor;
          while (l)
            {
              MBWMDecor *decor = l->data;
              if (decor->type == MBWMDecorTypeNorth)
                mb_wm_decor_mark_dirty (decor);
              l = l->next;
            }
        }
    }

  non_comp_changed = event->atom ==
        hd_comp_mgr_get_atom (hmgr, HD_ATOM_HILDON_NON_COMPOSITED_WINDOW);
  if (event->atom == wm->atoms[MBWM_ATOM_NET_WM_STATE] || non_comp_changed)
    {
      c = mb_wm_managed_client_from_xwindow (wm, event->window);
      if (c && HD_IS_APP (c))
        {
          gboolean client_non_comp;
          MBWindowManagerClient *tmp;
          gboolean found = FALSE;
          /* check if there is a window above that needs compositing */
          for (tmp = c->stacked_above; tmp; tmp = tmp->stacked_above)
            if (mb_wm_client_is_map_confirmed (tmp) &&
                hd_comp_mgr_client_prefers_compositing (tmp))
              {
                found = TRUE;
                break;
              }
          client_non_comp = hd_comp_mgr_is_non_composited (c, non_comp_changed);
          if (hd_render_manager_get_state () == HDRM_STATE_NON_COMPOSITED &&
	      !client_non_comp)
            hd_render_manager_set_state (HDRM_STATE_APP);
          else if (hd_render_manager_get_state () == HDRM_STATE_NON_COMP_PORT
                   && !client_non_comp)
            hd_render_manager_set_state (HDRM_STATE_APP_PORTRAIT);
          else if (hd_render_manager_get_state () == HDRM_STATE_APP &&
                   !hd_transition_is_rotating () &&
	           client_non_comp && !found)
            hd_render_manager_set_state (HDRM_STATE_NON_COMPOSITED);
          else if (hd_render_manager_get_state () == HDRM_STATE_APP_PORTRAIT &&
                   !hd_transition_is_rotating () &&
	           client_non_comp && !found)
            hd_render_manager_set_state (HDRM_STATE_NON_COMP_PORT);
        }
    }
  /* Check for changes to the hibernable state. */
  if (event->atom == killable ||
      event->atom == able_to_hibernate)
    {
      HdRunningApp *app, *current_app;
      c = mb_wm_managed_client_from_xwindow (wm, event->window);
      if (!c || !c->cm_client)
        return False;
      cc = HD_COMP_MGR_CLIENT (c->cm_client);
      if (event->state == PropertyNewValue)
        cc->priv->can_hibernate = TRUE;
      else
        cc->priv->can_hibernate = FALSE;

      /* Change the hibernable state of the app only if it's not the
       * current app.
       */
      app = cc->priv->app;
      if (!app)
        return False;
      current_app =
        HD_RUNNING_APP (hd_comp_mgr_client_get_app (hd_comp_mgr_get_current_client (hmgr)));
      if (!current_app || app == current_app)
        return False;

      if (event->state == PropertyNewValue)
        hd_app_mgr_hibernatable(app, TRUE);
      else
        hd_app_mgr_hibernatable (app, FALSE);

      return False;
    }

  if (event->atom == dnd)
    {
      hd_comp_mgr_check_do_not_disturb_flag (hmgr);
      return FALSE;
    }

  nothread = hd_comp_mgr_get_atom (hmgr, HD_ATOM_NOTIFICATION_THREAD);
  if (event->atom == nothread)
    {
      char *str;
      ClutterActor *a;
      HdTaskNavigator *tasw;

      c = mb_wm_managed_client_from_xwindow (wm, event->window);
      if (!c || !c->cm_client)
        return False;
      tasw = HD_TASK_NAVIGATOR (hmgr->priv->task_nav);
      a = mb_wm_comp_mgr_clutter_client_get_actor (
                          MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client));
      str = event->state == PropertyNewValue
        ? hd_util_get_x_window_string_property (wm, c->window->xwindow,
                                                HD_ATOM_NOTIFICATION_THREAD)
        : NULL;
      if (event->state != PropertyNewValue || str)
        /* Otherwise don't mess up more. */
        hd_task_navigator_notification_thread_changed (tasw, a, str);
      return False;
    }

  /* Process PORTRAIT flags */
  pok = hd_comp_mgr_get_atom (hmgr, HD_ATOM_WM_PORTRAIT_OK);
  prq = hd_comp_mgr_get_atom (hmgr, HD_ATOM_WM_PORTRAIT_REQUESTED);
  if (event->atom != pok && event->atom != prq)
    return True;

  /* Read the new property value. */
  if (event->state == PropertyNewValue)
    value = hd_util_get_win_prop_data_and_validate (wm->xdpy, event->window,
                                                    event->atom, XA_CARDINAL,
                                                    32, 1, NULL);
  else
    value = idontcare;
  if (!value)
    goto out0;

  /* Get the client whose property changed. */
  if (!(c = mb_wm_managed_client_from_xwindow (wm, event->window)))
    /* Care about fullscreen clients. */
    c = mb_wm_managed_client_from_frame (wm, event->window);
  if (!c || !(cc = HD_COMP_MGR_CLIENT (c->cm_client)))
    goto out1;

  /* Process the property value. */
  if (event->atom == pok)
    {
      cc->priv->portrait_supported            = *value > 0;
      cc->priv->portrait_supported_inherited  = *value < 0;
    }
  else
    {
      cc->priv->portrait_requested            = *value > 0 ? *value : 0;
      cc->priv->portrait_requested_inherited  = *value < 0;
    }
  g_debug ("portrait property of %p changed: supported=%d requested=%d", c,
           cc->priv->portrait_supported_inherited
             ? -1 : cc->priv->portrait_supported,
           cc->priv->portrait_requested_inherited
             ? -1 : cc->priv->portrait_requested);

  /* Switch HDRM state if we need to.  Don't consider changing the state if
   * it is approved by the new value of the property.  We must reconsider
   * if we don't know if the property appoves or not. */
  if (STATE_IS_PORTRAIT (hd_render_manager_get_state()))
    { /* Portrait => landscape? */
      if (*value <= 0 && !hd_comp_mgr_should_be_portrait (hmgr))
        hd_render_manager_set_state_unportrait ();
    }
  else if (STATE_IS_PORTRAIT_CAPABLE (hd_render_manager_get_state()))
    { /* Landscape => portrait? */
      if (*value != 0 && hd_comp_mgr_should_be_portrait (hmgr))
        hd_render_manager_set_state_portrait ();
    }

out1:
  if (value != idontcare)
    XFree (value);
out0:
  return False;
}

/* %MBWindowManagerSignalPortraitForecast callback */
static Bool
hd_comp_mgr_portrait_forecast (MBWindowManager *wm)
{
  gboolean activate;
  MBWindowManagerClient *c;

  /*
   * Be very conservative about commencing a transition, try to err on
   * the safe side, otherwise we may confuse the HDRM state.  This means
   * if we're moving -> activate because there's no guarantee the transition
   * will change orientation in the end.  If we're actually staying in
   * portrait there's nothing to worry about and we can safely activate.
   * Otherwise (we stay in landscape) check the window stack and see if
   * any dialogs don't agree with rotation.  This is important if it is
   * an application which is forecast here because dialogs are in front
   * of apps.
   */
  activate = hd_transition_is_rotating () || hd_comp_mgr_is_portrait ();
  if (!activate)
    {
      for (c = wm->stack_top; c && c != wm->desktop; c = c->stacked_below)
        {
          if (!mb_wm_client_is_map_confirmed (c))
            {
              if (c->window->portrait_on_map > 1)
                /* Forceful client, surrender. */
                break;
              else
                continue;
            }

          if (HD_COMP_MGR_CLIENT (c->cm_client)->priv->portrait_supported)
            /* Agrees. */
            continue;

          if (c->transient_for)
            /* Doesn't matter. */
            continue;

          if ((MB_WM_CLIENT_CLIENT_TYPE (c) & MBWMClientTypeDialog)
              || HD_IS_INFO_NOTE (c) || HD_IS_CONFIRMATION_NOTE (c))
            { /* Dealbreaker. */
              activate = 1;
              break;
            }
        }
    }

  if (!activate)
    /* libmatchbox believes we'd rotate soon, so let's do it beforehand. */
    hd_transition_rotate_screen (wm, TRUE);

  /* See who caused us the pain? */
  for (c = wm->stack_top; c; c = c->stacked_below)
    if (!mb_wm_client_is_map_confirmed (c) && c->window->portrait_on_map)
      {
        if (activate)
          {
            /* Needn't distract from normal operation. */
            mb_wm_activate_client (wm, c);

            /* There might be similar clients further down the stack,
             * but we can activate only one at a time. */
            break;
          }
        else
          /* Bastard hack part 1: hide it until the screen is reconfigured,
           * then libmatchbox will activate it. */
          mb_wm_client_hide (c);
      }

  return False;
}

static void
hd_comp_mgr_turn_on (MBWMCompMgr *mgr)
{
  MBWMCompMgrClass * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));

  /*
   * The parent class turn_on method deals with setting up the input shape on
   * the overlay window; so we first call it, and then change the shape to
   * suit our custom needs.
   */
  if (parent_klass->turn_on)
    parent_klass->turn_on (mgr);

}


static void
hd_comp_mgr_register_client (MBWMCompMgr           * mgr,
			     MBWindowManagerClient * c)
{
  HdCompMgrPrivate              * priv = HD_COMP_MGR (mgr)->priv;
  MBWMCompMgrClass              * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));

  g_debug ("%s, c=%p ctype=%d", __FUNCTION__, c, MB_WM_CLIENT_CLIENT_TYPE (c));
  if (MB_WM_CLIENT_CLIENT_TYPE (c) == MBWMClientTypeDesktop)
    {
      priv->desktop = c;
      return;
    }
  else if (MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeKeyboard)
    {
      priv->keyboard = c;
    }

  if (parent_klass->register_client)
    parent_klass->register_client (mgr, c);
}


static void
hd_comp_mgr_unregister_client (MBWMCompMgr *mgr, MBWindowManagerClient *c)
{
  ClutterActor                  * actor;
  HdCompMgrPrivate              * priv = HD_COMP_MGR (mgr)->priv;
  MBWMCompMgrClass              * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));
  MBWMCompMgrClutterClient      * cclient =
    MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);
  HdCompMgrClient               * hclient = HD_COMP_MGR_CLIENT (c->cm_client);

  g_debug ("%s, c=%p ctype=%d", __FUNCTION__, c, MB_WM_CLIENT_CLIENT_TYPE (c));
  actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);

  /* Check if it's the last window for the app. */
  if (hclient->priv->app)
    {
      HdRunningApp *app = hclient->priv->app;
      guint windows = (guint)g_hash_table_lookup (priv->shown_apps,
                                                  (gpointer)app);
      if (--windows == 0)
        {
          hd_app_mgr_app_closed (app);
          g_hash_table_remove (priv->shown_apps, (gpointer)app);
        }
      else
        {
          g_hash_table_insert (priv->shown_apps,
                               (gpointer)app,
                               (gpointer)windows);
        }
    }

  /*
   * If the actor is an application, remove it also to the switcher
   */
  if (hclient->priv->app &&
      hd_running_app_is_hibernating (hclient->priv->app) &&
      !g_hash_table_lookup (priv->hibernating_apps,
                            (gpointer) hclient->priv->hibernation_key))
    {
      /*
       * We want to hold onto the CM client object, so we can continue using
       * the actor.
       */
      mb_wm_comp_mgr_clutter_client_set_flags (cclient,
                                MBWMCompMgrClutterClientDontUpdate);
      mb_wm_object_ref (MB_WM_OBJECT (cclient));

      g_hash_table_insert (priv->hibernating_apps,
			   (gpointer) hclient->priv->hibernation_key,
			   hclient);

      hd_switcher_hibernate_window_actor (priv->switcher_group,
					  actor);
    }
  else if (MB_WM_CLIENT_CLIENT_TYPE (c) == MBWMClientTypeApp)
    {
      HdApp *app = HD_APP(c);
      MBWMCompMgrClutterClient * prev = NULL;

      if (actor)
        {
	      gboolean topmost;

	      if (app->stack_index < 0 /* non-stackable */
		  /* leader without secondarys: */
	          || (app->leader == app && !app->leader->followers) ||
	          /* or a secondary window on top of the stack: */
	          (app->leader != NULL &&
	           app->leader->followers &&
                   app == g_list_last (app->leader->followers)->data))
	        topmost = 1;
	      else
	        topmost = 0;

              /* if we are secondary, there must be leader and probably
	       * even followers */
              if (app->stack_index > 0 && app->leader != app)
                {
                  g_assert(app->leader);
                  g_debug ("%s: %p is STACKABLE SECONDARY", __func__, app);
                  /* show the topmost follower and replace switcher actor
		   * for the stackable */

		  /* remove this window from the followers list */
		  app->leader->followers
		  	= g_list_remove (app->leader->followers, app);

		  if (app->leader->followers)
		    prev = MB_WM_COMP_MGR_CLUTTER_CLIENT (
			     MB_WM_CLIENT (
		               g_list_last (app->leader->followers)->data)
			                     ->cm_client);
		  else
		    prev = MB_WM_COMP_MGR_CLUTTER_CLIENT (
		             MB_WM_CLIENT (app->leader)->cm_client);

		  if (topmost) /* if we were on top, update the switcher */
		  {
                    ClutterActor *pactor;
                    pactor = mb_wm_comp_mgr_clutter_client_get_actor (prev);
                    if (pactor)
                    {
                      clutter_actor_show (pactor);
		      g_debug ("%s: REPLACE ACTOR %p WITH %p", __func__,
                               actor, pactor);
                      hd_switcher_replace_window_actor (priv->switcher_group,
                                                        actor, pactor);
                    }
                    else
                      g_warning ("%s: leader or next secondary not found",
                                 __func__);
		  }
                }
              else if (!(c->window->ewmh_state &
		         MBWMClientWindowEWMHStateSkipTaskbar) &&
		       (app->stack_index < 0 ||
		       (app->leader == app && !app->followers)))
                {
                  g_debug ("%p: NON-STACKABLE OR FOLLOWERLESS LEADER"
			   " (index %d), REMOVE ACTOR %p",
		           __func__, app->stack_index, actor);
                  /* We are the leader or a non-stackable window,
                   * just remove the actor from the switcher.
                   * NOTE The test above breaks if the client changed
                   * the flag after it's been mapped. */
                  hd_switcher_remove_window_actor (priv->switcher_group,
                                                   actor, cclient);

                  if (c->window->xwindow == hd_wm_current_app_is (NULL, 0) &&
                       (app->detransitised_from == None ||
                        !mb_wm_managed_client_from_xwindow (mgr->wm, app->detransitised_from)))
		    {
		      /* We are in APP state and foreground application closed.
                       * hdrm is grown-up enough to figure out if it shouldn't
                       * go to tasw for some reason. */
                      hd_render_manager_set_state (HDRM_STATE_TASK_NAV);
		    }
                }
	      else if (app->leader == app && app->followers)
	        {
                  GList *l;
		  HdApp *new_leader;
                  g_debug ("%s: STACKABLE LEADER %p (index %d) WITH CHILDREN",
			   __func__, app, app->stack_index);

                  prev = MB_WM_COMP_MGR_CLUTTER_CLIENT (
			   hd_app_get_prev_group_member(app)->cm_client);
		  new_leader = HD_APP (app->followers->data);
                  for (l = app->followers; l; l = l->next)
                  {
		    /* bottommost secondary is the new leader */
                    HD_APP (l->data)->leader = new_leader;
		  }
                  /* set the new leader's followers list */
                  new_leader->followers = g_list_remove (app->followers,
                                                         new_leader);
		  /* disconnect the app */
                  app->followers = NULL; /* list is now in new_leader */
                  app->leader = NULL;
                  app->stack_index = -1;
		}
	      else
                { g_warning("%s:%u: what am i doing here?",
                            __FUNCTION__, __LINE__);
                  MBWindowManagerClient *current_client =
                          hd_comp_mgr_determine_current_app ();

                  if (STATE_IS_APP (hd_render_manager_get_state ()) &&
                      MB_WM_CLIENT_CLIENT_TYPE (current_client) &
                      MBWMClientTypeDesktop)
                    hd_render_manager_set_state (HDRM_STATE_TASK_NAV);
                }
          g_object_set_data (G_OBJECT (actor), "HD-ApplicationId", NULL);
        }
    }
  else if (MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeStatusArea)
    {
      hd_home_remove_status_area (HD_HOME (priv->home), actor);
      priv->status_area_client = NULL;
    }
  else if (MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeStatusMenu)
    {
      hd_home_remove_status_menu (HD_HOME (priv->home), actor);
      priv->status_menu_client = NULL;
    }
  else if (MB_WM_CLIENT_CLIENT_TYPE (c) == HdWmClientTypeHomeApplet)
    {
      ClutterActor *applet = mb_wm_comp_mgr_clutter_client_get_actor (cclient);

      /* Unregister applet from Home */
      if (applet)
        hd_home_unregister_applet (HD_HOME (priv->home),
                                   applet);
    }

  if (priv->current_hclient == hclient)
    priv->current_hclient = NULL;

  /*
   * We have the following situation to prevent: a client is forecast
   * as portrait-wanting, and we started to rotate but we're not in a
   * STATE_IS_PORTRAIT yet, but the client disappears in the meantime
   * without even being mapped.  If we don't do anything about it we
   * will remain rotated but not in STATE_IS_PORTRAIT().
   */
  if (!STATE_IS_PORTRAIT (hd_render_manager_get_state ())
      && hd_transition_is_rotating_to_portrait ()
      && !mb_wm_client_is_map_confirmed (c)
      && c->window->portrait_on_map)
    hd_transition_rotate_screen (mgr->wm, FALSE);

  /* Dialogs and Notes (including notifications) have already been dealt
   * with in hd_comp_mgr_effect().  This is because by this time we don't
   * have information about transiency. */

  if (parent_klass->unregister_client)
    parent_klass->unregister_client (mgr, c);
}

/* Returns the client @c is transient for.  Some clients (notably menus)
 * don't have their c->transient_for field set even though they are
 * transient.  Figure it out from the c->window in this case. */
static MBWindowManagerClient *
hd_comp_mgr_get_client_transient_for (MBWindowManagerClient *c)
{
  Window xtransfor;

  if (c->transient_for)
    return c->transient_for;

  xtransfor = c->window->xwin_transient_for;
  return xtransfor && xtransfor != c->window->xwindow
      && xtransfor != c->wmref->root_win->xwindow
    ? mb_wm_managed_client_from_xwindow (c->wmref, xtransfor)
    : NULL;
}

static void
hd_comp_mgr_texture_update_area(HdCompMgr *hmgr,
                                int x, int y, int width, int height,
                                ClutterActor* actor)
{
  ClutterActor *parent;
  HdCompMgrPrivate * priv;
  gboolean blur_update = FALSE;
  ClutterActor *actors_stage;

  if (!actor || !CLUTTER_ACTOR_IS_VISIBLE(actor) || hmgr == 0)
    return;

  /* If we are in the blanking period of the rotation transition
   * then we don't want to issue a redraw every time something changes.
   * This function also assumes that it is called because there was damage,
   * and makes sure it prolongs the blanking period a bit.
   */
  if (hd_transition_rotate_ignore_damage())
    return;

  priv = hmgr->priv;

  /* TFP textures are usually bundled into another group, and it is
   * this group that sets visibility - so we must check it too */
  parent = clutter_actor_get_parent(actor);
  actors_stage = clutter_actor_get_stage(actor);
  if (!actors_stage)
    /* if it's not on stage, it's not visible */
    return;

  while (parent && parent != actors_stage)
    {
      if (!CLUTTER_ACTOR_IS_VISIBLE(parent))
        return;
      /* if we're a child of a blur group, tell it that it has changed */
      if (TIDY_IS_BLUR_GROUP(parent))
        {
          /* we don't update blur on every change of
           * an application now as it causes a flicker, so
           * instead we just hint that next time we become
           * unblurred, we need to recalculate. */
          tidy_blur_group_hint_source_changed(parent);
          /* ONLY set blur_update if the image is buffered ->
           * we are actually blurred */
          if (tidy_blur_group_source_buffered(parent))
            blur_update = TRUE;
        }
      parent = clutter_actor_get_parent(parent);
    }

  /* We no longer display changes that occur on blurred windows, so if
   * this damage was actually on a blurred window, forget about it. */
  if (blur_update)
    return;

  /* Update the screen. This function checks for scaling/visibility and
   * chooses the area to update accordingly */
  {
    ClutterGeometry area = {x,y,width, height};
    hd_util_partial_redraw_if_possible(actor, &area);
  }
}

/* Hook onto and X11 texture pixmap children of this actor */
static void
hd_comp_mgr_hook_update_area(HdCompMgr *hmgr, ClutterActor *actor)
{
  if (CLUTTER_IS_GROUP(actor))
    {
      gint i;
      gint n = clutter_group_get_n_children(CLUTTER_GROUP(actor));

      for (i=0;i<n;i++)
        {
          ClutterActor *child =
              clutter_group_get_nth_child(CLUTTER_GROUP(actor), i);
          if (CLUTTER_X11_IS_TEXTURE_PIXMAP(child))
            {
              g_signal_connect_swapped(
                      G_OBJECT(child), "update-area",
                      G_CALLBACK(hd_comp_mgr_texture_update_area), hmgr);
#ifdef MAEMO_CHANGES
              clutter_actor_set_allow_redraw(child, FALSE);
#endif
            }
        }
    }
}

static void
fix_transiency (MBWindowManagerClient *client)
{
  MBWindowManager       *wm = client->wmref;
  MBWMClientWindow      *win = client->window;

  if (win->xwin_transient_for
      && win->xwin_transient_for != win->xwindow
      && win->xwin_transient_for != wm->root_win->xwindow)
    {
      MBWindowManagerClient *trans_parent;

      trans_parent = mb_wm_managed_client_from_xwindow (wm,
                      win->xwin_transient_for);

      if (trans_parent)
        {
          g_debug("%s: setting %lx transient to %lx\n", __FUNCTION__,
                 win->xwindow, win->xwin_transient_for);
          mb_wm_client_add_transient (trans_parent, client);
        }

      /* this change can affect stacking order */
      mb_wm_client_stacking_mark_dirty (client);
    }
  else
    g_debug("%s: DO NOTHING %lx is transient to %lx\n", __FUNCTION__,
                 win->xwindow, win->xwin_transient_for);
}

/* set composite overlay shape according to our state */
void hd_comp_mgr_reset_overlay_shape (HdCompMgr *hmgr)
{
  static gboolean    fs_comp = TRUE;
  gboolean           want_fs_comp;
  MBWMCompMgr       *mgr = MB_WM_COMP_MGR (hmgr);
  MBWindowManager   *wm;
  Window             clutter_window;
  ClutterActor      *stage;

  if (hd_render_manager_get_state () == HDRM_STATE_NON_COMPOSITED ||
      hd_render_manager_get_state () == HDRM_STATE_NON_COMP_PORT)
    want_fs_comp = FALSE;
  else
    want_fs_comp = TRUE;

  if (want_fs_comp == fs_comp)
    return;

  wm = mgr->wm;
  stage = clutter_stage_get_default ();
  clutter_window = clutter_x11_get_stage_window (CLUTTER_STAGE (stage));

  if (want_fs_comp) {
    /* Recreate the overlay window and move stuff back */
    mgr->disabled = True;
    XSetWindowBackgroundPixmap(wm->xdpy, clutter_window, None);
    hd_comp_mgr_turn_on(mgr);
    XMoveWindow(wm->xdpy, clutter_window, 0, 0);
    XSetWindowBackground(wm->xdpy, clutter_window,
                         BlackPixel(wm->xdpy, DefaultScreen(wm->xdpy)));

    /* g_printerr ("%s: COMPOSITING: FULL SCREEN\n", __FUNCTION__); */
#ifdef MAEMO_CHANGES
    clutter_stage_set_shaped_mode (stage, 0);
#endif
  } else {
    /* g_printerr ("%s: COMPOSITING: ZERO REGION\n", __FUNCTION__); */
    /* Change the stage background to None before we do anything, to avoid
     * ugly black flashes. */
    XSetWindowBackgroundPixmap(wm->xdpy, clutter_window, None);
    /* tell Clutter not to draw on the window */
#ifdef MAEMO_CHANGES
    clutter_stage_set_shaped_mode (stage, 1);
#endif
    /* Reparent X back to the root window - and move it offscreen, then
     * reset its background to black. */
    XReparentWindow (wm->xdpy, clutter_window, wm->root_win->xwindow, 0, 0);
    XMoveWindow(wm->xdpy, clutter_window, 0, -800);
    XSetWindowBackground(wm->xdpy, clutter_window,
                         BlackPixel(wm->xdpy, DefaultScreen(wm->xdpy)));
    /* Kill the overlay window */
    XCompositeReleaseOverlayWindow (wm->xdpy, wm->root_win->xwindow);
    mb_wm_comp_mgr_clutter_set_overlay_window(
        MB_WM_COMP_MGR_CLUTTER(hmgr), None);
  }

  fs_comp = want_fs_comp;
}

/* 'force' allows unredirecting non-fullscreen applications, it is used
 * for the key shortcut (handy when checking for compositing glitches) */
void
hd_comp_mgr_unredirect_topmost_client (MBWindowManager *wm, gboolean force)
{
  MBWindowManagerClient *c, *unredir_client = NULL;

  for (c = wm->stack_top; c && c != wm->desktop; c = c->stacked_below)
    {
      /* unredirect and do not track damage of the topmost
       * application window that is fullscreen */
      if (c->cm_client && c->window->net_type ==
            wm->atoms[MBWM_ATOM_NET_WM_WINDOW_TYPE_NORMAL] &&
          (c->window->ewmh_state & MBWMClientWindowEWMHStateFullscreen
           || force))
        {
          if (!mb_wm_comp_mgr_clutter_client_is_unredirected (c->cm_client))
            {
              mb_wm_comp_mgr_clutter_client_track_damage (
                MB_WM_COMP_MGR_CLUTTER_CLIENT(c->cm_client), False);
              mb_wm_comp_mgr_clutter_set_client_redirection (c->cm_client,
                                                             FALSE);
              unredir_client = c;
            }
          break;
        }
    }
  /*
  if (unredir_client)
    g_printerr ("%s: unredirected client '%s'\n", __func__,
                mb_wm_client_get_name (unredir_client));
                */
}

static void
hd_comp_mgr_unredirect_client (MBWindowManagerClient *c)
{
  if (c->cm_client &&
      !mb_wm_comp_mgr_clutter_client_is_unredirected (c->cm_client))
    {
      mb_wm_comp_mgr_clutter_client_track_damage (
        MB_WM_COMP_MGR_CLUTTER_CLIENT(c->cm_client), False);
      mb_wm_comp_mgr_clutter_set_client_redirection (c->cm_client,
                                                     FALSE);
    }
  else
    g_printerr("%s: ain't no doing no unredirection\n", __func__);
}

/* returns TRUE if the client wants non-composited mode */
static gboolean
hd_comp_mgr_is_non_composited (MBWindowManagerClient *client,
                               gboolean force_re_read)
{
  MBWindowManager *wm;
  HdCompMgr *hmgr;
  MBWMClientWindow *win;
  Atom atom, actual_type;
  int format;
  unsigned long items, left;
  unsigned char *prop;
  Status ret;

  if (!HD_IS_APP (client))
    return FALSE;

  wm = client->wmref;

  if (!HD_APP (client)->non_composited_read)
    {
      /* check if the window is blacklisted */
      XClassHint class_hint;
      memset (&class_hint, 0, sizeof (XClassHint));
      mb_wm_util_async_trap_x_errors (wm->xdpy);
      ret = XGetClassHint (wm->xdpy, client->window->xwindow, &class_hint);
      mb_wm_util_async_untrap_x_errors ();

      if (ret && class_hint.res_class)
        {
          if (!strcmp (class_hint.res_class, "Chessui") ||
              !strcmp (class_hint.res_class, "Mahjong"))
            {
              /* g_printerr ("%s: mahjong or chess\n", __func__); */
              HD_APP (client)->non_composited_read = True;
              HD_APP (client)->non_composited = False;
              HD_APP (client)->force_composited = True;
            }
        }

      if (class_hint.res_class)
        XFree (class_hint.res_class);

      if (class_hint.res_name)
        XFree (class_hint.res_name);
    }

  if (HD_APP (client)->force_composited)
    return FALSE;

  if (HD_APP (client)->non_composited_read && !force_re_read)
    {
      if (client->window->ewmh_state & MBWMClientWindowEWMHStateFullscreen &&
          (HD_APP (client)->non_composited ||
           /* non-stackable */
           HD_APP (client)->stack_index < 0))
        return TRUE;
      else
        return FALSE;
    }

  hmgr = HD_COMP_MGR (wm->comp_mgr);
  win = client->window;
  prop = NULL;

  atom = hd_comp_mgr_get_atom (hmgr, HD_ATOM_HILDON_NON_COMPOSITED_WINDOW);

  mb_wm_util_async_trap_x_errors (wm->xdpy);
  ret = XGetWindowProperty (wm->xdpy, win->xwindow,
                            atom, 0, 1, False,
                            XA_INTEGER, &actual_type, &format,
                            &items, &left, &prop);
  mb_wm_util_async_untrap_x_errors ();
  if (ret != Success)
    return FALSE;

  HD_APP (client)->non_composited_read = True;

  if (prop)
    XFree (prop);

  if (actual_type == XA_INTEGER)
    {
      HD_APP (client)->non_composited = True;
      if (client->window->ewmh_state & MBWMClientWindowEWMHStateFullscreen)
        return TRUE;
      else
        return FALSE;
    }
  else
   {
     HD_APP (client)->non_composited = False;
     if (client->window->ewmh_state & MBWMClientWindowEWMHStateFullscreen &&
         HD_APP (client)->stack_index < 0)
       return TRUE;
     else
       return FALSE;
   }
}

/* returns HdApp of client that was replaced, or NULL */
static void
hd_comp_mgr_handle_stackable (MBWindowManagerClient *client,
		              HdApp **replaced, HdApp **add_to_tn)
{
  MBWindowManager       *wm = client->wmref;
  MBWMClientWindow      *win = client->window;
  HdCompMgr             *hmgr = HD_COMP_MGR (wm->comp_mgr);
  HdApp                 *app = HD_APP (client);
  Window                 win_group;
  unsigned char         *prop = NULL;
  unsigned long          items, left;
  int			 format;
  Atom                   stack_atom, actual_type;
  Status                ret;

  app->stack_index = -1;  /* initially a non-stackable */
  *replaced = *add_to_tn = NULL;

  fix_transiency (client);
  stack_atom = hd_comp_mgr_get_atom (hmgr, HD_ATOM_HILDON_STACKABLE_WINDOW);

  mb_wm_util_async_trap_x_errors (wm->xdpy);
  /*
   * XGetWindowProperty() is a synchronization point so any errors reported
   * through the X error handler is most probably due to a bug earlier.
   * Let's hide the crap under the carpet and ignore it.
   *
   * It doesn't make much a difference because the errors would be trapped
   * and ignored anyway because of various bugs in matchbox (errors being
   * trapped twice).  Let's pretend they're not.
   */
  ret = XGetWindowProperty (wm->xdpy, win->xwindow,
                            stack_atom, 0, 1, False,
                            XA_INTEGER, &actual_type, &format,
                            &items, &left, &prop);
  mb_wm_util_async_untrap_x_errors ();
  if (ret != Success)
    /* Now, the call really failed. */
    return;

  if (actual_type == XA_INTEGER)
    {
      MBWindowManagerClient *c_tmp;
      HdApp *old_leader = NULL;
      HdApp *last_follower = NULL;

      win_group = win->xwin_group;
      app->stack_index = (int)*prop;
      g_debug ("%s: STACK INDEX %d\n", __func__, app->stack_index);

      mb_wm_stack_enumerate (wm, c_tmp)
        if (c_tmp != client &&
            MB_WM_CLIENT_CLIENT_TYPE (c_tmp) == MBWMClientTypeApp &&
            HD_APP (c_tmp)->stack_index >= 0 /* == stackable window */ &&
            c_tmp->window->xwin_group == win_group)
          {
	    /*
	     * It is possible that the bottommost window is mapped but we did
	     * not get a map notify yet. In this case the leader can be found
	     * higher (see NB#121902).
	     */
            if (mb_wm_client_is_map_confirmed(c_tmp))
              {
                old_leader = HD_APP (c_tmp)->leader;
                break;
              }
          }

      if (old_leader && old_leader->followers)
        last_follower = HD_APP (g_list_last (old_leader->followers)->data);

      if (old_leader && app->stack_index <= old_leader->stack_index)
        {
          GList *l;

          if (app == old_leader)
            /* ... like killing app->followers */
            g_critical ("%s: app == old_leader == %p, "
                        "i'm about to do silly things",
                        __FUNCTION__, app);

          app->leader = app;
          for (l = old_leader->followers; l; l = l->next)
          {
            HD_APP (l->data)->leader = app;
          }

          if (old_leader->stack_index == app->stack_index)
          {
            /* drop the old leader from the stack if we replace it */
            g_debug ("%s: DROPPING OLD LEADER %p OUT OF THE STACK\n", __func__,
		     app);
            app->followers = old_leader->followers;
            old_leader->followers = NULL;
            old_leader->leader = NULL;
            old_leader->stack_index = -1; /* mark it non-stackable */
	    *replaced = old_leader;
          }
          else
          {
            /* the new leader is now a follower */
            g_debug ("%s: OLD LEADER %p IS NOW A FOLLOWER\n", __func__, app);
            app->followers = g_list_prepend (old_leader->followers,
                                             old_leader);
            old_leader->followers = app->followers;
            old_leader->leader = app;
            fix_transiency ((MBWindowManagerClient*)old_leader);

            /* This forces the decors to be redone, taking into account the
             * stack index. */
            mb_wm_client_theme_change ((MBWindowManagerClient*)old_leader);
            mb_wm_client_theme_change ((MBWindowManagerClient*)app);
	    *replaced = old_leader;
	    if (HD_APP(*replaced)->followers)
	        *replaced = g_list_last (HD_APP(*replaced)->followers)->data;
          }
        }
      else if (app->stack_index > 0 && old_leader &&
	  (!last_follower || last_follower->stack_index < app->stack_index))
        {
          g_debug ("%s: %p is NEW SECONDARY OF THE STACK\n", __FUNCTION__, app);
          app->leader = old_leader;

          app->leader->followers = g_list_append (old_leader->followers,
                                                  client);
	  if (last_follower)
	    *replaced = last_follower;
	  else
	    *replaced = old_leader;
        }
      else if (old_leader && app->stack_index > old_leader->stack_index)
        {
          GList *flink;
          HdApp *f = NULL;

          app->leader = old_leader;
          /* find the follower that the new window replaces or follows */
          for (flink = old_leader->followers; flink; flink = flink->next)
          {
            f = flink->data;
            if (f->stack_index >= app->stack_index)
	    {
	      if (flink->prev &&
	          HD_APP (flink->prev->data)->stack_index == app->stack_index)
	      {
	        f = flink->prev->data;
		flink = flink->prev;
	      }
              break;
	    }
          }
	  if (!f && old_leader->followers &&
	      HD_APP (old_leader->followers->data)->stack_index
	                                             == app->stack_index)
	  {
	    f = old_leader->followers->data;
	    flink = old_leader->followers;
	  }

          if (!f)
          {
            g_debug ("%s: %p is FIRST FOLLOWER OF THE STACK\n", __func__, app);
            old_leader->followers = g_list_append (old_leader->followers, app);
	    *add_to_tn = app;
          }
          else if (f->stack_index == app->stack_index)
          {
	    if (f != app)
	    {
              g_debug ("%s: %p REPLACES A FOLLOWER OF THE STACK\n",
		       __func__, app);
              old_leader->followers
                = g_list_insert_before (old_leader->followers, flink, app);
              old_leader->followers
                = g_list_remove_link (old_leader->followers, flink);
              g_list_free (flink);
              /* drop the replaced follower from the stack */
              f->leader = NULL;
              f->stack_index = -1; /* mark it non-stackable */
	    }
	    else
	      g_debug ("%s: %p is the SAME CLIENT\n", __FUNCTION__, app);
	    *replaced = f;
          }
          else if (f->stack_index > app->stack_index)
          {
            g_debug ("%s: %p PRECEEDS (index %d) A FOLLOWER (with index %d)"
	             " OF THE STACK\n", __func__, app, app->stack_index,
		     f->stack_index);
            old_leader->followers
                = g_list_insert_before (old_leader->followers, flink, app);
            mb_wm_client_theme_change ((MBWindowManagerClient*)app);
          }
	  else  /* f->stack_index < app->stack_index */
	  {
            if (flink && flink->next)
	    {
              g_debug ("%s: %p PRECEEDS (index %d) A FOLLOWER (with index %d)"
	             " OF THE STACK\n", __func__, app, app->stack_index,
		     HD_APP (flink->next->data)->stack_index);
              old_leader->followers
                 = g_list_insert_before (old_leader->followers, flink->next,
				         app);
              mb_wm_client_theme_change ((MBWindowManagerClient*)app);
	    }
	    else
	    {
              g_debug ("%s: %p FOLLOWS LAST FOLLOWER OF THE STACK\n", __func__,
		       app);
              old_leader->followers = g_list_append (old_leader->followers,
                                                     app);
	      *add_to_tn = app;
	    }
	  }
        }
      else  /* we are the first window in the stack */
        {
          g_debug ("%s: %p is FIRST WINDOW OF THE STACK\n", __FUNCTION__, app);
          app->leader = app;
        }
    }

  if (prop)
    XFree (prop);

  /* all stackables have stack_index >= 0 */
  g_assert (!app->leader || (app->leader && app->stack_index >= 0));
}

extern gboolean hd_dbus_tklock_on;

static void
hd_comp_mgr_map_notify (MBWMCompMgr *mgr, MBWindowManagerClient *c)
{
  ClutterActor             * actor;
  HdCompMgrPrivate         * priv = HD_COMP_MGR (mgr)->priv;
  MBWMCompMgrClutterClient * cclient;
  MBWMCompMgrClass         * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));
  HdCompMgrClient          * hclient;
  HdCompMgrClient          * hclient_h;
  guint                      hkey;
  MBWMClientType             ctype;
  static gboolean            first_time = TRUE;
  MBWindowManagerClient    * transient_for;

  g_debug ("%s: 0x%lx", __FUNCTION__, c && c->window ? c->window->xwindow : 0);

  if (G_UNLIKELY (first_time == TRUE))
    {
      int fd = creat (STAMP_FILE, 0644);

      if (fd >= 0)
        {
          close (fd);
        }
      else
        {
          g_debug ("failed to create stamp file " STAMP_FILE);
        }

      first_time = FALSE;
    }

  /* Log the time this window was mapped */
  gettimeofday(&priv->last_map_time, NULL);

  /* if *anything* is mapped, remove our full-screen input blocker */
  hd_render_manager_remove_input_blocker();

  /* We want to make sure the rotation transition is notified of a map event.
   * It may have happened during blanking, and if so we want to increase the
   * blanking time. */
  hd_transition_rotate_ignore_damage();

  /*g_debug ("%s, c=%p ctype=%d", __FUNCTION__, c,
             MB_WM_CLIENT_CLIENT_TYPE (c));*/
  if (MB_WM_CLIENT_CLIENT_TYPE (c) == MBWMClientTypeDesktop)
    return;

  /* discard notification previews if necessary */
  if (HD_IS_INCOMING_EVENT_PREVIEW_NOTE(c))
    {
      if (priv->do_not_disturb_flag
          || STATE_DISCARD_PREVIEW_NOTE (hd_render_manager_get_state())
          || hd_dbus_tklock_on)
        {
          g_debug ("%s. Discard notification", __FUNCTION__);
          mb_wm_client_hide (c);
          mb_wm_client_deliver_delete (c);
          return;
        }
    }

  transient_for = mb_wm_client_get_transient_for (c);

  /* Discard notification banners if do not disturb flag is set
   * and the dnd override flag is not set on the information banner
   * and the client is not topmost
   */
  if (priv->do_not_disturb_flag && HD_IS_BANNER_NOTE (c) &&
      (!transient_for ||
       mb_wm_client_get_next_focused_app (transient_for) != NULL))
    {
      guint32 *value;
      Atom dnd_override = hd_comp_mgr_get_atom (HD_COMP_MGR (mgr),
						HD_ATOM_HILDON_DO_NOT_DISTURB_OVERRIDE);

      value = hd_util_get_win_prop_data_and_validate (c->wmref->xdpy,
                c->window->xwindow, dnd_override, XA_INTEGER, 32, 1, NULL);

      if (!value || *value != 1)
        {
          g_debug ("%s. Discard information banner (Do not Disturb flag set)",
                   __FUNCTION__);
          mb_wm_client_hide (c);
          mb_wm_client_deliver_delete (c);

         if (value)
            XFree (value);
          return;
        }

      if (value)
        XFree (value);
    }

  ctype = MB_WM_CLIENT_CLIENT_TYPE (c);

  /*
   * #MBWMCompMgrClutterClient already has an actor, now it's time
   * for #MBWMCompMgrClutter to create its texture and bind it to
   * the window's pixmap.  This is not necessary for notifications
   * whose windows we don't use for anything at all and not creating
   * the texture saves precious miliseconds.
   */
  if (!HD_IS_INCOMING_EVENT_NOTE(c))
    {
      if (hd_comp_mgr_client_prefers_compositing (c))
        {
          /* TODO: should check that this client really is above the
           * non-composited client */
          /* possibly switch away from non-composited mode to enable creating
           * the texture */
          if (hd_render_manager_get_state () == HDRM_STATE_NON_COMPOSITED)
            hd_render_manager_set_state (HDRM_STATE_APP);
          else if (hd_render_manager_get_state () == HDRM_STATE_NON_COMP_PORT)
            hd_render_manager_set_state (HDRM_STATE_APP_PORTRAIT);
        }

      parent_klass->map_notify (mgr, c);
    }

  /* Now the actor has been created and added to the desktop, make sure we
   * call hdrm_restack to put it in the correct group in hd-render-manager */
  hd_render_manager_restack();
  hd_comp_mgr_portrait_or_not_portrait(mgr, c);

  /* Hide the Edit button if it is currently shown */
  if (priv->home)
    hd_home_hide_edit_button (HD_HOME (priv->home));

  /*
   * If the actor is an application, add it also to the switcher
   * If it is Home applet, add it to the home
   */

  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);
  hclient = HD_COMP_MGR_CLIENT (cclient);
  actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);

  if (hclient->priv->app)
    g_object_set_data (G_OBJECT (actor),
           "HD-ApplicationId",
           (gchar *)hd_running_app_get_id (hclient->priv->app));

  hd_comp_mgr_hook_update_area(HD_COMP_MGR (mgr), actor);

  /* if we are in home_edit mode and we have stuff that will get in
   * our way and spoil our grab, move to home_edit_dlg so we can
   * look the same but remove our grab. */
  if ((hd_render_manager_get_state()==HDRM_STATE_HOME_EDIT) &&
      (ctype & (MBWMClientTypeDialog |
                HdWmClientTypeAppMenu |
                HdWmClientTypeStatusMenu)))
    {
      hd_render_manager_set_state(HDRM_STATE_HOME_EDIT_DLG);
    }

  /*
   * Leave switcher/launcher for home if we're mapping a system-modal
   * dialog, information note or confirmation note.  We need to leave now,
   * before we disable hdrm reactivity, because changing state after that
   * restores that.
   */
  if (!transient_for)
    if (ctype == MBWMClientTypeDialog
        || HD_IS_INFO_NOTE (c) || HD_IS_CONFIRMATION_NOTE (c))
      if (STATE_ONE_OF(hd_render_manager_get_state(),
                       HDRM_STATE_LAUNCHER | HDRM_STATE_TASK_NAV))
        {
          hd_render_manager_set_state(HDRM_STATE_HOME);
          if (hd_comp_mgr_client_is_maximized(c->window->geometry))
            /*
             * So we are in switcher view and want to get to home view
             * to show a dialog or something.  hdrm_set_visibilities()
             * is smart enough to leave alone dialogs (normally they
             * are in app_top) but the case with maximized clients is
             * different and they are subject to the regular checking.
             * This checking would fail because the transition from
             * switcher to home view is, well, a transition and it
             * takes time, during which set_visibilities() refuses
             * to show windows, even dialogs, if they are in home_blur.
             * Soooo, let's stop the transition and problem solved.
             */
            hd_render_manager_stop_transition();
        }

  /* Hide status menu if any window except an applet is mapped */
  if (priv->status_menu_client &&
      ctype != HdWmClientTypeHomeApplet &&
      ctype != MBWMClientTypeOverride)
    mb_wm_client_deliver_delete (priv->status_menu_client);

  if (ctype == HdWmClientTypeHomeApplet)
    {
      HdHomeApplet * applet  = HD_HOME_APPLET (c);
      char         * applet_id = applet->applet_id;

      if (priv->home && strcmp (OPERATOR_APPLET_ID, applet_id) != 0)
        {
          /* Normal applet */
          g_object_set_data_full (G_OBJECT (actor), "HD-applet-id",
                                  g_strdup (applet_id),
                                  (GDestroyNotify) g_free);
          hd_home_add_applet (HD_HOME (priv->home), actor);
        }
      else if (priv->home)
        {
          /* Special operator applet */
          hd_home_set_operator_applet (HD_HOME (priv->home), actor);
        }
      return;
    }
  else if (ctype == HdWmClientTypeStatusArea)
    {
      hd_home_add_status_area (HD_HOME (priv->home), actor);
      priv->status_area_client = c;
      return;
    }
  else if (ctype == HdWmClientTypeStatusMenu)
    { /* Either status menu OR power menu. */
      if (STATE_ONE_OF(hd_render_manager_get_state(),
                       HDRM_STATE_LAUNCHER | HDRM_STATE_TASK_NAV))
        hd_render_manager_set_state(HDRM_STATE_HOME);
      hd_home_add_status_menu (HD_HOME (priv->home), actor);
      priv->status_menu_client = c;
      return;
    }
  else if (ctype == HdWmClientTypeAnimationActor)
    {
      return;
    }
  else if (ctype == HdWmClientTypeAppMenu)
    {
      /* This is mainly for the power key menu, but we must not allow
       * menus is general when not in APP state because they are not
       * added to the switcher.  This can be considered a shortcoming. */
      if (STATE_NEED_WHOLE_SCREEN_INPUT(hd_render_manager_get_state()))
        hd_render_manager_set_state(HDRM_STATE_HOME);
      return;
    }
  else if (ctype == MBWMClientTypeNote)
    {
      if (HD_IS_BANNER_NOTE (c) || HD_IS_INCOMING_EVENT_PREVIEW_NOTE (c))
        hd_render_manager_add_to_front_group(actor);

      if (HD_IS_INCOMING_EVENT_PREVIEW_NOTE (c))
        { /* let's be us who decide when the previews can be clicked */
          clutter_actor_set_reactive (actor, TRUE);
          g_signal_connect_swapped (actor, "button-press-event",
                                    G_CALLBACK (hd_note_clicked), c);
        }

      if (HD_IS_INCOMING_EVENT_NOTE (c))
        hd_switcher_add_notification (priv->switcher_group, HD_NOTE (c));

      if (!HD_IS_INCOMING_EVENT_NOTE (c)
          && !HD_IS_INCOMING_EVENT_PREVIEW_NOTE (c)
          && transient_for)
        hd_switcher_add_dialog (priv->switcher_group, c, actor);
#if 0
      if (HD_IS_BANNER_NOTE (c) && priv->mce_proxy)
        { /* Turn display backlight on for banner notes. */
          g_debug ("%s. Call %s", __FUNCTION__, MCE_DISPLAY_ON_REQ);
          dbus_g_proxy_call_no_reply (priv->mce_proxy, MCE_DISPLAY_ON_REQ,
                                      G_TYPE_INVALID, G_TYPE_INVALID);
        }
#endif
      return;
    }
  else if (ctype == MBWMClientTypeDialog)
    {
      if (transient_for)
        {
          hd_switcher_add_dialog (priv->switcher_group, c, actor);
        }
      return;
    }
  else if (c->window->net_type ==
	   c->wmref->atoms[MBWM_ATOM_NET_WM_WINDOW_TYPE_POPUP_MENU])
    {
      MBWindowManagerClient *transfor;

      if ((transfor = hd_comp_mgr_get_client_transient_for (c)) != NULL)
        {
          hd_switcher_add_dialog_explicit (HD_SWITCHER (priv->switcher_group),
                                           c, actor, transfor);
        }
      return;
    }
  else if (ctype == MBWMClientTypeOverride &&
           (hd_render_manager_get_state () == HDRM_STATE_NON_COMPOSITED ||
            hd_render_manager_get_state () == HDRM_STATE_NON_COMP_PORT))
    {
      /* we need to unredirect this to screen */
      hd_comp_mgr_unredirect_client (c);
      return;
    }
  else if (ctype != MBWMClientTypeApp)
    return;

  hkey = hclient->priv->hibernation_key;

  hclient_h = g_hash_table_lookup (priv->hibernating_apps, (gpointer)hkey);

  if (hclient_h)
    {
      MBWMCompMgrClutterClient *cclient_h;
      ClutterActor *actor_h;
      cclient_h = MB_WM_COMP_MGR_CLUTTER_CLIENT (hclient_h);
      actor_h = mb_wm_comp_mgr_clutter_client_get_actor (cclient_h);
      hd_switcher_replace_window_actor (priv->switcher_group,
                                        actor_h, actor);
      mb_wm_object_unref (MB_WM_OBJECT (hclient_h));
      g_hash_table_remove (priv->hibernating_apps, (gpointer)hkey);
    }

  if (hd_comp_mgr_is_non_composited (c, FALSE))
    {
      MBWindowManagerClient *tmp;
      gboolean found = FALSE;
      /* first check that this client is not below some client that needs
       * compositing */
      for (tmp = c->stacked_above; tmp; tmp = tmp->stacked_above)
        if (mb_wm_client_is_map_confirmed (tmp) &&
            hd_comp_mgr_client_prefers_compositing (tmp))
          {
            found = TRUE;
            break;
          }

      if (!found &&
          hd_render_manager_get_state () != HDRM_STATE_NON_COMPOSITED &&
          !STATE_IS_PORTRAIT (hd_render_manager_get_state ()))
        hd_render_manager_set_state (HDRM_STATE_NON_COMPOSITED);
      else if (!found &&
               hd_render_manager_get_state () != HDRM_STATE_NON_COMP_PORT &&
               STATE_IS_PORTRAIT (hd_render_manager_get_state ()))
        hd_render_manager_set_state (HDRM_STATE_NON_COMP_PORT);
      else if (!found &&
               (hd_render_manager_get_state () == HDRM_STATE_NON_COMPOSITED ||
                hd_render_manager_get_state () == HDRM_STATE_NON_COMP_PORT))
        hd_comp_mgr_unredirect_topmost_client (c->wmref, FALSE);
    }
  else if (hd_render_manager_get_state () == HDRM_STATE_NON_COMPOSITED)
    {
      hd_render_manager_set_state (HDRM_STATE_APP);
    }
  else if (hd_render_manager_get_state () == HDRM_STATE_NON_COMP_PORT)
    {
      hd_render_manager_set_state (HDRM_STATE_APP_PORTRAIT);
    }

  int topmost;
  HdApp *app = HD_APP (c), *to_replace, *add_to_tn;

  hd_comp_mgr_handle_stackable (c, &to_replace, &add_to_tn);

  if (app->stack_index < 0 /* non-stackable */
      /* leader without followers: */
      || (!app->leader->followers && app->leader == app) ||
      /* or a secondary window on top of the stack: */
      app == g_list_last (app->leader->followers)->data)
    topmost = 1;
  else
    topmost = 0;

  /* handle the restart case when the stack does not have any window
   * in the switcher yet */
  if (app->stack_index >= 0 && !topmost)
    {
      HdTaskNavigator *tasknav;
      MBWMCompMgrClutterClient *cclient;
      gboolean in_tasknav;

      tasknav = HD_TASK_NAVIGATOR (priv->task_nav);

      cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);
      if (hd_task_navigator_has_window (tasknav,
          mb_wm_comp_mgr_clutter_client_get_actor (cclient)))
        in_tasknav = TRUE;
      else
        in_tasknav = FALSE;

      if (!app->leader->followers && !in_tasknav)
        /* lonely leader */
        topmost = TRUE;
      else if (app->leader->followers && !in_tasknav)
        {
          GList *l;
          gboolean child_found = FALSE;

          for (l = app->leader->followers; l; l = l->next)
            {
              cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (
                             MB_WM_CLIENT (l->data)->cm_client);
              if (hd_task_navigator_has_window (tasknav,
                  mb_wm_comp_mgr_clutter_client_get_actor (cclient)))
                {
                  child_found = TRUE;
                  break;
                }
            }

          if (!child_found)
            topmost = TRUE;
        }
    }

  if (to_replace &&
		  to_replace->leader == NULL &&
		  to_replace->stack_index == -1)
    {
      ClutterActor *old_actor;

      /*
       * If we replaced an existing follower and made it a non stackable.
       */
      old_actor = mb_wm_comp_mgr_clutter_client_get_actor (
                  MB_WM_COMP_MGR_CLUTTER_CLIENT (
			  MB_WM_CLIENT (to_replace)->cm_client));

      g_debug ("%s: REPLACE %p WITH %p and ADD %p BACK", __func__,
		      old_actor, actor, old_actor);
      hd_switcher_replace_window_actor (priv->switcher_group, old_actor, actor);
      hd_switcher_add_window_actor (priv->switcher_group, old_actor);

      /* and make sure we're in app mode and not transitioning as
       * we'll want to show this new app right away*/
      if (!STATE_IS_APP(hd_render_manager_get_state()))
        hd_render_manager_set_state(HDRM_STATE_APP);
      hd_render_manager_stop_transition();
      /* This forces the decors to be redone, taking into account the
       * stack index. */
      mb_wm_client_theme_change (c);
      mb_wm_client_theme_change (MB_WM_CLIENT(to_replace));
  } else if (to_replace && topmost)
    {
      ClutterActor *old_actor;
      old_actor = mb_wm_comp_mgr_clutter_client_get_actor (
                  MB_WM_COMP_MGR_CLUTTER_CLIENT (
                    MB_WM_CLIENT (to_replace)->cm_client));
      if (old_actor != actor)
      {
        g_debug ("%s: REPLACE ACTOR %p WITH %p", __func__, old_actor,
               actor);
        hd_switcher_replace_window_actor (priv->switcher_group,
                                        old_actor, actor);
        /* and make sure we're in app mode and not transitioning as
         * we'll want to show this new app right away*/
        if (!STATE_IS_APP(hd_render_manager_get_state()))
          hd_render_manager_set_state(HDRM_STATE_APP);
        hd_render_manager_stop_transition();
        /* This forces the decors to be redone, taking into account the
         * stack index. */
        mb_wm_client_theme_change (c);
      }
    }
  else if (to_replace && to_replace->leader == app)
    {
      ClutterActor *old_actor;

      /*
       * This is the 'old leader become a follower' use case. In this situation
       * the visible actor remains a follower, but we need to do something with
       * the new actor otherwise it will be unhandled by the task switcher and
       * it will be shown in the background.
       */
      g_debug ("%s: ADD ACTOR %p BEHIND THE LEADER", __func__, actor);
      old_actor = mb_wm_comp_mgr_clutter_client_get_actor (
		      MB_WM_COMP_MGR_CLUTTER_CLIENT (
			      MB_WM_CLIENT (to_replace)->cm_client));
      hd_switcher_replace_window_actor (priv->switcher_group, old_actor, actor);
      hd_switcher_replace_window_actor (priv->switcher_group, actor, old_actor);
      /* and make sure we're in app mode and not transitioning as
       * we'll want to show this new app right away*/
      if (!STATE_IS_APP(hd_render_manager_get_state()))
        hd_render_manager_set_state(HDRM_STATE_APP);
      hd_render_manager_stop_transition();
      /* This forces the decors to be redone, taking into account the
       * stack index. */
      mb_wm_client_theme_change (c);
      mb_wm_client_theme_change (MB_WM_CLIENT(to_replace));
    }
  else if (add_to_tn)
    {
      g_debug ("%s: ADD ACTOR %p", __func__, actor);
      hd_switcher_add_window_actor (priv->switcher_group, actor);
      /* and make sure we're in app mode and not transitioning as
       * we'll want to show this new app right away*/
      if (!STATE_IS_APP(hd_render_manager_get_state()))
        hd_render_manager_set_state(HDRM_STATE_APP);
      hd_render_manager_stop_transition();
      /* This forces the decors to be redone, taking into account the
       * stack index. */
      mb_wm_client_theme_change (c);
    }
  else if (app->leader && app->leader != app &&
		    app->leader->followers && !topmost)
    {
      ClutterActor *old_actor;
      HdApp *last_follower;

      /*
       * If this window belongs to a window stack, but it is not visible we
       * still have to do something with it, otherwise it will appear in the
       * background.
       */
      last_follower = g_list_last (app->leader->followers)->data;
      old_actor = mb_wm_comp_mgr_clutter_client_get_actor (
		      MB_WM_COMP_MGR_CLUTTER_CLIENT (
			      MB_WM_CLIENT (last_follower)->cm_client));
      g_debug ("%s: REPLACE %p WITH %p and ADD %p BACK", __func__,
		      old_actor, actor, old_actor);
      hd_switcher_replace_window_actor (priv->switcher_group, old_actor, actor);
      hd_switcher_replace_window_actor (priv->switcher_group, actor, old_actor);
    }


  if (!(c->window->ewmh_state & MBWMClientWindowEWMHStateSkipTaskbar)
      && !to_replace && !add_to_tn && topmost)
    {
            /*
      printf("non-stackable, stackable leader, "
             "or secondary acting as leader\n");
             */
      g_debug ("%s: ADD CLUTTER ACTOR %p", __func__, actor);
      hd_switcher_add_window_actor (priv->switcher_group, actor);
      /* and make sure we're in app mode and not transitioning as
       * we'll want to show this new app right away*/
      if (!STATE_IS_APP(hd_render_manager_get_state()))
        hd_render_manager_set_state(HDRM_STATE_APP);
      hd_render_manager_stop_transition();

      /* This forces the decors to be redone, taking into account the
       * stack index (if any). */
      mb_wm_client_theme_change (c);
    }
}

static MBWindowManagerClient *
hd_comp_mgr_determine_current_app ()
{
  extern MBWindowManager *hd_mb_wm;
  MBWindowManagerClient *c;

  /* Select the topmost client that is either the desktop
   * or a %HdApp with full screen coverage. */
  for (c = hd_mb_wm->stack_top; c; c = c->stacked_below)
    {
      if (MB_WM_CLIENT_CLIENT_TYPE (c) & MBWMClientTypeDesktop)
        return c;
      if (!HD_IS_APP (c))
        continue;
      if (mb_wm_client_is_unmap_confirmed (c))
        continue; 
      if (!mb_wm_client_is_map_confirmed (c) &&
          !hd_comp_mgr_client_is_maximized (c->frame_geometry))
        /* Not covering the whole application area. */
        continue;
      if (!c->window)
        continue;
      if (c->window->name && !g_strncasecmp (c->window->name, "systemui", 8))
        /* systemui is not an application. */
        continue;
      return c;
    }
  return hd_mb_wm->desktop;
}

void
hd_comp_mgr_reconsider_compositing (MBWMCompMgr *mgr)
{
  HDRMStateEnum hdrm_state = hd_render_manager_get_state ();
  MBWindowManagerClient *c = hd_comp_mgr_determine_current_app ();

  if (c && c != mgr->wm->desktop && !hd_transition_is_rotating () &&
      (hdrm_state == HDRM_STATE_APP || hdrm_state == HDRM_STATE_APP_PORTRAIT)
      && hd_comp_mgr_is_non_composited (c, FALSE))
    {
      MBWindowManagerClient *tmp;
      gboolean found = FALSE;

      /* check if there is a window that wishes composited mode above */
      for (tmp = c->stacked_above; tmp; tmp = tmp->stacked_above)
        if (mb_wm_client_is_map_confirmed (tmp) &&
            hd_comp_mgr_client_prefers_compositing (tmp))
          {
            found = TRUE;
            break;
          }

      if (!found)
        {
          if (hdrm_state == HDRM_STATE_APP)
            hd_render_manager_set_state (HDRM_STATE_NON_COMPOSITED);
          else
            hd_render_manager_set_state (HDRM_STATE_NON_COMP_PORT);
        }
    }
  else if (hdrm_state == HDRM_STATE_NON_COMPOSITED ||
           hdrm_state == HDRM_STATE_NON_COMP_PORT)
    {
      if (c && c == mgr->wm->desktop)
        hd_render_manager_set_state (HDRM_STATE_HOME);
      else if (c)
        {
          MBWindowManagerClient *tmp;
          gboolean found = FALSE;

          /* check if there is a window that needs composited mode above */
          for (tmp = c->stacked_above; tmp; tmp = tmp->stacked_above)
            if (mb_wm_client_is_map_confirmed (tmp) &&
                hd_comp_mgr_client_prefers_compositing (tmp))
              {
                found = TRUE;
                break;
              }

          if (found || !hd_comp_mgr_is_non_composited (c, FALSE))
            {
              if (hdrm_state == HDRM_STATE_NON_COMPOSITED)
                hd_render_manager_set_state (HDRM_STATE_APP);
              else
                hd_render_manager_set_state (HDRM_STATE_APP_PORTRAIT);
            }
          /* this is for the case of two clients on top of each other,
           * where the top client is unredirected and unmapped but the
           * lower client is not unredirected yet */
          else if (hd_comp_mgr_is_non_composited (c, FALSE) && c->cm_client &&
                 !mb_wm_comp_mgr_clutter_client_is_unredirected (c->cm_client))
            hd_comp_mgr_unredirect_client (c);
        }
      else /* no application -> we should be composited */
        {
          g_warning ("non-composited but no application, should not happen");
          if (hdrm_state == HDRM_STATE_NON_COMPOSITED)
            hd_render_manager_set_state (HDRM_STATE_APP);
          else
            hd_render_manager_set_state (HDRM_STATE_APP_PORTRAIT);
        }
    }
}

static void
hd_comp_mgr_unmap_notify (MBWMCompMgr *mgr, MBWindowManagerClient *c)
{
  HdCompMgrPrivate          * priv = HD_COMP_MGR (mgr)->priv;
  MBWMClientType            c_type = MB_WM_CLIENT_CLIENT_TYPE (c);
  MBWMCompMgrClutterClient *cclient;
  HDRMStateEnum             hdrm_state;

  g_debug ("%s: 0x%lx", __FUNCTION__, c && c->window ? c->window->xwindow : 0);
  cclient = MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client);
  hdrm_state = hd_render_manager_get_state ();

  /* if we are in home_edit_dlg mode, check and see if there is stuff
   * that would spoil our grab now - and if not, return to home_edit mode */
  if (hdrm_state == HDRM_STATE_HOME_EDIT_DLG)
    {
      gboolean grab_spoil = FALSE;
      MBWindowManagerClient *above = mgr->wm->desktop;
      if (above) above = above->stacked_above;
      while (above)
        {
          if (above != c &&
              MB_WM_CLIENT_CLIENT_TYPE(above)!=HdWmClientTypeHomeApplet &&
              MB_WM_CLIENT_CLIENT_TYPE(above)!=HdWmClientTypeStatusArea)
            grab_spoil = TRUE;
          above = above->stacked_above;
        }
      if (!grab_spoil)
        hd_render_manager_set_state(HDRM_STATE_HOME_EDIT);
    }

  if (HD_IS_APP (c) && HD_APP (c)->stack_index > 0 &&
      HD_APP (c)->leader != HD_APP (c))
    {
      GList *l;
      g_debug ("%s: detransitise stackable secondary %lx\n", __FUNCTION__,
               c->window->xwindow);
      /* stackable window: detransitise if it is not the leader, so we
       * don't unmap the secondaries above us */
      mb_wm_client_detransitise (MB_WM_CLIENT (c));

      l = g_list_find (HD_APP (c)->leader->followers, c);
      if (l && l->next)
      {
        /* remove link from the window above, so that it is not unmapped
         * by libmatchbox2 */
        mb_wm_client_detransitise (MB_WM_CLIENT (l->next->data));
        /* add link from that window to the window below */
        if (l->prev)
          {
            g_debug("%s: re-link stackable %lx to secondary\n", __FUNCTION__,
                   MB_WM_CLIENT (l->next->data)->window->xwindow);
            mb_wm_client_add_transient (MB_WM_CLIENT (l->prev->data),
                                        MB_WM_CLIENT (l->next->data));
          }
        else
          {
            g_debug("%s: re-link stackable %lx to leader\n", __FUNCTION__,
                   MB_WM_CLIENT (l->next->data)->window->xwindow);
            mb_wm_client_add_transient (MB_WM_CLIENT (HD_APP (c)->leader),
                                        MB_WM_CLIENT (l->next->data));
          }
      }
    }
  else if (HD_IS_APP (c) && HD_APP (c)->stack_index >= 0 &&
           HD_APP (c)->leader == HD_APP (c))
    {
      g_debug ("%s: detransitise stackable leader %lx\n", __FUNCTION__,
               c->window->xwindow);
      if (HD_APP (c)->followers)
        {
          /* remove link from the first secondary, so that it is not unmapped
           * by libmatchbox2 */
          mb_wm_client_detransitise (
                MB_WM_CLIENT (HD_APP (c)->followers->data));
        }
    }
  else if (HD_IS_INCOMING_EVENT_NOTE(c))
    {
      hd_switcher_remove_notification (priv->switcher_group,
                                       HD_NOTE (c));
      return;
    }
  else if (c_type == MBWMClientTypeNote || c_type == MBWMClientTypeDialog ||
           c->window->net_type ==
           c->wmref->atoms[MBWM_ATOM_NET_WM_WINDOW_TYPE_POPUP_MENU])
    {
      ClutterActor *actor;
      actor = mb_wm_comp_mgr_clutter_client_get_actor (cclient);
      /* checking for transiency is not enough because nowadays dialogs
       * can become non-transient after they have been transient */
      if (actor)
        hd_switcher_remove_dialog (priv->switcher_group, actor);
    }
}


static void
hd_comp_mgr_effect (MBWMCompMgr                *mgr,
                    MBWindowManagerClient      *c,
                    MBWMCompMgrClientEvent      event)
{
  HdCompMgr      *hmgr = HD_COMP_MGR(mgr);
  MBWMClientType c_type = MB_WM_CLIENT_CLIENT_TYPE (c);
  /*g_debug ("%s, c=%p ctype=%d event=%d",
            __FUNCTION__, c, MB_WM_CLIENT_CLIENT_TYPE (c), event);*/

  if (c->window->allowed_actions & MBWMClientWindowActionNoTransitions)
    {
      hd_comp_mgr_reconsider_compositing (mgr);
      return;
    }

  /*HdCompMgrPrivate *priv = HD_COMP_MGR (mgr)->priv;*/
  if (event == MBWMCompMgrClientEventUnmap)
    {
      if (c_type == HdWmClientTypeStatusMenu)
        hd_transition_popup(hmgr, c, MBWMCompMgrClientEventUnmap);
      else if (HD_IS_INCOMING_EVENT_PREVIEW_NOTE(c))
        hd_transition_notification(hmgr, c, MBWMCompMgrClientEventUnmap);
      else if (c_type == MBWMClientTypeDialog ||
               c_type == HdWmClientTypeAppMenu)
        {
          if (!hd_util_client_obscured(c))
            hd_transition_popup(hmgr, c, MBWMCompMgrClientEventUnmap);
        }
      else if (c_type == MBWMClientTypeNote && !HD_IS_INCOMING_EVENT_NOTE(c))
        hd_transition_fade(hmgr, c, MBWMCompMgrClientEventUnmap);
      else if (c_type == MBWMClientTypeApp)
        {
          /* Look if it's a stackable window. */
          HdApp *app = HD_APP (c);
          if (app->stack_index > 0 && app->leader != app)
            {
              /* Find the next window to transition to. We haven't yet been
               * removed from the stack so want the window second from the
               * top */
              MBWindowManagerClient *next = MB_WM_CLIENT(app->leader);
              if (app->leader->followers &&
                  g_list_last(app->leader->followers)->prev)
                next = MB_WM_CLIENT(
                    g_list_last(app->leader->followers)->prev->data);
              /* Start actual transition */
              hd_transition_subview(hmgr, c,
                                    next,
                                    MBWMCompMgrClientEventUnmap);
            }
          else if ((app->stack_index < 0
                    || (app->leader == app && !app->followers))
                   && hd_task_navigator_is_crowded (hmgr->priv->task_nav)
                   && c->window->xwindow == hd_wm_current_app_is (NULL, 0)
                   && hd_render_manager_get_state () != HDRM_STATE_APP_PORTRAIT
                   && !hd_wm_has_modal_blockers (mgr->wm)
                   && !c->transient_for)
	  {
            hd_render_manager_set_state (HDRM_STATE_TASK_NAV);
	  }
          else
            {
              HdCompMgrClient *hclient = HD_COMP_MGR_CLIENT (c->cm_client);
              HdRunningApp *app = 
		HD_RUNNING_APP (hd_comp_mgr_client_get_app (hclient));

              /* Avoid this transition if app is being hibernated */
              if (!app ||
                  !hd_running_app_is_hibernating (app))
                {
                  /* unregister_client() will switch state if it thinks so */
                  gboolean window_on_top = FALSE;
                  MBWindowManagerClient *cit = c->stacked_above;
                  while (cit)
                    {
                      MBWMClientType cit_type = MB_WM_CLIENT_CLIENT_TYPE(cit);
                      if (cit_type == MBWMClientTypeApp ||
                          cit_type == MBWMClientTypeDialog ||
                          cit_type == MBWMClientTypeDesktop)
                        {
                          window_on_top = TRUE;
                          break;
                        }
                      cit = cit->stacked_above;
                    }

                  /* if the window has another app/dialog on top of it then
                   * don't show the closing animation - but still play the
                   * sound */
                  if (!window_on_top)
                    hd_transition_close_app (hmgr, c);
                  else
                    {
                      hd_comp_mgr_reconsider_compositing (mgr);
                      hd_transition_play_sound (HDCM_WINDOW_CLOSED_SOUND);
                    }
                }
              else
                hd_comp_mgr_reconsider_compositing (mgr);
            }
          app->map_effect_before = FALSE;
        }
      else
        hd_comp_mgr_reconsider_compositing (mgr);
    }
  else if (event == MBWMCompMgrClientEventMap)
    {
      if (c_type == HdWmClientTypeStatusMenu)
        hd_transition_popup(hmgr, c, MBWMCompMgrClientEventMap);
      else if ((c_type == MBWMClientTypeDialog) ||
               (c_type == HdWmClientTypeAppMenu))
        hd_transition_popup(hmgr, c, MBWMCompMgrClientEventMap);
      else if (HD_IS_INCOMING_EVENT_PREVIEW_NOTE(c))
        hd_transition_notification(hmgr, c, MBWMCompMgrClientEventMap);
      else if (c_type == MBWMClientTypeNote && !HD_IS_INCOMING_EVENT_NOTE(c))
        /* std event notes go direct to the switcher, so we don't want to
         * use a transition for them */
        hd_transition_fade(hmgr, c, MBWMCompMgrClientEventMap);
      else if (c_type == MBWMClientTypeApp)
        {
          /* Look if it's a stackable window. We don't do the subview
           * animation again if we have had a mapping without an unmap,
           * which is what happens in the Image Viewer when the same
           * window goes from Fullscreen to Windowed */
          HdApp *app = HD_APP (c);
          if (app->stack_index > 0 && !app->map_effect_before)
            {
              /* Find the window to transition from (we have already been
               * added to the stack, so the window is second from the
               * top of the stack */
              MBWindowManagerClient *next = MB_WM_CLIENT(app->leader);
              if (app->leader->followers &&
                  g_list_last(app->leader->followers)->prev)
                next = MB_WM_CLIENT(
                    g_list_last(app->leader->followers)->prev->data);
              /* Start actual transition - We may have the case where 2
               * stackable windows are created at the same time, and we are
               * mapping the subview before the main view is mapped. In this
               * case the first window will not be in the followers list, and
               * app->leader == app, so we don't want any transition.
               * Solves NB#112411 */
              if (c!=next)
                hd_transition_subview(hmgr, c,
                                      next,
                                      MBWMCompMgrClientEventMap);
            }
          /* We're now showing this app, so remove our app
           * starting screen if we had one */
          hd_launcher_window_created();
          app->map_effect_before = TRUE;
        }
    }
}

gboolean
hd_comp_mgr_restack (MBWMCompMgr * mgr)
{
  HdCompMgrPrivate         * priv = HD_COMP_MGR (mgr)->priv;
  MBWMCompMgrClass         * parent_klass =
    MB_WM_COMP_MGR_CLASS (MB_WM_OBJECT_GET_PARENT_CLASS(MB_WM_OBJECT(mgr)));

  /* g_debug ("%s", __FUNCTION__); */

  /*
   * We use the parent class restack() method to do the stacking, but as our
   * switcher shares actors with the CM, we cannot run this when the switcher
   * is showing, or an unmap effect is in progress; instead we set a flag, and
   * let the switcher request stack sync when it closes.
   */
  if (priv->stack_sync)
    {
      g_source_remove (priv->stack_sync);
      priv->stack_sync = 0;
    }

  if (STATE_NEED_TASK_NAV (hd_render_manager_get_state()))
    {
      hd_comp_mgr_check_do_not_disturb_flag (HD_COMP_MGR (mgr));
      return FALSE;
    }

  /* Hide the Edit button if it is currently shown */
  if (priv->home)
    hd_home_hide_edit_button (HD_HOME (priv->home));

  if (parent_klass->restack)
    parent_klass->restack (mgr);

  /* Update _MB_CURRENT_APP_WINDOW if we're ready and it's changed.
   * Don't if we're in the middle of a transition which will change
   * state one again because getting is-topmost wrong is frowned upon. */
  if (mgr->wm && mgr->wm->root_win && mgr->wm->desktop
      && !hd_transition_rotation_will_change_state ())
    {
      MBWindowManagerClient *current_client =
                              hd_comp_mgr_determine_current_app ();

      HdCompMgrClient *new_current_hclient =
        HD_COMP_MGR_CLIENT (current_client->cm_client);
      if (new_current_hclient != priv->current_hclient)
        {
          HdRunningApp *old_current_app;
          HdRunningApp *new_current_app;

          /* Switch the hibernatable state for the new current client. */
          if (priv->current_hclient &&
              hd_comp_mgr_client_can_hibernate (priv->current_hclient))
            {
              old_current_app =
                HD_RUNNING_APP (hd_comp_mgr_client_get_app (priv->current_hclient));
              if (old_current_app)
                hd_app_mgr_hibernatable (old_current_app, TRUE);
            }

          if (new_current_hclient)
            {
              new_current_app =
                HD_RUNNING_APP (hd_comp_mgr_client_get_app (new_current_hclient));
              if (new_current_app)
                hd_app_mgr_hibernatable (new_current_app, FALSE);
            }

          priv->current_hclient = new_current_hclient;
        }

      hd_wm_current_app_is (mgr->wm, current_client->window->xwindow);

      /* If we have an app as the current client and we're not in
       * app mode - enter app mode. */
      if (!(MB_WM_CLIENT_CLIENT_TYPE(current_client) &
                                     MBWMClientTypeDesktop) &&
          !STATE_IS_APP(hd_render_manager_get_state()))
        hd_render_manager_set_state(HDRM_STATE_APP);
    }

  /* Decide about portraitification in case a blocking window was unmapped. */
  hd_comp_mgr_check_do_not_disturb_flag (HD_COMP_MGR (mgr));
  hd_render_manager_restack ();
  hd_comp_mgr_portrait_or_not_portrait (mgr, NULL);

  return FALSE;
}

/* Do a restack some time.  Used in cases when multiple parties
 * want restacking, not knowing about each other. */
void
hd_comp_mgr_sync_stacking (HdCompMgr * hmgr)
{
  HdCompMgrPrivate * priv = hmgr->priv;

  if (!priv->stack_sync)
    /* We need higher priority than idles usually have because
     * the effect has higher priority too and it could starve us. */
    priv->stack_sync = g_idle_add_full (0, (GSourceFunc)hd_comp_mgr_restack,
                                        hmgr, NULL);
}

/*
 * Shuts down a client, handling hibernated applications correctly.
 * if @close_all and @cc is associated with a window stack then
 * close all windows in the stack, otherwise only @cc's.
 */
void
hd_comp_mgr_close_app (HdCompMgr *hmgr, MBWMCompMgrClutterClient *cc,
                       gboolean close_all)
{
  HdCompMgrPrivate      * priv = hmgr->priv;
  HdCompMgrClient       * h_client = HD_COMP_MGR_CLIENT (cc);

  g_return_if_fail (cc != NULL);
  g_return_if_fail (h_client != NULL);
  if (hd_comp_mgr_client_is_hibernating(h_client))
    {
      ClutterActor * actor;

      actor = mb_wm_comp_mgr_clutter_client_get_actor (cc);

      hd_switcher_remove_window_actor (priv->switcher_group, actor, cc);

      g_hash_table_remove (priv->hibernating_apps,
                           (gpointer)h_client->priv->hibernation_key);

      if (h_client->priv->app)
        {
          hd_app_mgr_app_stop_hibernation (h_client->priv->app);
        }

      mb_wm_object_unref (MB_WM_OBJECT (cc));
    }
  else
    {
      MBWindowManagerClient * c = MB_WM_COMP_MGR_CLIENT (cc)->wm_client;

      if (close_all && HD_IS_APP (c) && HD_APP (c)->leader)
        {
          c = MB_WM_CLIENT (HD_APP (c)->leader);
          hd_app_close_followers (HD_APP (c));
	  if (HD_APP (c)->leader == HD_APP (c))
	    /* send delete to the leader */
            mb_wm_client_deliver_delete (c);
        }
      else /* Either primary or a secondary who's lost its leader. */
        mb_wm_client_deliver_delete (c);
    }
}

void
hd_comp_mgr_close_client (HdCompMgr *hmgr, MBWMCompMgrClutterClient *cc)
{
  hd_comp_mgr_close_app (hmgr, cc, FALSE);
}

void
hd_comp_mgr_wakeup_client (HdCompMgr *hmgr, HdCompMgrClient *hclient)
{
  hd_app_mgr_activate (hclient->priv->app);
}

void
hd_comp_mgr_kill_all_apps (HdCompMgr *hmgr)
{
  hd_app_mgr_kill_all ();
}

/* Update the inherited portrait flags of @cs if they were calculated
 * earlier than @now.  If @now is G_MAXUINT the flags are uncoditionally
 * updated but are not cached.  Returns @cs's %HdCompMgrClient. */
static HdCompMgrClient *
hd_comp_mgr_update_clients_portrait_flags (MBWindowManagerClient *cs,
                                           guint now)
{
  HdCompMgrClient *hcmgrcs, *hcmgrct;

  hcmgrcs = HD_COMP_MGR_CLIENT (cs->cm_client);
  if ((hcmgrcs->priv->portrait_supported_inherited
       || hcmgrcs->priv->portrait_requested_inherited)
      && hcmgrcs->priv->portrait_timestamp != now)
    { /* @cs has outdated flags */
      if (  !hcmgrcs->priv->portrait_requested_inherited
          && hcmgrcs->priv->portrait_requested
          && hcmgrcs->priv->portrait_supported_inherited)
        /* Add some crap to the pile: if you request but don't say
         * you support you do. */
        hcmgrcs->priv->portrait_supported = TRUE;
      else if (cs->transient_for)
        { /* Get the parent's and copy them. */
          hcmgrct = hd_comp_mgr_update_clients_portrait_flags (cs->transient_for,
                                                               now);
          if (hcmgrcs->priv->portrait_supported_inherited)
            hcmgrcs->priv->portrait_supported = hcmgrct->priv->portrait_supported;
          if (hcmgrcs->priv->portrait_requested_inherited)
            hcmgrcs->priv->portrait_requested = hcmgrct->priv->portrait_requested;
        }
      if (now != G_MAXUINT)
        hcmgrcs->priv->portrait_timestamp = now;
    }
  return hcmgrcs;
}

/* Does any visible client request portrait mode?
 * Are all of them concerned prepared for it? If ignore is nonzero,
 * we ignore the given client (as it may be disappearing soon). */
static gboolean
hd_comp_mgr_should_be_portrait_ignoring (HdCompMgr *hmgr,
                                         MBWindowManagerClient *ignore)
{
  static guint counter;
  gboolean any_requests;
  MBWindowManager *wm;
  HdCompMgrClient *hcmgrc;
  MBWindowManagerClient *c;

  /* Invalidate all cached, inherited portrait flags at once. */
  counter++;

  PORTRAIT ("SHOULD BE PORTRAIT?");
  any_requests = FALSE;
  wm = MB_WM_COMP_MGR (hmgr)->wm;
  for (c = wm->stack_top; c && c != wm->desktop; c = c->stacked_below)
    {
      if (c == ignore)
        continue;

      PORTRAIT ("CLIENT %p", c);
      PORTRAIT ("IS IGNORABLE?");
      if (MB_WM_CLIENT_CLIENT_TYPE (c) & HdWmClientTypeStatusArea)
        /* It'll be blocked anyway. */
        continue;
      if (MB_WM_CLIENT_CLIENT_TYPE (c)
          & (HdWmClientTypeAppMenu | MBWMClientTypeMenu))
        /* Menus are not transient for their window nor they claim
         * portrait layout support.  Let's just assume they can. */
        continue;
      if (MB_WM_CLIENT_CLIENT_TYPE (c) & HdWmClientTypeHomeApplet)
        /* Make an exception for applets. */
        continue;
      if (HD_IS_BANNER_NOTE (c) || HD_IS_INCOMING_EVENT_PREVIEW_NOTE (c))
        /* Assume it for now. */
        continue;

      PORTRAIT ("IS VISIBLE OR CURRENT?");
      if (!hd_render_manager_is_client_visible (c) && !(c->window
          && hd_wm_current_app_is (NULL, 0) == c->window->xwindow))
        /*
         * Ignore invisibles except if it's the current application.
         * This is for cases when the topmost client requests pmode
         * and you launch another program.  When we're invoked the
         * new client doesn't have an actor yet but it needs to be
         * taken into account.
         */
        continue;

      /* Get @portrait_supported/requested updated. */
      hcmgrc = hd_comp_mgr_update_clients_portrait_flags (c, counter);
      PORTRAIT ("SUPPORT IS %d", hcmgrc->priv->portrait_supported);
      if (!hcmgrc->priv->portrait_supported)
        return FALSE;
      any_requests |= hcmgrc->priv->portrait_requested != 0;

      /*
       * This is a workaround for the fullscreen incoming call dialog.
       * Since it's fullscreen we can safely assume it will cover
       * everything underneath, even if that's still visible in
       * clutter sense.  This is an evidence that we just cannot
       * rely on visibility checking entirely. TODO remove later
       */
      if (hcmgrc->priv->portrait_requested > 1
          || (hcmgrc->priv->portrait_requested && c->window
              && c->window->ewmh_state & MBWMClientWindowEWMHStateFullscreen))
        {
          PORTRAIT ("DEMANDED");
          break;
        }
    }

  PORTRAIT ("SHOULD BE: %d", any_requests);
  return any_requests;
}

/* Does any visible client request portrait mode?
 * Are all of them concerned prepared for it? */
gboolean
hd_comp_mgr_should_be_portrait (HdCompMgr *hmgr)
{
  return hd_comp_mgr_should_be_portrait_ignoring(hmgr, 0);
}

/*
 * Based on the visible windows decide whether we should be portrait or not.
 * Requires that the visibilities be sorted out.  Otherwise it doesn't work
 * correctly.  @c is the client being mapped, if the context is appropriate,
 * otherwise NULL.
 */
static void
hd_comp_mgr_portrait_or_not_portrait (MBWMCompMgr *mgr, MBWindowManagerClient *c)
{
  /* I think this is a guard for cases when we do a
   * set_state() -> portrait/unportrait() -> restack() */
  if (hd_render_manager_is_changing_state ())
    return;

  /* Undo hd_comp_mgr_portrait_forecast() if in the end it was false. */
  if (c && mb_wm_client_wants_portrait (c)
      && !STATE_IS_PORTRAIT (hd_render_manager_get_state ())
      && hd_transition_is_rotating_to_portrait ()
      && !hd_comp_mgr_should_be_portrait (HD_COMP_MGR (mgr)))
    {
      hd_transition_rotate_screen (mgr->wm, FALSE);
      return;
    }

  /*
   * Change state if necessary:
   * APP <=> APP_PORTRAIT and HOME <=> HOME_PORTRAIT
   */
  if (STATE_IS_PORTRAIT_CAPABLE (hd_render_manager_get_state ()))
    { /* Landscape -> portrait? */
      if (hd_comp_mgr_should_be_portrait (HD_COMP_MGR (mgr)))
        hd_render_manager_set_state_portrait ();
    }
  else if (STATE_IS_PORTRAIT (hd_render_manager_get_state ()))
    { /* Portrait -> landscape? */
      if (!hd_comp_mgr_should_be_portrait (HD_COMP_MGR (mgr)))
        hd_render_manager_set_state_unportrait ();
    }
}

gboolean
hd_comp_mgr_client_supports_portrait (MBWindowManagerClient *mbwmc)
{
  /* Don't mess with hd_comp_mgr_should_be_portrait()'s @counter. */
  return hd_comp_mgr_update_clients_portrait_flags (mbwmc, G_MAXUINT)
    ->priv->portrait_supported;
}

static void
hd_comp_mgr_check_do_not_disturb_flag (HdCompMgr *hmgr)
{
  HdCompMgrPrivate *priv = hmgr->priv;
  MBWindowManager *wm;
  Window xwindow;
  gboolean do_not_disturb_flag = FALSE;

  wm = MB_WM_COMP_MGR (hmgr)->wm;
  xwindow = hd_wm_current_app_is (NULL, 0);

  if (xwindow && wm->desktop && xwindow != wm->desktop->window->xwindow)
    {
      guint32 *value;
      Atom dnd;

      dnd = hd_comp_mgr_get_atom (hmgr, HD_ATOM_HILDON_DO_NOT_DISTURB);

      value = hd_util_get_win_prop_data_and_validate (wm->xdpy, xwindow,
                                                      dnd, XA_INTEGER,
                                                      32, 1, NULL);
      do_not_disturb_flag = (value && *value == 1);

      if (value)
        XFree (value);
    }

  /* Check change */
  if (priv->do_not_disturb_flag != do_not_disturb_flag)
    {
      priv->do_not_disturb_flag = do_not_disturb_flag;
      g_debug ("DND: %d", priv->do_not_disturb_flag);
    }
}

Atom
hd_comp_mgr_get_atom (HdCompMgr *hmgr, HdAtoms id)
{
  HdCompMgrPrivate *priv = hmgr->priv;

  if (id >= _HD_ATOM_LAST)
    return (Atom) 0;

  return priv->atoms[id];
}

ClutterActor *
hd_comp_mgr_get_home (HdCompMgr *hmgr)
{
  HdCompMgrPrivate *priv = hmgr->priv;

  return priv->home;
}

GObject *
hd_comp_mgr_get_switcher (HdCompMgr *hmgr)
{
  HdCompMgrPrivate *priv = hmgr->priv;

  return G_OBJECT(priv->switcher_group);
}

gint
hd_comp_mgr_get_current_home_view_id (HdCompMgr *hmgr)
{
  HdCompMgrPrivate *priv = hmgr->priv;

  return hd_home_get_current_view_id (HD_HOME (priv->home));
}

MBWindowManagerClient *
hd_comp_mgr_get_desktop_client (HdCompMgr *hmgr)
{
  HdCompMgrPrivate *priv = hmgr->priv;

  return priv->desktop;
}

#ifndef G_DEBUG_DISABLE
static void
dump_clutter_actor_tree (ClutterActor *actor, GString *indent)
{
  const gchar *name;
  MBWMCompMgrClient *cmgrc;
  ClutterGeometry geo;
  gint ax, ay;

  if (!indent)
    indent = g_string_new ("");

  if (!(name = clutter_actor_get_name (actor)) && CLUTTER_IS_LABEL (actor))
    name = clutter_label_get_text (CLUTTER_LABEL (actor));
  cmgrc = g_object_get_data(G_OBJECT (actor), "HD-MBWMCompMgrClutterClient");

  clutter_actor_get_geometry (actor, &geo);
  clutter_actor_get_anchor_point (actor, &ax, &ay);
  g_debug ("actor[%u]: %s%p (type=%s, name=%s, win=0x%lx), "
           "size: %ux%u%+d%+d[%d,%d], visible: %d, reactive: %d",
           indent->len, indent->str, actor,
           G_OBJECT_TYPE_NAME (actor), name,
           cmgrc && cmgrc->wm_client && cmgrc->wm_client->window
               ? cmgrc->wm_client->window->xwindow : 0,
           geo.width, geo.height, geo.x, geo.y, ax, ay,
           CLUTTER_ACTOR_IS_VISIBLE (actor) != 0,
           CLUTTER_ACTOR_IS_REACTIVE (actor) != 0);
  if (CLUTTER_IS_CONTAINER (actor))
    {
      g_string_append_c (indent, ' ');
      clutter_container_foreach (CLUTTER_CONTAINER (actor),
                                 (ClutterCallback)dump_clutter_actor_tree,
                                 indent);
      g_string_truncate (indent, indent->len-1);
    }
}
#endif

void
hd_comp_mgr_dump_debug_info (const gchar *tag)
{
#ifndef G_DEBUG_DISABLE
  Window focus;
  MBWMRootWindow *root;
  MBWindowManagerClient *mbwmc;
  int i, revert, ninputshapes, unused;
  XRectangle *inputshape;
  ClutterActor *stage;

  if (tag)
    g_debug ("%s", tag);

  g_debug ("Windows:");
  root = mb_wm_root_window_get (NULL);
  mb_wm_stack_enumerate_reverse (root->wm, mbwmc)
    {
      MBGeometry geo;
      const HdApp *app;

      geo = mbwmc->window ? mbwmc->window->geometry : mbwmc->frame_geometry;
      g_debug (" client=%p, type=%d, size=%dx%d%+d%+d, trfor=%p, layer=%d, "
               "stkidx=%d, win=0x%lx, group=0x%lx, name=%s",
               mbwmc, MB_WM_CLIENT_CLIENT_TYPE (mbwmc), MBWM_GEOMETRY (&geo),
               mbwmc->transient_for, mbwmc->stacking_layer,
               HD_IS_APP (mbwmc) ? HD_APP (mbwmc)->stack_index : -1,
               mbwmc->window ? mbwmc->window->xwindow : 0,
               mbwmc->window ? mbwmc->window->xwin_group : 0,
               mbwmc->window ? mbwmc->window->name : "<unset>");
      if (HD_IS_APP (mbwmc) && (app = HD_APP (mbwmc))->followers)
        {
          const GList *li;

          g_debug ("  followers:");
          for (li = app->followers; li; li = li->next)
            g_debug ("   %p", li->data);
        }
    }
  mb_wm_object_unref (MB_WM_OBJECT (root));

  g_debug ("input:");
  XGetInputFocus (clutter_x11_get_default_display (), &focus, &revert);
  g_debug ("  focus: 0x%lx", focus);
  if (revert == RevertToParent)
    g_debug ("  reverts to parent");
  else if (revert == RevertToPointerRoot)
    g_debug ("  reverts to pointer root");
  else if (revert == RevertToNone)
    g_debug ("  reverts to none");
  else
    g_debug ("  reverts to %d", revert);

  g_debug ("  shape:");
  stage = clutter_stage_get_default ();
  inputshape = XShapeGetRectangles(root->wm->xdpy,
                   clutter_x11_get_stage_window (CLUTTER_STAGE (stage)),
                   ShapeInput, &ninputshapes, &unused);
  for (i = 0; i < ninputshapes; i++)
    g_debug ("    %dx%d%+d%+d", MBWM_GEOMETRY(&inputshape[i]));
  XFree(inputshape);

  dump_clutter_actor_tree (clutter_stage_get_default (), NULL);
  hd_app_mgr_dump_app_list (TRUE);
#endif
}

void hd_comp_mgr_set_effect_running(HdCompMgr *hmgr, gboolean running)
{
  /* We don't need this now, but this might be useful in the future.
   * It is called when any transition begins or ends. */
}

/* Return the time since the last window was mapped (in ms). This
 * is used in the _launch dbus call to check that the window we
 * were asked to do a transition for hasn't actually mapped before
 * we got the dbug message.
 */
gint hd_comp_mgr_time_since_last_map(HdCompMgr *hmgr)
{
  struct timeval current;
  gettimeofday(&current, NULL);

  return ((current.tv_sec - hmgr->priv->last_map_time.tv_sec) * 1000) +
         ((current.tv_usec - hmgr->priv->last_map_time.tv_usec) / 1000);
}

void
hd_comp_mgr_update_applets_on_current_desktop_property (HdCompMgr *hmgr)
{
  HdHome *home = HD_HOME (hmgr->priv->home);
  GSList *applets = NULL, *a;
  GSList *views, *v;

  applets = hd_home_view_get_all_applets (HD_HOME_VIEW (
                                          hd_home_get_current_view (home)));


  mb_wm_util_async_trap_x_errors (MB_WM_COMP_MGR(hmgr)->wm->xdpy);
  /* Handle applets on current view */
  for (a = applets; a; a = a->next)
    {
      MBWindowManagerClient *wm_client
              = MB_WM_COMP_MGR_CLIENT (a->data)->wm_client;
      guint32 on_desktop = 1;
      if (STATE_NEED_DESKTOP (hd_render_manager_get_state ()) &&
          STATE_SHOW_APPLETS (hd_render_manager_get_state ()))
        {
          XChangeProperty (wm_client->wmref->xdpy,
                           wm_client->window->xwindow,
                           hd_comp_mgr_get_atom (hmgr,
                                HD_ATOM_HILDON_APPLET_ON_CURRENT_DESKTOP),
                           XA_CARDINAL,
                           32,
                           PropModeReplace,
                           (const guchar *) &on_desktop,
                           1);
        }
      else
        {
          XDeleteProperty (wm_client->wmref->xdpy,
                           wm_client->window->xwindow,
                           hd_comp_mgr_get_atom (hmgr,
                                HD_ATOM_HILDON_APPLET_ON_CURRENT_DESKTOP));
        }
    }
  g_slist_free (applets);

  views = hd_home_get_not_visible_views (home);
  for (v = views; v; v = v->next)
    {
      applets = hd_home_view_get_all_applets (HD_HOME_VIEW (v->data));
      for (a = applets; a; a = a->next)
        {
          MBWindowManagerClient *wm_client
                  = MB_WM_COMP_MGR_CLIENT (a->data)->wm_client;
          XDeleteProperty (wm_client->wmref->xdpy,
                           wm_client->window->xwindow,
                           hd_comp_mgr_get_atom (hmgr,
                                HD_ATOM_HILDON_APPLET_ON_CURRENT_DESKTOP));
        }
      g_slist_free (applets);
    }
  g_slist_free (views);

  mb_wm_util_async_untrap_x_errors ();
}

guint
hd_comp_mgr_get_current_screen_width (void)
{
  return hd_mb_wm->xdpy_width;
}

guint
hd_comp_mgr_get_current_screen_height(void)
{
  return hd_mb_wm->xdpy_height;
}

gboolean 
hd_comp_mgr_is_portrait(void)
{ /* This is a very typesafe macro. */
  return hd_mb_wm->xdpy_width < hd_mb_wm->xdpy_height;
}

gboolean
hd_comp_mgr_client_is_maximized (MBGeometry geom)
{

  if (geom.x != 0 || geom.y != 0)
    return FALSE;
  if (geom.width >= hd_mb_wm->xdpy_width && geom.height >= hd_mb_wm->xdpy_height)
    return TRUE;
  if (geom.width >= hd_mb_wm->xdpy_height && geom.height >= hd_mb_wm->xdpy_width)
    /* Client covers the rotated screen.  If we select it as the CURRENT_APP,
     * we'll rotate [back] and everything will make sense. */
    return TRUE;

  return FALSE;
}

