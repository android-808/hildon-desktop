/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author:  Marc Ordinas i Llopis <marc.ordinasillopis@collabora.co.uk>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "hd-launcher.h"
#include "hd-launcher-tile.h"
#include "hd-launcher-grid.h"
#include "hd-launcher-config.h"

#include <glib-object.h>
#include <clutter/clutter.h>
#include <stdlib.h>
#include <gtk/gtk.h>

#include "hd-desktop-config.h"
#include "tidy/tidy-highlight.h"
#include "hd-transition.h"

#define I_(str) (g_intern_static_string ((str)))
#define HD_PARAM_READWRITE (G_PARAM_READWRITE | \
                            G_PARAM_STATIC_NICK | \
                            G_PARAM_STATIC_NAME | \
                            G_PARAM_STATIC_BLURB)

#define HD_LAUNCHER_TILE_GET_PRIVATE(obj)       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), HD_TYPE_LAUNCHER_TILE, HdLauncherTilePrivate))

struct _HdLauncherTilePrivate
{
  gchar *icon_name;
  gchar *text;

  ClutterActor *icon;
  ClutterActor *label;
  TidyHighlight *icon_glow;
  ClutterTimeline *glow_timeline;

  ClutterActor *click_area;

  float glow_amount;
  float glow_radius; // radius of glow - loaded from transitions.ini

  /* We need to know if there's been scrolling. */
  gboolean is_pressed;
};

enum
{
  PROP_0,

  PROP_LAUNCHER_TILE_ICON_NAME,
  PROP_LAUNCHER_TILE_TEXT
};

enum
{
  CLICKED,

  LAST_SIGNAL
};

static guint launcher_tile_signals[LAST_SIGNAL] = { 0, };

/* Forward declarations */
/*   GObject */
static void hd_launcher_tile_dispose (GObject *gobject);
static void hd_launcher_tile_finalize (GObject *gobject);
static void hd_launcher_tile_get_property (GObject    *object,
                                           guint       property_id,
                                           GValue     *value,
                                           GParamSpec *pspec);
static void hd_launcher_tile_set_property (GObject      *gobject,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec);
/* ClutterActor */
static gboolean hd_launcher_tile_button_press (ClutterActor       *actor);
static gboolean hd_launcher_tile_button_release (ClutterActor       *actor);
static void hd_launcher_on_glow_frame(ClutterTimeline *timeline,
                                      gint frame_num,
                                      ClutterActor *actor);
static void hd_launcher_tile_allocate (ClutterActor          *self,
                                       const ClutterActorBox *box,
                                       gboolean       absolute_origin_changed);

G_DEFINE_TYPE (HdLauncherTile, hd_launcher_tile, CLUTTER_TYPE_GROUP);

static void
hd_launcher_tile_class_init (HdLauncherTileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  GParamSpec *pspec;

  g_type_class_add_private (klass, sizeof (HdLauncherTilePrivate));

  gobject_class->get_property = hd_launcher_tile_get_property;
  gobject_class->set_property = hd_launcher_tile_set_property;
  gobject_class->dispose      = hd_launcher_tile_dispose;
  gobject_class->finalize     = hd_launcher_tile_finalize;

  actor_class->allocate = hd_launcher_tile_allocate;

  pspec = g_param_spec_string ("icon-name",
                               "Icon Name",
                               "Name of the icon to display",
                               hd_launcher_config_get_default_icon (),
                               G_PARAM_CONSTRUCT | HD_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_LAUNCHER_TILE_ICON_NAME, pspec);
  pspec = g_param_spec_string ("text",
                               "Text",
                               "Text to display",
                               "Unknown",
                               G_PARAM_CONSTRUCT | HD_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_LAUNCHER_TILE_TEXT, pspec);

  launcher_tile_signals[CLICKED] =
    g_signal_new (I_("clicked"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}

static void
hd_launcher_tile_init (HdLauncherTile *tile)
{
  HdLauncherTilePrivate *priv =
        tile->priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);
  gint tile_width, tile_height;

  hd_launcher_config_get_tile_size (&tile_width, &tile_height);

  clutter_actor_set_name(CLUTTER_ACTOR(tile), "HdLauncherTile");
  clutter_actor_set_size(CLUTTER_ACTOR(tile), tile_width, tile_height);

  clutter_actor_show(CLUTTER_ACTOR(tile));
#ifdef MAEMO_CHANGES
  /* Explicitly enable maemo-specific visibility detection to cut down
   * spurious paints */
  clutter_actor_set_visibility_detect(CLUTTER_ACTOR(tile), TRUE);
#endif
  /* We have a 'click area' because when the tile is near the side of the
   * screen, the click area is actually clipped to the margins. This
   * is done on an overridden allocate function.
   *
   * It's good that we can make this actor a rectangle and see exactly
   * where the user is allowed to click - but to make it not draw anything
   * but still be selectable */
  if (TRUE)
    priv->click_area = clutter_group_new();
  else
    {
      ClutterColor red = {0xFF, 0x00, 0x00, 0x3F};
      priv->click_area = clutter_rectangle_new_with_color(&red);
    }

  clutter_actor_set_name(priv->click_area, "HdLauncherTile::click_area");
  clutter_actor_set_reactive(priv->click_area, TRUE);
  clutter_actor_set_position(priv->click_area, 0, 0);
  clutter_actor_set_size(priv->click_area, tile_width, tile_height);
  
clutter_container_add_actor(CLUTTER_CONTAINER(tile), priv->click_area);

  g_signal_connect_swapped(priv->click_area, "button-press-event",
                           G_CALLBACK (hd_launcher_tile_button_press), tile);
  g_signal_connect_swapped(priv->click_area, "button-release-event",
                           G_CALLBACK (hd_launcher_tile_button_release), tile);

  tile->priv->glow_timeline = clutter_timeline_new_for_duration(200);
  g_signal_connect(tile->priv->glow_timeline, "new-frame",
                   G_CALLBACK (hd_launcher_on_glow_frame), tile);
}

HdLauncherTile *
hd_launcher_tile_new (const gchar *icon_name, const gchar *text)
{
  return g_object_new (HD_TYPE_LAUNCHER_TILE,
                       "icon-name", icon_name,
                       "text", text,
                       NULL);
}

const gchar *
hd_launcher_tile_get_icon_name (HdLauncherTile *tile)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);

  return priv->icon_name;
}

const gchar *
hd_launcher_tile_get_text (HdLauncherTile *tile)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);

  return priv->text;
}

static void
hd_launcher_tile_get_property (GObject    *object,
                               guint       property_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  HdLauncherTile *tile = HD_LAUNCHER_TILE (object);

  switch (property_id)
    {
    case PROP_LAUNCHER_TILE_ICON_NAME:
      g_value_set_string (value,
          hd_launcher_tile_get_icon_name (tile));
      break;

    case PROP_LAUNCHER_TILE_TEXT:
      g_value_set_string (value,
          hd_launcher_tile_get_text (tile));
      break;

    default:
      /* We don't have any other property... */
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

ClutterActor *
hd_launcher_tile_get_icon (HdLauncherTile *tile)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);

  return priv->icon;
}

ClutterActor *
hd_launcher_tile_get_label (HdLauncherTile *tile)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);

  return priv->label;
}

void
hd_launcher_tile_set_icon_name (HdLauncherTile *tile,
                                const gchar *icon_name)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);
  GtkIconTheme *icon_theme;
  GtkIconInfo *info;
  GdkPixbuf *pixbuf;
  gint real_icon_size,
       tile_icon_size,
       glow_size, 
       tile_width, 
       tile_height;
 
  hd_launcher_config_get_icons_size (&real_icon_size, 
				     &tile_icon_size, 
				     &glow_size);

  hd_launcher_config_get_tile_size (&tile_width, &tile_height);

  if (priv->icon_name)
    {
      g_free (priv->icon_name);
    }
  if (icon_name)
    priv->icon_name = g_strdup (icon_name);
  else
    /* Set the default if none was passed. */
    priv->icon_name = g_strdup (hd_launcher_config_get_default_icon ());

  /* Recreate the icon actor */
  if (priv->icon)
    {
      clutter_actor_destroy (priv->icon);
      priv->icon = NULL;
    }

  icon_theme = gtk_icon_theme_get_default();
  info = gtk_icon_theme_lookup_icon(icon_theme, priv->icon_name,
                                    real_icon_size,
                                    GTK_ICON_LOOKUP_NO_SVG);
  if (info == NULL)
    {
      /* Try to get the default icon. */
      g_free (priv->icon_name);
      priv->icon_name = g_strdup (hd_launcher_config_get_default_icon ());
      info = gtk_icon_theme_lookup_icon(icon_theme, priv->icon_name,
                                        real_icon_size,
                                        GTK_ICON_LOOKUP_NO_SVG);
    }
  if (info == NULL)
    {
      g_warning ("%s: couldn't find icon %s\n", __FUNCTION__, priv->icon_name);
      g_free (priv->icon_name);
      priv->icon_name = NULL;
      return;
    }

  const gchar *fname = gtk_icon_info_get_filename(info);
  if (fname == NULL)
    {
      g_warning ("%s: couldn't get icon %s\n", __FUNCTION__, priv->icon_name);
      g_free (priv->icon_name);
      priv->icon_name = NULL;
      return;
    }

  /* We must expand these images so there is a 1 pixel transparent
   * border around them, or the glow effect won't work properly. We use
   * gdk_pixbuf_new_from_file_at_size as the pixbuf pointed to by fname
   * isn't actually guaranteed to be the correct size.  */
  pixbuf = 
   gdk_pixbuf_new_from_file_at_size (fname,real_icon_size, real_icon_size, 0);

  if (pixbuf)
    {
      gint w = gdk_pixbuf_get_width(pixbuf);
      gint h = gdk_pixbuf_get_height(pixbuf);
      GdkPixbuf *pixbufb = gdk_pixbuf_new(
          GDK_COLORSPACE_RGB, TRUE, 8, w+2, h+2);

      gdk_pixbuf_fill(pixbufb, 0);
      gdk_pixbuf_copy_area(pixbuf, 0, 0, w, h,
                           pixbufb, 1, 1);

      priv->icon = clutter_texture_new();
      clutter_texture_set_from_rgb_data(
          CLUTTER_TEXTURE(priv->icon),
          gdk_pixbuf_get_pixels(pixbufb),
          gdk_pixbuf_get_has_alpha(pixbufb),
          gdk_pixbuf_get_width(pixbufb),
          gdk_pixbuf_get_height(pixbufb),
          gdk_pixbuf_get_rowstride(pixbufb),
          gdk_pixbuf_get_n_channels(pixbufb), 0, 0);
      g_object_unref(pixbufb);
    }
  if (pixbuf)
    g_object_unref(pixbuf);

  if (!priv->icon)
  {
    g_warning ("%s: couldn't create texture for %s\n", __FUNCTION__, fname);
    g_free (priv->icon_name);
    priv->icon_name = NULL;
    return;
  }

  clutter_actor_set_size (priv->icon, tile_icon_size,tile_icon_size);

  clutter_actor_set_position (priv->icon,
      			      (tile_width - tile_icon_size) / 2,
      			      hd_launcher_config_get_default_margin ()/2);

  clutter_container_add_actor (CLUTTER_CONTAINER(tile), priv->icon);

  gtk_icon_info_free(info);

  if (priv->icon_glow)
    /* free the old one */
    clutter_actor_destroy (CLUTTER_ACTOR (priv->icon_glow));

  priv->icon_glow = tidy_highlight_new(CLUTTER_TEXTURE(priv->icon));
  clutter_actor_set_size (CLUTTER_ACTOR(priv->icon_glow),
        		  glow_size,
        		  glow_size);

  clutter_actor_set_position (CLUTTER_ACTOR(priv->icon_glow),
        		      (tile_width - glow_size) / 2,
        	 	       (hd_launcher_config_get_default_margin ()/2) +
        		       (tile_icon_size - glow_size) / 2);

  clutter_container_add_actor (CLUTTER_CONTAINER(tile),
                               CLUTTER_ACTOR(priv->icon_glow));

  clutter_actor_lower_bottom(CLUTTER_ACTOR(priv->icon_glow));

  clutter_actor_hide(CLUTTER_ACTOR(priv->icon_glow));
}

void
hd_launcher_tile_set_text (HdLauncherTile *tile,
                           const gchar *text)
{
  ClutterColor text_color = {0xFF, 0xFF, 0xFF, 0xFF};
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);
  ClutterUnit label_width;
  guint label_height, label_width_px;
  gint tile_width, tile_height, real_icon_size;

  hd_launcher_config_get_tile_size (&tile_width, &tile_height);
  hd_launcher_config_get_icons_size (&real_icon_size, NULL, NULL);

  if (!text)
    return;

  if (priv->text)
    {
      g_free (priv->text);
    }
  priv->text = g_strdup (text);

  /* Recreate the label actor */
  if (priv->label)
    {
      clutter_actor_destroy (priv->label);
    }

  priv->label = 
    clutter_label_new_full (hd_launcher_config_get_tile_font(),
			    priv->text, &text_color);

  clutter_actor_set_name(priv->label, "HdLauncherTile::label");

  /* FIXME: This is a huge work-around because clutter/pango do not
   * support setting ellipsize to NONE and wrap to FALSE.
   */
  clutter_label_set_line_wrap (CLUTTER_LABEL (priv->label), TRUE);
  clutter_label_set_ellipsize (CLUTTER_LABEL (priv->label),
                               PANGO_ELLIPSIZE_NONE);
  clutter_label_set_alignment (CLUTTER_LABEL (priv->label),
                               PANGO_ALIGN_CENTER);
  clutter_label_set_line_wrap_mode (CLUTTER_LABEL (priv->label),
                                    PANGO_WRAP_CHAR);

  label_height = 
    tile_height - (real_icon_size + hd_launcher_config_get_default_margin ());

  clutter_actor_get_preferred_width (priv->label,
    				     CLUTTER_UNITS_FROM_DEVICE(label_height),
                              	     NULL, &label_width);

  label_width_px = MIN (CLUTTER_UNITS_TO_DEVICE(label_width),
                        tile_width);

  clutter_actor_set_size(priv->label, label_width_px, label_height);
  clutter_actor_set_position(priv->label,
      			     (tile_width - label_width_px) / 2,
      			     tile_height - label_height);

  clutter_container_add_actor (CLUTTER_CONTAINER(tile), priv->label);

  if (CLUTTER_UNITS_TO_DEVICE(label_width) > tile_width)
    clutter_actor_set_clip (priv->label, 0, 0,
                  	    tile_width, label_height);
}

static void
hd_launcher_tile_set_property (GObject      *gobject,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  switch (prop_id)
    {
    case PROP_LAUNCHER_TILE_ICON_NAME:
      hd_launcher_tile_set_icon_name (HD_LAUNCHER_TILE (gobject),
                                      g_value_get_string (value));
      break;

    case PROP_LAUNCHER_TILE_TEXT:
      hd_launcher_tile_set_text (HD_LAUNCHER_TILE (gobject),
                                 g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
hd_launcher_on_glow_frame(ClutterTimeline *timeline,
                          gint frame_num,
                          ClutterActor *actor)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (actor);

  priv->glow_amount = frame_num /
                      (float)clutter_timeline_get_n_frames(timeline);
  if (priv->icon_glow)
    tidy_highlight_set_amount(priv->icon_glow,
                              priv->glow_amount * priv->glow_radius);
  else
    return;

  if (priv->glow_amount != 0)
    clutter_actor_show(CLUTTER_ACTOR(priv->icon_glow));
  else
    clutter_actor_hide(CLUTTER_ACTOR(priv->icon_glow));
}

static void
hd_launcher_tile_set_glow(HdLauncherTile *tile, gboolean glow)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);
  ClutterColor glow_col = {0xFF, 0xFF, 0x7F, 0xFF};
  float glow_brightness;
  gint n_frames;

  clutter_timeline_stop(priv->glow_timeline);

  /* If we're already there, skip */
  if ((glow && priv->glow_amount==1) ||
      (!glow && priv->glow_amount==0))
    return;

  clutter_timeline_set_duration(priv->glow_timeline,
          hd_transition_get_int("launcher_glow",
              glow ? "duration_in" : "duration_out",
              500));

  clutter_timeline_set_direction(priv->glow_timeline,
       glow ? CLUTTER_TIMELINE_FORWARD : CLUTTER_TIMELINE_BACKWARD);

  /* Set our start position based on how much glow we had previously */
  n_frames = clutter_timeline_get_n_frames(priv->glow_timeline);
  clutter_timeline_advance(priv->glow_timeline,
      (int)(priv->glow_amount*n_frames));

  /* set our glow colour from the theme */
  glow_brightness = hd_transition_get_double("launcher_glow", "brightness", 1);
  hd_desktop_config_get_color (HD_TXT_COLOR, &glow_col);

  glow_col.alpha = (int)(glow_col.alpha * glow_brightness);
  tidy_highlight_set_color(priv->icon_glow, &glow_col);
  /* load our glow radius */
  priv->glow_radius = hd_transition_get_double("launcher_glow", "radius", 8);


  clutter_timeline_start(priv->glow_timeline);
}

static gboolean
hd_launcher_tile_button_press (ClutterActor       *actor)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (actor);

  /* Unglow everything else, but glow this tile */
  hd_launcher_tile_set_glow(HD_LAUNCHER_TILE(actor), TRUE);
  /* Set the 'pressed' flag */
  priv->is_pressed = TRUE;

  return TRUE;
}

static gboolean
hd_launcher_tile_button_release (ClutterActor       *actor)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (actor);

  if (!priv->is_pressed)
    return TRUE;

  priv->is_pressed = FALSE;

  g_signal_emit (actor, launcher_tile_signals[CLICKED], 0);

  return TRUE;
}

static void
hd_launcher_tile_dispose (GObject *gobject)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (gobject);

  if (priv->glow_timeline)
    {
      clutter_timeline_stop(priv->glow_timeline);
      g_object_unref(priv->glow_timeline);
      priv->glow_timeline = 0;
    }
  if (priv->label)
    {
      clutter_actor_destroy (priv->label);
      priv->label = 0;
    }
  if (priv->icon_glow)
    {
      clutter_actor_destroy (CLUTTER_ACTOR(priv->icon_glow));
      priv->icon_glow = 0;
    }
  if (priv->icon)
    {
      clutter_actor_destroy (priv->icon);
      priv->icon = 0;
    }
  G_OBJECT_CLASS (hd_launcher_tile_parent_class)->dispose (gobject);
}

static void
hd_launcher_tile_finalize (GObject *gobject)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (gobject);

  g_free (priv->icon_name);
  g_free (priv->text);

  G_OBJECT_CLASS (hd_launcher_tile_parent_class)->finalize (gobject);
}

static void
hd_launcher_tile_allocate (ClutterActor          *self,
                           const ClutterActorBox *box,
                           gboolean               absolute_origin_changed)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (self);

  gint right_margin;
  gint box_x1 = CLUTTER_UNITS_TO_INT(box->x1);
  gint box_x2 = CLUTTER_UNITS_TO_INT(box->x2);

  gint xmin,xmax,default_margin,tile_width,margin_left,margin_right;

  hd_launcher_config_get_margins_size (&margin_left, &margin_right, NULL, NULL);
  hd_launcher_config_get_tile_size (&tile_width, NULL);
  
  default_margin = hd_launcher_config_get_default_margin ();

  right_margin = hd_comp_mgr_get_current_screen_width () - margin_right; 

  /* Set our default click area - we position our icons default margin
   * apart, so make us extend sideways a bit so there are no gaps */
  xmin = -default_margin/2;
  xmax = tile_width + default_margin/2;

  /* When this tile is moved around, set our click area up so that
   * it is clipped to the margins */
  if (box_x1 < margin_left)
    xmin = margin_left - box_x1;
  if (box_x2 > right_margin)
    xmax = tile_width - (box_x2 - right_margin);

  clutter_actor_set_x(priv->click_area, xmin);
  clutter_actor_set_width(priv->click_area, xmax-xmin);

  CLUTTER_ACTOR_CLASS (hd_launcher_tile_parent_class)->allocate (
      self, box, absolute_origin_changed);
}

/* Reset this tile to the state it should be in when first shown */
void hd_launcher_tile_reset(HdLauncherTile *tile)
{
  HdLauncherTilePrivate *priv = HD_LAUNCHER_TILE_GET_PRIVATE (tile);
  /* remove glow */
  hd_launcher_tile_set_glow (tile, FALSE);
  priv->is_pressed = FALSE;
}