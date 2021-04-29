/* directfb.c
 * DirectFB graphics driver
 * (c) 2002 Sven Neumann <sven@directfb.org>
 *
 * This file is a part of the Links program, released under GPL.
 */

/* TODO:
 * - Store window size as driver params (?)
 * - Fix wrong colors on big-endian systems (fixed?)
 * - Make everything work correctly ;-)
 *
 * KNOWN PROBLEMS:
 * - If mouse drags don't work for you, update DirectFB
 *   (the upcoming 0.9.14 release fixes this).
 */


#include "cfg.h"

#ifdef GRDRV_DIRECTFB

#include "links.h"

#undef debug
#include <directfb.h>

#include "dfb_cur.h"


#define FOCUSED_OPACITY    0xFF
#define UNFOCUSED_OPACITY  0xC0

#define DIRECTFB_HASH_TABLE_SIZE  23
static struct graphics_device **directfb_hash_table[DIRECTFB_HASH_TABLE_SIZE];

typedef struct _DFBDeviceData DFBDeviceData;
struct _DFBDeviceData
{
  DFBWindowID       id;
  IDirectFBWindow  *window;
  IDirectFBSurface *surface;
  DFBRegion         flip_region;
  int               flip_pending;
};


extern struct graphics_driver directfb_driver;

static IDirectFB             *dfb         = NULL;
static IDirectFBDisplayLayer *layer       = NULL;
static IDirectFBSurface      *arrow       = NULL;
static IDirectFBEventBuffer  *events      = NULL;
static DFBSurfacePixelFormat  pixelformat = DSPF_UNKNOWN;
static struct timer *event_timer = NULL;


static void directfb_register_flip     (DFBDeviceData *data,
                                        int x, int y, int w, int h);
static void directfb_flip_surface      (void *pointer);
static void directfb_check_events      (void *pointer);
static void directfb_translate_key     (DFBWindowEvent *event,
                                        int *key, int *flag);
static void directfb_add_to_table      (struct graphics_device *dev);
static void directfb_remove_from_table (struct graphics_device *dev);
static struct graphics_device * directfb_lookup_in_table (DFBWindowID  id);


static unsigned char *
directfb_init_driver (unsigned char *param, unsigned char *display)
{
  DFBDisplayLayerConfig  config;
  DFBResult              ret;
  unsigned char          *error;
  unsigned char          *result;

  DirectFBInit (&g_argc, (char ***)(void *)&g_argv);
  if ((ret = DirectFBCreate (&dfb)) != DFB_OK) {
      error = (unsigned char *)DirectFBErrorString(ret);
      goto ret;
    }

  if ((ret = dfb->GetDisplayLayer (dfb, DLID_PRIMARY, &layer)) != DFB_OK) {
      error = (unsigned char *)DirectFBErrorString(ret);
      goto ret_dfb;
  }

  if ((ret = layer->GetConfiguration (layer, &config)) != DFB_OK) {
      error = (unsigned char *)DirectFBErrorString(ret);
      goto ret_layer;
  }

  pixelformat = config.pixelformat;

  directfb_driver.depth = (((DFB_BYTES_PER_PIXEL (pixelformat) & 0x7)) |
                           ((DFB_COLOR_BITS_PER_PIXEL  (pixelformat) & 0x1F) << 3));

  if (directfb_driver.depth == 4)
	directfb_driver.depth = 196;

  /* endian test */
  if (big_endian) {
     if ((directfb_driver.depth & 0x7) == 2)
	directfb_driver.depth |= 0x100;
     if ((directfb_driver.depth & 0x7) == 4)
	directfb_driver.depth |= 0x200;
  }

  if (!get_color_fn(directfb_driver.depth)) {
	error = cast_uchar "Unsupported color depth";
	goto ret_layer;
  }

  directfb_driver.x = config.width;
  directfb_driver.y = config.height;

  memset (directfb_hash_table, 0, sizeof (directfb_hash_table));

  if ((ret = dfb->CreateEventBuffer (dfb, &events)) != DFB_OK) {
      error = (unsigned char *)DirectFBErrorString(ret);
      goto ret_layer;
  }

  event_timer = install_timer (20, directfb_check_events, events);

  if (dfb->CreateSurface (dfb, directfb_get_arrow_desc(), &arrow) != DFB_OK)
    arrow = NULL;

  return NULL;

ret_layer:
  layer->Release(layer);
ret_dfb:
  dfb->Release(dfb);
ret:
  result = init_str();
  add_to_strn(&result, error);
  add_to_strn(&result, cast_uchar "\n");
  return result;
}

static struct graphics_device *
directfb_init_device (void)
{
  struct graphics_device *dev;
  DFBDeviceData          *data;
  IDirectFBWindow        *window;
  DFBWindowDescription    desc;

  desc.flags  = (DFBWindowDescriptionFlags)(DWDESC_WIDTH | DWDESC_HEIGHT | DWDESC_POSX | DWDESC_POSY);
  desc.width  = directfb_driver.x;
  desc.height = directfb_driver.y;
  desc.posx   = 0;
  desc.posy   = 0;

  retry:
  if (layer->CreateWindow (layer, &desc, &window) != DFB_OK) {
    if (out_of_memory(MF_GPI, NULL, 0))
      goto retry;
    return NULL;
  }

  dev = mem_alloc (sizeof (struct graphics_device));

  dev->size.x1 = 0;
  dev->size.y1 = 0;
  window->GetSize (window, &dev->size.x2, &dev->size.y2);

  dev->clip = dev->size;

  data = mem_alloc (sizeof (DFBDeviceData));

  data->window       = window;
  data->flip_pending = 0;

  if (arrow)
    window->SetCursorShape (window, arrow, arrow_hot_x, arrow_hot_y);

  window->GetSurface (window, &data->surface);
  window->GetID (window, &data->id);

  dev->driver_data = data;
  dev->user_data   = NULL;

  directfb_add_to_table (dev);

  window->AttachEventBuffer (window, events);

  window->SetOpacity (window, FOCUSED_OPACITY);

  return dev;
}

static void
directfb_shutdown_device (struct graphics_device *dev)
{
  DFBDeviceData *data;

  if (!dev)
    return;

  data = dev->driver_data;

  unregister_bottom_half (directfb_flip_surface, data);
  directfb_remove_from_table (dev);

  data->surface->Release (data->surface);
  data->window->Destroy (data->window);
  data->window->Release (data->window);

  mem_free (data);
  mem_free (dev);
}

static void
directfb_shutdown_driver (void)
{
  int i;

  kill_timer (event_timer);
  events->Release (events);
  events = NULL;

  if (arrow)
    arrow->Release (arrow);

  layer->Release (layer);
  dfb->Release (dfb);

  for (i = 0; i < DIRECTFB_HASH_TABLE_SIZE; i++)
    if (directfb_hash_table[i])
      mem_free (directfb_hash_table[i]);

  dfb = NULL;
}

static int
directfb_get_empty_bitmap (struct bitmap *bmp)
{
  IDirectFBSurface      *surface;
  DFBSurfaceDescription  desc;
  int skip;

  bmp->data = bmp->flags = NULL;

  desc.flags = (DFBSurfaceDescriptionFlags)(DSDESC_WIDTH | DSDESC_HEIGHT);
  desc.width  = bmp->x;
  desc.height = bmp->y;

  retry:
  if (dfb->CreateSurface (dfb, &desc, &surface) != DFB_OK) {
    if (out_of_memory(MF_GPI, NULL, 0))
	goto retry;
    return -1;
  }

  surface->Lock (surface, (DFBSurfaceLockFlags)(DSLF_READ | DSLF_WRITE), &bmp->data, &skip);

  bmp->skip = skip;
  bmp->flags = surface;

  return 0;
}

static void
directfb_register_bitmap (struct bitmap *bmp)
{
  IDirectFBSurface *surface = bmp->flags;
  if (!surface) return;

  surface->Unlock (surface);
  bmp->data = NULL;
}

static void *
directfb_prepare_strip (struct bitmap *bmp, int top, int lines)
{
  IDirectFBSurface *surface = bmp->flags;
  int skip;
  if (!surface) return NULL;

  surface->Lock (surface, (DFBSurfaceLockFlags)(DSLF_READ | DSLF_WRITE), &bmp->data, &skip);

  return ((unsigned char *) bmp->data + top * bmp->skip);
}

static void
directfb_commit_strip (struct bitmap *bmp, int top, int lines)
{
  IDirectFBSurface *surface = bmp->flags;
  if (!surface) return;

  surface->Unlock (surface);
  bmp->data = NULL;
}

static void
directfb_unregister_bitmap (struct bitmap *bmp)
{
  IDirectFBSurface *surface = bmp->flags;
  if (!surface) return;

  surface->Release (surface);
}

static void
directfb_draw_bitmap (struct graphics_device *dev, struct bitmap *bmp,
                      int x, int y)
{
  DFBDeviceData    *data = dev->driver_data;
  IDirectFBSurface *src  = bmp->flags;
  if (!src) return;

  CLIP_DRAW_BITMAP

  data->surface->Blit (data->surface, src, NULL, x, y);

  directfb_register_flip (data, x, y, bmp->x, bmp->y);
}

static long
directfb_get_color (int rgb)
{
  return rgb;
}


static inline void directfb_set_color (IDirectFBSurface *surface, long color)
{
  surface->SetColor (surface,
                     (color & 0xFF0000) >> 16,
                     (color & 0xFF00)   >> 8,
                     (color & 0xFF),
                     0xFF);
}

static void
directfb_fill_area (struct graphics_device *dev,
                    int x1, int y1, int x2, int y2, long color)
{
  DFBDeviceData *data = dev->driver_data;
  int w, h;

  CLIP_FILL_AREA

  w = x2 - x1;
  h = y2 - y1;

  directfb_set_color (data->surface, color);
  data->surface->FillRectangle (data->surface, x1, y1, w, h);

  directfb_register_flip (data, x1, y1, w, h);
}

static void
directfb_draw_hline (struct graphics_device *dev,
                     int x1, int y, int x2, long color)
{
  DFBDeviceData *data = dev->driver_data;

  CLIP_DRAW_HLINE

  x2--;

  directfb_set_color (data->surface, color);
  data->surface->DrawLine (data->surface, x1, y, x2, y);

  directfb_register_flip (data, x1, y, x2 - x1, 1);
}

static void
directfb_draw_vline (struct graphics_device *dev,
                     int x, int y1, int y2, long color)
{
  DFBDeviceData *data = dev->driver_data;

  CLIP_DRAW_VLINE

  y2--;

  directfb_set_color (data->surface, color);
  data->surface->DrawLine (data->surface, x, y1, x, y2);

  directfb_register_flip (data, x, y1, 1, y2 - y1);
}

static void
directfb_set_clip_area (struct graphics_device *dev)
{
  DFBDeviceData *data   = dev->driver_data;
  DFBRegion      region;

  region.x1 = dev->clip.x1;
  region.y1 = dev->clip.y1;
  region.x2 = dev->clip.x2 - 1;
  region.y2 = dev->clip.y2 - 1;

  data->surface->SetClip (data->surface, &region);
}

static int
directfb_scroll (struct graphics_device *dev, struct rect_set **set, int scx, int scy)
{
  DFBDeviceData *data = dev->driver_data;
  DFBRectangle   rect;

  rect.x = dev->clip.x1;
  rect.y = dev->clip.y1;
  rect.w = dev->clip.x2 - rect.x;
  rect.h = dev->clip.y2 - rect.y;

  data->surface->Blit (data->surface,
                       data->surface, &rect, rect.x + scx, rect.y + scy);

  directfb_register_flip (data, rect.x, rect.y, rect.w, rect.h);

  return 1;
}

static void directfb_register_flip (DFBDeviceData *data,
                                    int x, int y, int w, int h)
{
  if (x < 0 || y < 0 || w < 1 || h < 1)
    return;

  w = x + w - 1;
  h = y + h - 1;

  if (data->flip_pending)
    {
      if (data->flip_region.x1 > x)  data->flip_region.x1 = x;
      if (data->flip_region.y1 > y)  data->flip_region.y1 = y;
      if (data->flip_region.x2 < w)  data->flip_region.x2 = w;
      if (data->flip_region.y2 < h)  data->flip_region.y2 = h;
    }
  else
    {
      data->flip_region.x1 = x;
      data->flip_region.y1 = y;
      data->flip_region.x2 = w;
      data->flip_region.y2 = h;

      data->flip_pending = 1;

      register_bottom_half (directfb_flip_surface, data);
    }
}

static void
directfb_flip_surface (void *pointer)
{
  DFBDeviceData *data = pointer;

  if (!data->flip_pending)
    return;

  data->surface->Flip (data->surface, &data->flip_region, (DFBSurfaceFlipFlags)0);

  data->flip_pending = 0;
}

static void
directfb_flush(struct graphics_device *dev)
{
  DFBDeviceData *data = dev->driver_data;
  unregister_bottom_half (directfb_flip_surface, data);
  directfb_flip_surface(data);
}

static void
directfb_check_events (void *pointer)
{
  struct graphics_device *dev   = NULL;
  DFBDeviceData          *data = NULL;
  DFBWindowEvent          event;
  DFBWindowEvent          next;

  while (events->GetEvent (events, DFB_EVENT (&event)) == DFB_OK)
    {
      switch (event.type)
        {
        case DWET_GOTFOCUS:
        case DWET_LOSTFOCUS:
        case DWET_POSITION_SIZE:
        case DWET_SIZE:
        case DWET_KEYDOWN:
        case DWET_BUTTONDOWN:
        case DWET_BUTTONUP:
        case DWET_WHEEL:
        case DWET_MOTION:
          break;
        default:
          continue;
        }

      if (!dev || data->id != event.window_id)
        {
          dev = directfb_lookup_in_table (event.window_id);
          if (!dev)
            continue;
        }

      data = dev->driver_data;

      switch (event.type)
        {
#if 0
        case DWET_GOTFOCUS:
          data->window->SetOpacity (data->window, FOCUSED_OPACITY);
          break;

        case DWET_LOSTFOCUS:
          data->window->SetOpacity (data->window, UNFOCUSED_OPACITY);
          break;
#endif

        case DWET_POSITION_SIZE:
        case DWET_SIZE:
          while ((events->PeekEvent (events, DFB_EVENT (&next)) == DFB_OK)   &&
                 (next.type == DWET_SIZE || next.type == DWET_POSITION_SIZE) &&
                 (next.window_id == data->id))
            events->GetEvent (events, DFB_EVENT (&event));

          dev->size.x2 = event.w;
          dev->size.y2 = event.h;
          dev->resize_handler (dev);
          break;

        case DWET_KEYDOWN:
          {
            int key, flag;

            directfb_translate_key (&event, &key, &flag);
            if (key)
              dev->keyboard_handler (dev, key, flag);
          }
          break;

        case DWET_BUTTONDOWN:
        case DWET_BUTTONUP:
          {
            int flags;

	     /*
	      * For unknown reason, we get the event twice
	      */
             while ((events->PeekEvent (events, DFB_EVENT (&next)) == DFB_OK)   &&
                    (next.type == event.type && next.button == event.button &&
		     next.x == event.x && next.y == event.y && next.window_id == data->id))
               events->GetEvent (events, DFB_EVENT (&event));

            if (event.type == DWET_BUTTONUP)
              {
                flags = B_UP;
                data->window->UngrabPointer (data->window);
              }
            else
              {
                flags = B_DOWN;
                data->window->GrabPointer (data->window);
              }

            switch (event.button)
              {
              case DIBI_LEFT:
                flags |= B_LEFT;
                break;
              case DIBI_RIGHT:
                flags |= B_RIGHT;
                break;
              case DIBI_MIDDLE:
                flags |= B_MIDDLE;
                break;
              default:
                continue;
              }

            dev->mouse_handler (dev, event.x, event.y, flags);
          }
          break;

        case DWET_WHEEL:
          dev->mouse_handler (dev, event.x, event.y,
                             B_MOVE |
                             (event.step > 0 ? B_WHEELUP : B_WHEELDOWN));
	  break;

        case DWET_MOTION:
          {
            int flags;

            while ((events->PeekEvent (events, DFB_EVENT (&next)) == DFB_OK) &&
                   (next.type      == DWET_MOTION)                           &&
                   (next.window_id == data->id))
              events->GetEvent (events, DFB_EVENT (&event));

            switch (event.buttons)
              {
              case DIBM_LEFT:
                flags = B_DRAG | B_LEFT;
                break;
              case DIBM_RIGHT:
                flags = B_DRAG | B_RIGHT;
                break;
              case DIBM_MIDDLE:
                flags = B_DRAG | B_MIDDLE;
                break;
              default:
                flags = B_MOVE;
                break;
              }

            dev->mouse_handler (dev, event.x, event.y, flags);
          }
          break;

        case DWET_CLOSE:
          dev->keyboard_handler (dev, KBD_CLOSE, 0);
          break;

        default:
          break;
        }
    }

  event_timer = install_timer (20, directfb_check_events, events);
}

static void
directfb_translate_key (DFBWindowEvent *event, int *key, int *flag)
{
  *key  = 0;
  *flag = 0;

  if (event->modifiers & DIMM_CONTROL && event->key_id == DIKI_C)
    {
      *key = KBD_CTRL_C;
      return;
    }

  /* setting Shift seems to break things
   *
   *  if (event->modifiers & DIMM_SHIFT)
   *     *flag |= KBD_SHIFT;
   */
  if (event->modifiers & DIMM_CONTROL)
    *flag |= KBD_CTRL;
  if (event->modifiers & DIMM_ALT)
    *flag |= KBD_ALT;

  switch (event->key_symbol)
    {
    case DIKS_ENTER:        *key = KBD_ENTER;     break;
    case DIKS_BACKSPACE:    *key = KBD_BS;        break;
    case DIKS_TAB:          *key = KBD_TAB;       break;
    case DIKS_ESCAPE:       *key = KBD_ESC;       break;
    case DIKS_CURSOR_UP:    *key = KBD_UP;        break;
    case DIKS_CURSOR_DOWN:  *key = KBD_DOWN;      break;
    case DIKS_CURSOR_LEFT:  *key = KBD_LEFT;      break;
    case DIKS_CURSOR_RIGHT: *key = KBD_RIGHT;     break;
    case DIKS_INSERT:       *key = KBD_INS;       break;
    case DIKS_DELETE:       *key = KBD_DEL;       break;
    case DIKS_HOME:         *key = KBD_HOME;      break;
    case DIKS_END:          *key = KBD_END;       break;
    case DIKS_PAGE_UP:      *key = KBD_PAGE_UP;   break;
    case DIKS_PAGE_DOWN:    *key = KBD_PAGE_DOWN; break;
    case DIKS_F1:           *key = KBD_F1;        break;
    case DIKS_F2:           *key = KBD_F2;        break;
    case DIKS_F3:           *key = KBD_F3;        break;
    case DIKS_F4:           *key = KBD_F4;        break;
    case DIKS_F5:           *key = KBD_F5;        break;
    case DIKS_F6:           *key = KBD_F6;        break;
    case DIKS_F7:           *key = KBD_F7;        break;
    case DIKS_F8:           *key = KBD_F8;        break;
    case DIKS_F9:           *key = KBD_F9;        break;
    case DIKS_F10:          *key = KBD_F10;       break;
    case DIKS_F11:          *key = KBD_F11;       break;
    case DIKS_F12:          *key = KBD_F12;       break;

    default:
      if (DFB_KEY_TYPE (event->key_symbol) == DIKT_UNICODE)
        *key = event->key_symbol;
      break;
    }
}

static void
directfb_add_to_table (struct graphics_device *dev)
{
  DFBDeviceData           *data = dev->driver_data;
  struct graphics_device **devices;
  int i;

  i = data->id % DIRECTFB_HASH_TABLE_SIZE;

  devices = directfb_hash_table[i];

  if (devices)
    {
      int c = 0;

      while (devices[c++])
        if (c == MAXINT) overalloc();

      if ((unsigned)c > MAXINT / sizeof(void *) - 1) overalloc();
      devices = mem_realloc (devices, (c + 1) * sizeof (void *));
      devices[c-1] = dev;
      devices[c]   = NULL;
    }
  else
    {
      devices = mem_alloc (2 * sizeof (void *));
      devices[0] = dev;
      devices[1] = NULL;
    }

  directfb_hash_table[i] = devices;
}

static void
directfb_remove_from_table (struct graphics_device *dev)
{
  DFBDeviceData           *data = dev->driver_data;
  struct graphics_device **devices;
  int i, j, c;

  i = data->id % DIRECTFB_HASH_TABLE_SIZE;

  devices = directfb_hash_table[i];
  if (!devices)
    return;

  for (j = 0, c = -1; devices[j]; j++)
    if (devices[j] == dev)
      c = j;

  if (c < 0)
    return;

  memmove (devices + c, devices + c + 1, (j - c) * sizeof (void *));
  devices = mem_realloc (devices, j * sizeof (void *));

  directfb_hash_table[i] = devices;
}

static struct graphics_device *
directfb_lookup_in_table (DFBWindowID id)
{
  struct graphics_device **devices;
  int i;

  i = id % DIRECTFB_HASH_TABLE_SIZE;

  devices = directfb_hash_table[i];
  if (!devices)
    return NULL;

  while (*devices)
    {
      DFBDeviceData *data = (*devices)->driver_data;

      if (data->id == id)
        return *devices;

      devices++;
    }

  return NULL;
}

struct graphics_driver directfb_driver =
{
  cast_uchar "directfb",
  directfb_init_driver,
  directfb_init_device,
  directfb_shutdown_device,
  directfb_shutdown_driver,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  directfb_get_empty_bitmap,
  directfb_register_bitmap,
  directfb_prepare_strip,
  directfb_commit_strip,
  directfb_unregister_bitmap,
  directfb_draw_bitmap,
  directfb_get_color,
  directfb_fill_area,
  directfb_draw_hline,
  directfb_draw_vline,
  directfb_scroll,
  directfb_set_clip_area,
  directfb_flush,
  NULL,	 /*  block */
  NULL,	 /*  unblock */
  NULL,	 /*  set_palette */
  NULL,	 /*  get_real_colors */
  NULL,	 /*  set_title  */
  NULL,	 /*  exec	*/
  NULL,	 /*  set_clipboard_text */
  NULL,	 /*  get_clipboard_text */
  0,	 /*  depth      */
  0, 0,	 /*  size       */
  GD_UNICODE_KEYS | GD_NO_OS_SHELL | GD_NOAUTO, /*  flags      */
  NULL,	 /*  param	*/
};

#endif /* GRDRV_DIRECTFB */
