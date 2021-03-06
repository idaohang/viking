/*
 * viking -- GPS Data and Topo Analyzer, Explorer, and Manager
 *
 * Copyright (C) 2003-2005, Evan Battaglia <gtoevan@gmx.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <string.h>
#include "globals.h"
#include "mapcache.h"
#include "preferences.h"
#include "vik_compat.h"

typedef struct _List {
  struct _List *next;
  gchar *key;
} List;

/* a circular linked list, a pointer to the tail, and the tail points to the head */
/* this is so we can free the last */
static List *queue_tail = NULL;
static int queue_count = 0;

static guint32 cache_size = 0;
static guint32 max_cache_size = VIK_CONFIG_MAPCACHE_SIZE * 1024 * 1024;

static GHashTable *cache = NULL;

static GMutex *mc_mutex = NULL;

#define HASHKEY_FORMAT_STRING "%d-%d-%d-%d-%d-%d-%d-%.3f-%.3f"
#define HASHKEY_FORMAT_STRING_NOSHRINK_NOR_ALPHA "%d-%d-%d-%d-%d-%d-"

static VikLayerParamScale params_scales[] = {
  /* min, max, step, digits (decimal places) */
 { 1, 1024, 1, 0 },
};

static VikLayerParam prefs[] = {
  { VIK_LAYER_NUM_TYPES, VIKING_PREFERENCES_NAMESPACE "mapcache_size", VIK_LAYER_PARAM_UINT, VIK_LAYER_GROUP_NONE, N_("Map cache memory size (MB):"), VIK_LAYER_WIDGET_HSCALE, params_scales, NULL, NULL, NULL, NULL, NULL },
};

void a_mapcache_init ()
{
  VikLayerParamData tmp;
  tmp.u = VIK_CONFIG_MAPCACHE_SIZE;
  a_preferences_register(prefs, tmp, VIKING_PREFERENCES_GROUP_KEY);

  mc_mutex = vik_mutex_new ();
  cache = g_hash_table_new_full ( g_str_hash, g_str_equal, g_free, g_object_unref );
}

static void cache_add(gchar *key, GdkPixbuf *pixbuf)
{
#if !GLIB_CHECK_VERSION(2,26,0)
  // Only later versions of GLib actually return a value for this function
  // Annoyingly the documentation doesn't say anything about this interface change :(
  if ( g_hash_table_insert ( cache, key, pixbuf ) )
#else
  g_hash_table_insert ( cache, key, pixbuf );
#endif
  {
    cache_size += gdk_pixbuf_get_rowstride(pixbuf) * gdk_pixbuf_get_height(pixbuf);
    cache_size += 100;
  }
}

static void cache_remove(const gchar *key)
{
  GdkPixbuf *buf = g_hash_table_lookup ( cache, key );
  if (buf) {
    cache_size -= gdk_pixbuf_get_rowstride(buf) * gdk_pixbuf_get_height(buf);
    cache_size -= 100;
    g_hash_table_remove ( cache, key );
  }
}

/* returns key from head, adds on newtailkey to tail. */
static gchar *list_shift_add_entry ( gchar *newtailkey )
{
  gchar *oldheadkey = queue_tail->next->key;
  queue_tail->next->key = newtailkey;
  queue_tail = queue_tail->next;
  return oldheadkey;
}

static gchar *list_shift ()
{
  gchar *oldheadkey = queue_tail->next->key;
  List *oldhead = queue_tail->next;
  queue_tail->next = queue_tail->next->next;
  g_free ( oldhead );
  queue_count--;
  return oldheadkey;
}

/* adds key to tail */
static void list_add_entry ( gchar *key )
{
  List *newlist = g_malloc ( sizeof ( List ) );
  newlist->key = key;
  if ( queue_tail ) {
    newlist->next = queue_tail->next;
    queue_tail->next = newlist;
    queue_tail = newlist;
  } else {
    newlist->next = newlist;
    queue_tail = newlist;
  }
  queue_count++;
}

void a_mapcache_add ( GdkPixbuf *pixbuf, gint x, gint y, gint z, guint16 type, gint zoom, guint8 alpha, gdouble xshrinkfactor, gdouble yshrinkfactor, const gchar* name )
{
  guint nn = name ? g_str_hash ( name ) : 0;
  gchar *key = g_strdup_printf ( HASHKEY_FORMAT_STRING, x, y, z, type, zoom, nn, alpha, xshrinkfactor, yshrinkfactor );

  g_mutex_lock(mc_mutex);
  cache_add(key, pixbuf);

  // TODO: that should be done on preference change only...
  max_cache_size = a_preferences_get(VIKING_PREFERENCES_NAMESPACE "mapcache_size")->u * 1024 * 1024;

  if ( cache_size > max_cache_size ) {
    if ( queue_tail ) {
      gchar *oldkey = list_shift_add_entry ( key );
      cache_remove(oldkey);

      while ( cache_size > max_cache_size &&
             (queue_tail->next != queue_tail) ) { /* make sure there's more than one thing to delete */
        oldkey = list_shift ();
        cache_remove(oldkey);
      }
    }
    /* chop off 'start' etc */
  } else {
    list_add_entry ( key );
    /* business as usual */
  }
  g_mutex_unlock(mc_mutex);

  static int tmp = 0;
  if ( (++tmp == 100 )) { g_debug("DEBUG: cache count=%d size=%u list count=%d\n", g_hash_table_size(cache), cache_size, queue_count ); tmp=0; }
}

GdkPixbuf *a_mapcache_get ( gint x, gint y, gint z, guint16 type, gint zoom, guint8 alpha, gdouble xshrinkfactor, gdouble yshrinkfactor, const gchar* name )
{
  static char key[64];
  guint nn = name ? g_str_hash ( name ) : 0;
  g_snprintf ( key, sizeof(key), HASHKEY_FORMAT_STRING, x, y, z, type, zoom, nn, alpha, xshrinkfactor, yshrinkfactor );
  return g_hash_table_lookup ( cache, key );
}

/**
 * Appears this is only used when redownloading tiles (i.e. to invalidate old images)
 */
void a_mapcache_remove_all_shrinkfactors ( gint x, gint y, gint z, guint16 type, gint zoom )
{
  char key[64];
  List *loop = queue_tail;
  List *tmp;
  gint len;

  if ( queue_tail == NULL )
    return;

  g_snprintf ( key, sizeof(key), HASHKEY_FORMAT_STRING_NOSHRINK_NOR_ALPHA, x, y, z, type, zoom, 0 );
  len = strlen(key);

  g_mutex_lock(mc_mutex);
  /* TODO: check logic here */
  do {
    tmp = loop->next;
    if ( tmp ) {
    if ( strncmp(tmp->key, key, len) == 0 )
    {
      cache_remove(tmp->key);
      if ( tmp == loop ) /* we deleted the last thing in the queue! */
        loop = queue_tail = NULL;
      else {
        loop->next = tmp->next;
        if ( tmp == queue_tail )
          queue_tail = tmp->next;
      }
      g_free ( tmp );
      tmp = NULL;
      queue_count--;
    }
    else
      loop = tmp;
    } else
      loop = NULL;
  } while ( loop && (loop != queue_tail || tmp == NULL) );

  /* loop thru list, looking for the one, compare first whatever chars */
  cache_remove(key);
  g_mutex_unlock(mc_mutex);
}

void a_mapcache_flush ()
{
  List *loop = queue_tail;
  List *tmp;

  if ( queue_tail == NULL )
    return;

  g_mutex_lock(mc_mutex);
  do {
    tmp = loop->next;
    cache_remove(tmp->key);
    if ( tmp == queue_tail ) /* we deleted the last thing in the queue */
      loop = queue_tail = NULL;
    else
      loop->next = tmp->next;
    g_free ( tmp );
    tmp = NULL;
  } while ( loop );

  g_mutex_unlock(mc_mutex);
}

void a_mapcache_uninit ()
{
  g_hash_table_destroy ( cache );
  /* free list */
  cache = NULL;
  vik_mutex_free (mc_mutex);
}

// Size of mapcache in memory
gint a_mapcache_get_size ()
{
  return cache_size;
}

// Count of items in the mapcache
gint a_mapcache_get_count ()
{
  return g_hash_table_size ( cache );
}
