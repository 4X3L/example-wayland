/* main.c
 * 
 * Copyright (c) 2018 Alex Benishek
 * 
 * Permission is hereby granted, free of charge, to any person obtaining 
 * a copy of this software and associated documentation files (the 
 * "Software"), to deal in the Software without restriction, including 
 * without limitation the rights to use, copy, modify, merge, publish, 
 * distribute, sublicense, and/or sell copies of the Software, and to 
 * permit persons to whom the Software is furnished to do so, subject to 
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the 
 * next paragraph) shall be included in all copies or substantial 
 * portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS 
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN 
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN 
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
 * SOFTWARE. 
 */
/* Sources:
   https://github.com/hdante/hello_wayland/blob/master/helpers.c
   https://github.com/hdante/hello_wayland/blob/master/hello_wayland.c
   https://wayland.freedesktop.org/docs/html/
   https://jan.newmarch.name/Wayland/index.html
   https://cgit.freedesktop.org/wayland/wayland/tree/src/wayland-client.c
   https://cgit.freedesktop.org/wayland/wayland/tree/protocol/wayland.xml
   https://github.com/wayland-project/weston/blob/master/clients/window.c
   https://github.com/eyelash/tutorials/blob/master/wayland-egl.c
   https://github.com/eyelash/tutorials/blob/master/wayland-input.c
   https://gist.github.com/Miouyouyou/ca15af1c7f2696f66b0e013058f110b4
*/

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-unstable-v6-client-protocol.h"
#include "int_set.h"

#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

/* We will be checking if things failed a lot. */
#define CHECK_COND(cond,label,fail,success) if (cond)                 \
    {                                                                 \
      if (errno != 0)                                                 \
        perror (fail);                                                \
      else                                                            \
        fprintf (stderr, fail);                                       \
      status = EXIT_FAILURE;                                          \
      goto label;                                                     \
    } else printf (success);                                    

#define CHECK_NULL(ptr,label,fail,success) if (ptr == NULL)             \
    {                                                                   \
  if (errno != 0)                                                       \
    perror (fail);                                                      \
  else                                                                  \
    fprintf (stderr, fail);                                             \
  status = EXIT_FAILURE;                                                \
  goto label;                                                           \
    }                                                                   \
  else printf (success);

// STRUCTS

/* Wayland protocol specifies that all servers support ARGB (8-bit x4) layout so we 
   will use that. */
/* Little endian, so we layout the pixels in the reverse order than you'd expect */
struct pixel
{
  uint8_t b;
  uint8_t g;
  uint8_t r;
  uint8_t a;
};

struct output_info
{
  int32_t width;
  int32_t height;
  int32_t hertz;
};

struct ua_keyboard 
{
  struct wl_keyboard *kbd;
  struct int_set pressed;
  uint32_t buf_size;
  int keymap_fd;
  struct xkb_context *ctx;
  struct xkb_keymap *map;
  struct xkb_state *kb_state;
};

struct input_bundle
{
  struct ua_keyboard keyboard;
  struct wl_pointer *mouse;
  struct wl_seat *seat;
};

struct ua_wayland_data 
{
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_surface *surface;
  struct zxdg_surface_v6 *shell_surface;
  struct zxdg_shell_v6 *shell;
  struct zxdg_toplevel_v6 *top;
  struct wl_shm *shm;
  /* List of outputs */
  struct wl_list *monitors;
  struct input_bundle inputs;
};

struct ua_wayland_output
{
  struct wl_output *out;
  struct output_info info;
  struct wl_list link;
};

struct mem 
{
  void *raw;
  size_t raw_size;
  int fd;
  struct wl_shm_pool *pool;
};

struct ua_buffer
{
  uint32_t width;
  uint32_t height;
  struct ua_double_buffer_list *bufs;
};

struct ua_double_buffer_list
{
  struct wl_buffer *wl_buffer;
  struct pixel *pixels;
  struct ua_double_buffer_list *next;
  bool used;
};

// LISTENERS

static void
do_nothing () {}


static void
output_mode_cb (void *data, struct wl_output *out, uint32_t flags,
                int32_t width, int32_t height, int32_t refresh_rate)
{
  struct output_info *info = data;
  info->width = width;
  info->height = height;
  info->hertz = refresh_rate;
}

static const struct wl_output_listener output_listener = 
  {
    do_nothing,
    output_mode_cb,
    do_nothing,
    do_nothing
  };

static void
motion_pointer_cb (void *data, struct wl_pointer *pointer, uint32_t time,
                   wl_fixed_t surface_x, wl_fixed_t surface_y) {

  printf ("Motion: %u, %f, %f.\n", time, wl_fixed_to_double(surface_x),
          wl_fixed_to_double(surface_y));
  
}

static void
button_pointer_cb (void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
  if (state == WL_POINTER_BUTTON_STATE_PRESSED)
    printf ("Pressed %u.\n", button);
  else
    printf ("Released %u.\n", button);
}

static const struct wl_pointer_listener pointer_listener =
  { do_nothing,
    do_nothing,
    motion_pointer_cb,
    button_pointer_cb,
    do_nothing,
    do_nothing,
    do_nothing,
    do_nothing,
    do_nothing };

static void
keymap_format_cb (void *data, struct wl_keyboard *keyboard, uint32_t format,
                  int32_t fd, uint32_t keymap_size ) 
{
  struct ua_keyboard *kbd = data;
  char *str = mmap (NULL, keymap_size, PROT_READ, MAP_SHARED, fd, 0);
  xkb_keymap_unref (kbd->map);
  kbd->map = xkb_keymap_new_from_string (kbd->ctx, str, XKB_KEYMAP_FORMAT_TEXT_V1,
                                         XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap (str, keymap_size);
  close (fd);
  xkb_state_unref (kbd->kb_state);
  kbd->kb_state = xkb_state_new (kbd->map);
  
}

static void
key_cb (void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
        uint32_t key, uint32_t state)
{
  (void) keyboard;
  (void) serial;
  (void) time;
  struct ua_keyboard *keydata = data;
  printf ("Key pressed: %u\n", key);
  xkb_keysym_t keysym =
    xkb_state_key_get_one_sym (keydata->kb_state, key+8);  
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    int_set_add (&keydata->pressed, keysym);
  else
    int_set_remove (&keydata->pressed, keysym);
  char keyname[64];
  xkb_keysym_get_name (keysym, keyname, 64);
  printf ("Pressed key, \"%s\"\n", keyname);
}

static const struct wl_keyboard_listener keyboard_listener = 
  {
    keymap_format_cb,
    do_nothing,
    do_nothing,
    key_cb,
    do_nothing,
    do_nothing,
  };

static void
calc_capabilities (void *data, struct wl_seat *s, uint32_t cap)
{
  struct input_bundle *inputs = data;
  (void) s;
  //Cap is a bitfield. The WL_SEAT_CAPABILITY_XXX enum is a mask
  // that selects the corresponding bit.
  inputs->seat = s;
  if (cap & WL_SEAT_CAPABILITY_KEYBOARD)
    {
      printf ("Has a keyboard.\n");
      inputs->keyboard.kbd = wl_seat_get_keyboard(s);
      wl_keyboard_add_listener (inputs->keyboard.kbd, &keyboard_listener, &inputs->keyboard);
    }

  if (cap & WL_SEAT_CAPABILITY_POINTER)
    {
      printf ("Has a pointer.\n");
      inputs->mouse = wl_seat_get_pointer(s);
      wl_pointer_add_listener(inputs->mouse, &pointer_listener, NULL);
    }
  
  if (cap & WL_SEAT_CAPABILITY_TOUCH)
      printf ("Has a touchscreen.\n");
}

static const struct wl_seat_listener seat_listener = 
  {
    calc_capabilities,
    do_nothing,
  };

static void
shell_ping_respond (void *data, struct zxdg_shell_v6 *shell, uint32_t serial)
{
  (void) data;
  zxdg_shell_v6_pong (shell, serial);
}

static const struct zxdg_shell_v6_listener xdg_shell_listener =
  {
    shell_ping_respond
  };

/* Whenever something is added to the registry our program will be notified by 
   wayland running this callback */
static void
global_registry_handler (void *data,
                         struct wl_registry *registry,
                         uint32_t id,
                         const char *interface,
                         uint32_t version) 
{
  struct ua_wayland_data *objs = data;
  printf("Got a registry event for %s id %d version %d \n", interface, id, version);
  if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
      objs->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
      objs->surface = wl_compositor_create_surface (objs->compositor);
    }
  else if (strcmp(interface, zxdg_shell_v6_interface.name) == 0)
    {
      objs->shell = wl_registry_bind (registry, id, &zxdg_shell_v6_interface, 1);
      zxdg_shell_v6_add_listener(objs->shell, &xdg_shell_listener, NULL);
    }
  else if (strcmp(interface, wl_shm_interface.name) == 0)
    objs->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
  else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
      objs->inputs.seat = wl_registry_bind(registry, id, &wl_seat_interface, 5);
      wl_seat_add_listener(objs->inputs.seat, &seat_listener, &objs->inputs);
    }
  else if (strcmp(interface, wl_output_interface.name) == 0)
    {
      struct wl_output *wl_out = wl_registry_bind (registry, id, &wl_output_interface, 2);
      // TODO Free
      struct ua_wayland_output *ua_out = malloc (sizeof (struct ua_wayland_output));
      ua_out->out = wl_out;
      wl_output_add_listener (ua_out->out, &output_listener, &ua_out->info);
      wl_list_insert (objs->monitors, &ua_out->link);
    }
  
}

/* The registry expects a pair of callbacks, one for when an object is created,
   the other for when it is destroyed. The pair is wrapped in a struct. */
static const struct wl_registry_listener registry_listener = 
  {
    global_registry_handler,
    do_nothing,
  };

static void
surface_configure_cb (void *data, struct zxdg_surface_v6 *surface, uint32_t serial)
{
  (void) data;
  zxdg_surface_v6_ack_configure (surface, serial);
}

static const struct zxdg_surface_v6_listener xdg_surface_listener = 
  {
    surface_configure_cb,
  };

static void
toplevel_close_cb (void *data, struct zxdg_toplevel_v6 *top)
{
  (void) top;
  
  bool *b = data;
  *b = true;
}

static const struct zxdg_toplevel_v6_listener xdg_top_listener =
  {
    do_nothing,
    toplevel_close_cb
  };

static void
buffer_release_cb (void *data, struct wl_buffer *buf)
{
  (void) buf;
  bool *b = data;
  *b = false;
}

static const struct wl_buffer_listener buffer_listener = 
  {
    buffer_release_cb
  };

static void
callback_set_ready (void *data, struct wl_callback *cb, uint32_t cb_data)
{
  (void) cb;
  (void) cb_data;
  bool *ready = data;
  *ready = true;
}

static const struct wl_callback_listener frame_limiter_listener = 
  {
    callback_set_ready
  };


// FUNCTIONS

void
time_elapsed (struct timespec *c, const struct timespec *a, const struct timespec *b) 
{
  c->tv_sec = a->tv_sec - b->tv_sec;
  if (b->tv_nsec > a->tv_nsec)
    {
      c->tv_sec--;
      c->tv_nsec = 1000000000;
      c->tv_nsec += a->tv_nsec;
      c->tv_nsec -= b->tv_nsec;
    }
  else
    {
      c->tv_nsec = a->tv_nsec - b->tv_nsec;
    }  
}

bool
time_lessthan (const struct timespec *a, const struct timespec *b) 
{
  return a->tv_sec == b->tv_sec ?
    a->tv_nsec < b->tv_nsec :
    a->tv_sec < b->tv_sec;
}

uint64_t
nanoseconds (const struct timespec *t) 
{
  uint64_t ns = t->tv_sec * 1000000000;
  ns += t->tv_nsec;
  return ns;
}

void
time_set (struct timespec *t, uint64_t seconds, uint64_t nanosec)
{
  t->tv_sec = seconds;
  t->tv_nsec = nanosec;
}

static void
render (void *buf, uint32_t width, uint32_t height)
{
  struct pixel (*pixel)[height][width] = buf;
  float x_factor = 1.0 / (float) height;
  float y_factor = 1.0 / (float) width;

  for (uint32_t y = 0; y < width; y++)
    {
      for (uint32_t x = 0; x < height; x++) 
        {
          float nx = x * x_factor;
          float ny = y * y_factor;
          float dx = 0.5 - nx;
          float dy = 0.5 - ny;
          float d = sqrt (dx * dx + dy * dy);
          (*pixel)[x][y].r = nx * 255;
          (*pixel)[x][y].g = ny * 255;
          (*pixel)[x][y].b = d * 255;
          (*pixel)[x][y].a = 255;
        }
    }
}

// INITIALIZATION

enum MEM_ERR { MEM_ERR_OK
             , MEM_ERR_FD
             , MEM_ERR_TRUNC
             , MEM_ERR_MMAP
             , MEM_ERR_POOL };

static enum MEM_ERR
init_mem(struct wl_shm *shm, struct mem *m, size_t fb_size) 
{
  memset (m, 0, sizeof (struct mem));
  
  m->raw_size = fb_size << 1;

  m->fd = shm_open ("/unknown_animal_wayland_frame_buffer",
                         O_RDWR | O_CREAT, S_IRUSR | S_IWUSR );
  if (m->fd < 0) 
    {
      perror ("Could not open shared memory file.\n");
      return MEM_ERR_FD;
    }
  else
    printf ("Created shared memory file.\n");
  
  if (ftruncate (m->fd, m->raw_size) != 0)
    {
      perror ("Could not reized shared memory file.\n");
      return MEM_ERR_TRUNC;
    }
  else
    printf ("Resized chared memory file descriptor.\n");
        
  m->raw = mmap (NULL, m->raw_size, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd,  0);

  if (m->raw == MAP_FAILED)
    {
      perror ("Could not map file to memory.\n");
      return MEM_ERR_MMAP;
    }
  else
    printf ("Mapped file to memory.\n");

  for (uint8_t *ptr = m->raw; ptr != ((uint8_t*) m->raw) + m->raw_size; ptr++)
    *ptr = 0xFF;
  
  memset (m->raw, ~0, m->raw_size);
  
  m->pool = wl_shm_create_pool(shm, m->fd, m->raw_size);

  if (m->pool == NULL) 
    {
      fprintf (stderr, "Could not create pool.\n");
      return MEM_ERR_POOL;
    }
  else
    printf ("Created shared memory pool\n");

  return MEM_ERR_OK;
}

enum WAYLAND_SETUP_ERR
  {
    SETUP_OK,
    NO_WAY_DISP,
    NO_REG,
    NO_COMP,
    NO_SHELL,
    NO_SEAT,
    NO_SHM,
    NO_MONITORS,
    NO_SURFACE,
    NO_SHELL_SURFACE,
    NO_TOPLEVEL
  };

static enum WAYLAND_SETUP_ERR
setup_wayland (struct ua_wayland_data *objs)
{
  memset (objs, 0, sizeof (struct ua_wayland_data));
  //TODO Free, safe malloc
  objs->monitors = malloc (sizeof (struct wl_list));
  wl_list_init (objs->monitors);
  
  
  memset (&objs->inputs, 0, sizeof (struct input_bundle));
  int_set_init (&objs->inputs.keyboard.pressed);
  objs->inputs.keyboard.ctx = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  
  objs->display = wl_display_connect (NULL);
  if (objs->display == NULL)
    return NO_WAY_DISP;
  
  objs->registry = wl_display_get_registry (objs->display);
  if (objs->registry == NULL)
    return NO_REG;
  wl_registry_add_listener (objs->registry, &registry_listener, objs);
  wl_display_roundtrip (objs->display); // Wait for registry listener to run
  if (objs->compositor == NULL)
    return NO_COMP;
  if (objs->shell == NULL)
    return NO_SHELL;
  if (objs->inputs.seat == NULL)
    return NO_SEAT;
  if (objs->shm == NULL)
    return NO_SHM;
  if (wl_list_empty (objs->monitors))
    return NO_MONITORS;
  if (objs->surface == NULL)
    return NO_SURFACE;

  objs->shell_surface = zxdg_shell_v6_get_xdg_surface (objs->shell, objs->surface);
  if (objs->shell_surface == NULL)
    return NO_SHELL_SURFACE;
  
  zxdg_surface_v6_add_listener (objs->shell_surface, &xdg_surface_listener, NULL);

  objs->top = zxdg_surface_v6_get_toplevel (objs->shell_surface);
  if (objs->top == NULL)
    return NO_TOPLEVEL;

  zxdg_toplevel_v6_set_maximized (objs->top);
  zxdg_toplevel_v6_set_title (objs->top, "Unknown Animal");
  wl_display_roundtrip (objs->display);

  return SETUP_OK;
}

static void
destroy_wayland_data (struct ua_wayland_data *objs)
{
  struct ua_wayland_output *out = NULL;
  struct wl_list *head = objs->monitors;
  struct wl_list *current = head ->next;
  
  while(current != head)
    {
      struct wl_list *next = current->next;
      out = container_of (current, struct ua_wayland_output, link);
      wl_output_destroy (out->out);
      free (out);
      current = next;
    }
  free (head);
  xkb_keymap_unref (objs->inputs.keyboard.map);
  xkb_state_unref (objs->inputs.keyboard.kb_state);
  xkb_context_unref (objs->inputs.keyboard.ctx);
  wl_keyboard_destroy (objs->inputs.keyboard.kbd);

  free (objs->inputs.keyboard.pressed.data);
  free (objs->inputs.keyboard.pressed.valid);
  
  wl_pointer_destroy (objs->inputs.mouse);
  wl_seat_destroy (objs->inputs.seat);
  wl_shm_destroy (objs->shm);
  zxdg_toplevel_v6_destroy (objs->top);
  zxdg_surface_v6_destroy (objs->shell_surface);
  zxdg_shell_v6_destroy (objs->shell);
  wl_surface_destroy (objs->surface);
  wl_compositor_destroy (objs->compositor);
  wl_registry_destroy (objs->registry);
  wl_display_disconnect (objs->display);
}

enum BUFFER_ERROR
  {
    BUFFER_OK,
    BUFFER_NO_WL_BUF
  };

static enum BUFFER_ERROR
init_buffers (struct ua_buffer *buf, const struct mem *mem, uint32_t width,
              uint32_t height)
{
  buf->width = width;
  buf->height = height;

  buf->bufs = malloc (sizeof (struct ua_double_buffer_list));
  buf->bufs->wl_buffer =
    wl_shm_pool_create_buffer (mem->pool, 0, width, height,
                               width * sizeof (struct pixel),
                               WL_SHM_FORMAT_ARGB8888);
  if (buf->bufs->wl_buffer == NULL)
    return BUFFER_NO_WL_BUF;
  
  buf->bufs->pixels = mem->raw;
  buf->bufs->used = false;
  wl_buffer_add_listener (buf->bufs->wl_buffer, &buffer_listener, &buf->bufs->used);

  struct ua_double_buffer_list *back_buf = malloc (sizeof (struct ua_double_buffer_list));
  back_buf->wl_buffer =
    wl_shm_pool_create_buffer(mem->pool, sizeof (struct pixel) * width * height,
                              width, height, width * sizeof (struct pixel),
                              WL_SHM_FORMAT_ARGB8888);
  if (back_buf == NULL)
    return BUFFER_NO_WL_BUF;
  
  back_buf->pixels = ((struct pixel*) mem->raw) + width * height;
  back_buf->used = false;
  wl_buffer_add_listener (back_buf->wl_buffer, &buffer_listener, &back_buf->used);

  buf->bufs->next = back_buf;
  back_buf->next = buf->bufs;

  return BUFFER_OK;
}

static void
free_buffers (struct ua_buffer *buf)
{
  if (buf == NULL || buf->bufs == NULL)
    return;

  if (buf->bufs->next != NULL)
    {
      wl_buffer_destroy (buf->bufs->next->wl_buffer);
      free (buf->bufs->next);
    }
  wl_buffer_destroy (buf->bufs->wl_buffer);
  free (buf->bufs);
}

static void
free_mem (struct mem *mem)
{
  wl_shm_pool_destroy (mem->pool);
  munmap (mem->raw, mem->raw_size);
}

int
main () 
{
  struct ua_wayland_data objs;
  struct mem mem;
  struct ua_buffer buf;
  struct ua_double_buffer_list *current;

  if (setup_wayland (&objs) != SETUP_OK)
    {
      destroy_wayland_data (&objs);
      return EXIT_FAILURE;
    }
  
  struct ua_wayland_output *out = container_of (objs.monitors->next,
                                                struct ua_wayland_output,
                                                link);
  const size_t width = out->info.width;
  const size_t height = out->info.height;
  const size_t num_px = width * height;
  const size_t fb_size = num_px * sizeof (struct pixel);
  printf ("(%lu, %lu)\n", width, height);

  bool close = false;

  zxdg_toplevel_v6_set_fullscreen (objs.top, out->out);
  zxdg_toplevel_v6_add_listener (objs.top, &xdg_top_listener, &close);
  
  enum MEM_ERR e = init_mem (objs.shm, &mem, fb_size);
  if (e != MEM_ERR_OK)
    {
      destroy_wayland_data (&objs);
      return EXIT_FAILURE;
    }

  wl_surface_commit(objs.surface);

  if (init_buffers (&buf, &mem, width, height) != BUFFER_OK)
    {
      free_buffers (&buf);
      destroy_wayland_data (&objs);
    }

  wl_display_roundtrip (objs.display);
  
  uint32_t fb_index = 0;

  struct wl_callback *ready_cb = NULL;
  bool ready = true;

  struct timespec last_frame;
  struct timespec current_frame;
  struct timespec frame_delta;
  struct timespec sleep_time;
  struct timespec limit;

  memset (&last_frame, 0, sizeof (struct timespec));
  memset (&current_frame, 0, sizeof (struct timespec));
  memset (&frame_delta, 0, sizeof (struct timespec));
  memset (&sleep_time, 0, sizeof (struct timespec));
  memset (&limit, 0, sizeof (struct timespec));
  
  time_set (&limit, 0, 40000000);
  
  clock_settime (CLOCK_MONOTONIC_RAW, &last_frame);
  current_frame = last_frame;

  const xkb_keysym_t esc_sym = xkb_keysym_from_name ("Escape", XKB_KEYSYM_NO_FLAGS);

  current = buf.bufs;
  
  while (!close)
    {
      if (!current->used && ready)
        {
          ready = false;
          current->used = true;
          if (ready_cb != NULL)
            wl_callback_destroy (ready_cb);
          ready_cb = wl_surface_frame (objs.surface);
          wl_callback_add_listener (ready_cb, &frame_limiter_listener, &ready);
          
          fb_index = (fb_index + 1) & 1;

          render (current->pixels, width, height);
          wl_surface_attach (objs.surface, current->wl_buffer, 0, 0);
          wl_surface_damage (objs.surface, 0, 0, width, height);
          wl_surface_commit (objs.surface);
          current = current->next;
        }
      
      if (wl_display_dispatch (objs.display) < 0)
        {
          fprintf (stderr, "Could not dispatch messages in main loop.\n");
          break;
        }

      if (int_set_contains(&objs.inputs.keyboard.pressed, esc_sym))
        close = true;
          
      last_frame = current_frame;
      clock_settime (CLOCK_MONOTONIC_RAW, &current_frame);
      time_elapsed (&frame_delta, &current_frame, &last_frame);
      if (time_lessthan (&frame_delta, &limit))
        {
          time_elapsed (&sleep_time, &limit, &frame_delta);
          nanosleep (&sleep_time, NULL);
        }
   }

  wl_callback_destroy (ready_cb);
  free_buffers (&buf);
  free_mem (&mem);
  destroy_wayland_data (&objs);
  printf ("Disconnected from display.\n");
  return EXIT_SUCCESS;
}
