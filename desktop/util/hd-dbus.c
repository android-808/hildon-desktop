#include <unistd.h>

#include "hd-dbus.h"
#include "hd-switcher.h"
#include "hd-render-manager.h"

#include <glib.h>
/*
 * Application killer DBus interface
 */
#define APPKILLER_SIGNAL_INTERFACE "com.nokia.osso_app_killer"
#define APPKILLER_SIGNAL_PATH      "/com/nokia/osso_app_killer"
#define APPKILLER_SIGNAL_NAME      "exit"

#define TASKNAV_SIGNAL_INTERFACE   "com.nokia.hildon_desktop"
#define TASKNAV_SIGNAL_NAME        "exit_app_view"

#define DSME_SIGNAL_INTERFACE "com.nokia.dsme.signal"
#define DSME_SHUTDOWN_SIGNAL_NAME "shutdown_ind"

gboolean hd_dbus_display_is_off = FALSE;
gboolean hd_dbus_tklock_on = FALSE;

static DBusConnection *connection, *sysbus_conn;

static DBusHandlerResult
hd_dbus_signal_handler (DBusConnection *conn, DBusMessage *msg, void *data)
{
  HdCompMgr  * hmgr = data;

  if (dbus_message_is_signal(msg,
			     APPKILLER_SIGNAL_INTERFACE,
			     APPKILLER_SIGNAL_NAME))
    {
      /* kill -TERM all programs started from the launcher unconditionally,
       * this signal is used by Backup application */
      hd_comp_mgr_kill_all_apps (hmgr);

      return DBUS_HANDLER_RESULT_HANDLED;
    }
  else if (dbus_message_is_signal(msg,
                                  TASKNAV_SIGNAL_INTERFACE,
                                  TASKNAV_SIGNAL_NAME))
    {
      if (STATE_IS_APP (hd_render_manager_get_state ()))
        hd_render_manager_set_state (HDRM_STATE_TASK_NAV);
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
hd_dbus_system_bus_signal_handler (DBusConnection *conn,
                                   DBusMessage *msg, void *data)
{
  HdCompMgr  * hmgr = data;
  extern MBWindowManager *hd_mb_wm;

  if (dbus_message_is_signal(msg, DSME_SIGNAL_INTERFACE,
			     DSME_SHUTDOWN_SIGNAL_NAME))
    {
      Window overlay;
      g_warning ("%s: " DSME_SHUTDOWN_SIGNAL_NAME " from DSME", __func__);
      /* send TERM to applications and exit without cleanup */
      
      hd_comp_mgr_kill_all_apps (hmgr);
      overlay = mb_wm_comp_mgr_clutter_get_overlay_window (
                        MB_WM_COMP_MGR_CLUTTER (hmgr));
      if (overlay != None)
        {
          /* needed because of the non-composite optimisations in X,
           * otherwise we could show garbage if the shutdown screen is
           * a bit slow or missing */
          XClearWindow (hd_mb_wm->xdpy, overlay);
          XFlush (hd_mb_wm->xdpy);
        }
      _exit (0);
    }
#if 0
  else if (dbus_message_is_signal (msg, MCE_SIGNAL_IF, "tklock_mode_ind"))
    {
      const char *mode;
      if (dbus_message_get_args (msg, NULL, DBUS_TYPE_STRING, &mode,
                                 DBUS_TYPE_INVALID));
        {
          if (strcmp(mode, MCE_TK_LOCKED))
            {
              if (hd_dbus_tklock_on)
                {
                  hd_dbus_tklock_on = FALSE;
                  /* if we avoided focusing a window during tklock, do it now
                   * (this only has an effect if no window is currently
                   * focused) */
                  mb_wm_unfocus_client (hd_mb_wm, NULL);
                }
            }
          else
            {
              hd_dbus_tklock_on = TRUE;
            }
        }
    }
  else if (dbus_message_is_signal (msg, MCE_SIGNAL_IF, "display_status_ind"))
    {
      DBusMessageIter iter;

      if (dbus_message_iter_init(msg, &iter))
        {
          char *str = NULL;
          dbus_message_iter_get_basic(&iter, &str);
          if (str)
            {
              if (strcmp (str, "on") == 0)
                {
                  ClutterActor *stage = clutter_stage_get_default ();
                  /* Allow redraws again... */
                  clutter_actor_show(
                      CLUTTER_ACTOR(hd_render_manager_get()));
                  clutter_actor_set_allow_redraw(stage, TRUE);
                  /* make a blocking redraw to draw any new window (such as
                   * the "swipe to unlock") first, otherwise just a black
                   * screen will be visible (see below) */
                  hd_dbus_display_is_off = FALSE;
                  clutter_redraw (CLUTTER_STAGE (stage));
                }
              else if (strcmp (str, "off") == 0)
                {
                  ClutterActor *stage = clutter_stage_get_default ();
                  /* Stop redraws from anything. We do this on the stage
                   * because Rotation does it on HDRM, and we don't want to
                   * conflict. */
                  clutter_actor_hide(
                      CLUTTER_ACTOR(hd_render_manager_get()));
                  clutter_actor_set_allow_redraw(stage, FALSE);
                  hd_dbus_display_is_off = TRUE;
                  /* Hiding before set_allow_redraw will queue a redraw,
                   * which will draw a black screen (because hdrm is hidden).
                   * This is needed for bug 139928 so that there is
                   * absolutely no flicker of the previous screen
                   * contents before the lock window appears. */
                  clutter_redraw (CLUTTER_STAGE (stage));
                }

              hd_comp_mgr_update_applets_on_current_desktop_property (hmgr);
            }
        }
    }
#endif
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
#if 0
static void
hd_dbus_prevent_display_blanking (void)
{
  DBusMessage *msg;
  dbus_bool_t b;

  if (sysbus_conn == NULL) {
    g_warning ("%s: no D-Bus system bus connection", __func__);
    return;
  }
  msg = dbus_message_new_method_call(MCE_SERVICE, MCE_REQUEST_PATH,
                                     MCE_REQUEST_IF, MCE_PREVENT_BLANK_REQ);
  if (msg == NULL) {
    g_warning ("%s: could not create message", __func__);
    return;
  }

  b = dbus_connection_send(sysbus_conn, msg, NULL);
  if (!b) {
    g_warning ("%s: dbus_connection_send() failed", __func__);
  } else {
    dbus_connection_flush(sysbus_conn);
  }
  dbus_message_unref(msg);
}

static gboolean
display_timeout_f (gpointer unused)
{
  /* g_warning ("%s: called", __func__); */
  hd_dbus_prevent_display_blanking ();
  return TRUE;
}

/* keeps the display lit by sending periodical messages to MCE's D-Bus
 * interface */
void
hd_dbus_disable_display_blanking (gboolean setting)
{
  static guint timeout_f = 0;

  if (setting)
    {
      hd_dbus_prevent_display_blanking ();
      if (!timeout_f)
        timeout_f = g_timeout_add (30 * 1000, display_timeout_f, NULL);
    }
  else if (timeout_f)
    {
      g_source_remove (timeout_f);
      timeout_f = 0;
    }
}
#endif
DBusConnection *
hd_dbus_init (HdCompMgr * hmgr)
{
  DBusError       error;

  dbus_error_init (&error);

  connection = dbus_bus_get (DBUS_BUS_SESSION, &error);
  if (dbus_error_is_set (&error))
    {
      g_warning ("Could not open D-Bus session bus connection.");
      dbus_error_free (&error);
    }
  sysbus_conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);

  if (!connection || !sysbus_conn)
    {
      g_warning ("Could not open D-Bus session/system bus connection.");
      dbus_error_free (&error);
    }
  else
    {
      /* session bus */
      dbus_bus_add_match (connection, "type='signal', interface='"
                          APPKILLER_SIGNAL_INTERFACE "'", NULL);

      dbus_bus_add_match (connection, "type='signal', interface='"
                          TASKNAV_SIGNAL_INTERFACE "'", NULL);

      dbus_connection_add_filter (connection, hd_dbus_signal_handler,
				  hmgr, NULL);

      /* system bus */
      dbus_bus_add_match (sysbus_conn, "type='signal', interface='"
                          DSME_SIGNAL_INTERFACE "'", NULL);

      dbus_connection_add_filter (sysbus_conn,
                                  hd_dbus_system_bus_signal_handler,
                                  hmgr, NULL);
    }

  return connection;
}