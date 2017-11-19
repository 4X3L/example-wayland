/* main.c
 * 
 * Copyright © 2017 Alex Benishek
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

/* What is the maximum number of screens that our program will recognize */
#define MAX_OUTPUT 6

/* The confusing thing about wayland is that there are two main parts, a 
   "meta-protocol" that can be used to define a "concrete-protocol" that the client
   and server use to communicate. Because of this things like, 
   "wl_registry_add_listener" and "wl_registry_bind" do not have definitions in 
   wayland-client.h. Instead, they are found in an xml file in "protocol/wayland.xml"
   in the wayland source code. The "meta-protocol" then uses this xml file to 
   generate a concrete protocol that our client will use to actually communicate 
   with the server. The only thing the "meta-protocol" provides is a way to 
   communicate with the server, what is communicated is specified by 
   "concrete-protocol". 

   Despite being written in C the concrete protocol is actually an object oriented 
   protocol. The objects in the protocol are called "interfaces" The general form of 
   "methods" are "<Wayland Namespace>_<Interface Name>_<Method Name>" So if we were
   to write this in C++ this:

   struct wl_registry *registry = wl_display_get_registry(...);
   wl_registry_bind(registry, ...);

   would look like this:
   
   wl::registry reg = display.get_registry(...);
   reg.bind(...); 
   
   The order of parameters in a method is:
   Object pointer, ... Protocol Specific Params ... , Protocol Version Number
*/

/* The API operates asynchronously so in order to get information about a display we 
   have to create a callback which sets the data we want and then we send that
   callback to the server and wait for a response. */
struct output_info
{
  int32_t width;
  int32_t height;
  int32_t hertz;
};

/* If we specify one callback to run once an event starts then we have to specify all 
   events that the object supports. So we will want to give a dummy callback that 
   does nothing. */
static void do_nothing () {}

static void
output_mode_cb (void *data, struct wl_output *out, uint32_t flags,
                int32_t width, int32_t height, int32_t refresh_rate)
{
  (void) flags;
  (void) out;
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
    do_nothing,
  };
/* The important part of adding listeners is that you add the listener of a message 
   before reading the message. As long as you add the listener before reading the 
   message in the main loop the callback will still trigger so even though the client
   recieved the message from the server, the imporant part is not reading it until 
   client is ready.
   
   Thanks to SardemFF7 on #wayland IRC for explaining that */

/* These callbacks can be though of as methods, and we can also specify fields that 
   the object has.

   In order to allow the function pointer to access the fields of the object we 
   have to give a pointer to a struct which represents the fields. That pointer is 
   then given as the first parameter to the function pointer. 

   We have to write:

   int 
   cb (void *data)
     {
       int *i_ptr = data;
       return *i_ptr;
     }
   static struct foo_listener foo_listener { cb };

   int
   main ()
     {
       int b = 3;
       wl_foo_add_listener(bar, foo_listener, &b);
     }
   
*/

/* These are the user fields of the registry object. This object provides a way to 
   access the other objects */
struct registry_objects
{
  struct wl_compositor *comp;
  /* A block of unused memory shared between server and client. Sub-blocks are carved
     out of this block to form buffers */
  struct wl_shm *shm;
  /* Reference to the graphical shell */
  struct wl_shell *shell;
  /* A "seat", described in the documentation as a collection of keyboard, monitors, 
     and "pointer" (mouse or touchscreen) devices. I *think* the idea is that if you 
     have multiple thin graphical clients connected to a server that you want the 
     mouse and keyboard and monitor that are all in some way "associated" together. 
     So in the case of clients, the devices all associated with the same thin client*/
  struct wl_seat *seat;
  /* There could be multiple monitors (wl_output) so we need to have an array of 
     monitors */
  struct wl_output *outputs[MAX_OUTPUT];
  /* The number of monitors actually used */
  uint32_t output_num;
};


/* Whenever something is added to the registry our program will be notified by 
   wayland running this callback */
static void
global_registry_handler (void *data, /* data is the final paramter passed to 
                                        add_listener */
                         /* Pointer to registry this method was called on */
                         struct wl_registry *registry,
                         /* Wayland ID "name" of object that became visible */
                         uint32_t id,
                         /* The text name of object that became visible */
                         const char *interface,
                         /* The version of the object that became visible */
                         uint32_t version) 
{
  struct registry_objects *objs = data;
  printf("Got a registry event for %s id %d version %d \n", interface, id, version);
  if (strcmp(interface, wl_compositor_interface.name) == 0)
    objs->comp = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
  /* The above code takes the registry called "registry" and associates the name
     id with the object wl_compositor_interface. Also, we are using version 1 of 
     the API */
  /* Shell is short for graphical shell, it is the GUI program that starts and stops
     other programs. A surface to draw on is gotten from the shell */
  else if (strcmp(interface, wl_shell_interface.name) == 0)
    objs->shell = wl_registry_bind(registry, id, &wl_shell_interface, 1);
  /* The pool of memory that is shared between client and server. Buffers are carved
     out of this pool and is what is actually put on screen */
  else if (strcmp(interface, wl_shm_interface.name) == 0)
    objs->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
  /* I believe the idea behind a seat is when you have multiple dumb, thin clients
     connnected to a server so ther will be multiple keyboards, mice, monitors, etc.
     but those IO devices are grouped in some way. A "seat" is a grouping of such 
     devices */
  else if (strcmp(interface, wl_seat_interface.name) == 0)
    objs->seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
  /* Get all outputs, in the case that there are multiple monitors */
  else if (strcmp(interface, wl_output_interface.name) == 0)
    if (objs->output_num < MAX_OUTPUT)
      {
        objs->outputs[objs->output_num] = wl_registry_bind(registry, id, &wl_output_interface, 1);
        objs->output_num++;
      }
}

/* The registry expects a pair of callbacks, one for when an object is created,
   the other for when it is destroyed. The pair is wrapped in a struct. */
static const struct wl_registry_listener registry_listener = 
  {
    global_registry_handler,
    do_nothing,
  };

/* In order to know if a client has become unresponsive the server will send "ping" 
   messages and the client needs to repond with "pong" or the client will be deemed 
   to be unresponsive */
static void
ping_cb (void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
  (void) data;
  wl_shell_surface_pong (shell_surface, serial);
}

static const struct wl_shell_surface_listener shell_surface_listener = 
  {
    ping_cb,
    do_nothing,
    do_nothing
  };

int
main () 
{
  /* There is a socket on the filesystem somewhere that programs can use to 
     communicate with the wayland server. We want to connect to that socket.
     We can specify a specific socket to use be we just want the default one
     so we pass in NULL. This returns an opaque pointer to an object 
     representing our connection to the wayland server */
  struct wl_display *display = wl_display_connect(NULL);
  struct registry_objects objs;
  struct output_info output_info[MAX_OUTPUT];
  
  memset (&objs, 0, sizeof(objs));
  memset (&output_info, 0, sizeof (struct output_info) * MAX_OUTPUT);
  
  int status = EXIT_SUCCESS;

  CHECK_NULL (display,
              exit,
              "Could not connect to wayland display.\n",
              "Connected to wayland display.\n");
  
  /* There are many objects contained in the server that we would like to use.
     these objects are contained in a registry on the server */
  struct wl_registry *registry = wl_display_get_registry (display);
  CHECK_NULL (registry,
              disconnect_display,
              "Could not get registry form display.\n",
              "Got registry from display.\n");
  
  wl_registry_add_listener (registry, &registry_listener, &objs);

  /* Messages created by our client are initially just queued to be sent but are not
     actually sent. For exaple, wl_registry_add_listener will add an event to the 
     queue but will not actually notify the server directly. "wl_display_dispatch"
     will send all of the messages in the queue to the server. */

  /* wl_display_roundtrip will just sit and wait for the server's response. Wayland 
     is an asyncronous API so normally we wouldn't want to wait for the server 
     response but in this case we can't do anything until we have the compositor
     anyhow so we might as well wait */
  wl_display_roundtrip (display);
  /* Since we are done with the registry we can destroy it now. */
  wl_registry_destroy (registry);
  printf ("Destroyed registry.\n");

  /* Check that all of the objects from the registry were found. */
  CHECK_NULL (objs.comp,
              disconnect_display,
              "Could not find compositor.\n",
              "Found compositor.\n");

  CHECK_NULL (objs.shell,
              destroy_compositor,
              "Could not find graphical shell.\n",
              "Found graphical shell\n");
  
  CHECK_NULL (objs.seat,
              destroy_shell,
              "Could not find seat.\n",
              "Found seat.\n");

  /* An shm object is a handle that allows creation of pools of shared memory and
     informs the client (that's us!) about supported memory formats. So if would want
     AYCbCr Layout for some reason we would check the shm object to see if that is 
     supported by the server. However, the wayland specification requires all servers
     to support ARBG 8-bit layout so that is the one we will use and won't bother
     checking. */
  CHECK_NULL (objs.shm,
              destroy_seat,
              "Could not find shared memory.\n",
              "Found shared memory.\n");

  CHECK_COND(objs.output_num == 0,
             destroy_shm,
             "No output devices found.\n",
             "Found monitors.\n");

  /* Get information about each output */
  for (size_t i = 0; i < objs.output_num; i++)
     wl_output_add_listener (objs.outputs[i], &output_listener, &output_info[i]);

  /* The listener we added won't be triggered until we  actually read events */
  /* Important to remember that if a callback segfaults, gdb will have problems 
     telling you exactly where the segfault occured */
  wl_display_roundtrip (display); /* Wait for server to aknowledge our message */
  
  for (size_t i = 0; i < objs.output_num; i++)
    printf ("Monitor resolution: %d pixels by %d pixels at %dmHz\n",
            output_info[i].width, output_info[i].height, output_info[i].hertz);

  /* We'd like to use the shm object to create a shared memory pool and then use the
     shared memory pool to create frame buffers that we can then use to acutally
     display things, but we need to know the size of the monitors in order to know
     how much memory to allocate. We don't know which monitor the user wants the 
     display to be on. We might also want to deal with "hotswapping" monitors. 
     We will have to do something smarter than this in a real program but for now we
     will arbitrarily choose a monitor. */

  /* First we need to create an in memory "file". We then us mmap to turn that "file"
     into an actual memory buffer. */
  /* This creates an acutal file in the file system so if we run two instances of 
     the program at the same time this will fail, except maybe not? The O_CREAT flag
     might create a object with new name */
  int mem_fd = shm_open ("/unknown_animal_wayland_frame_buffer",
                         O_RDWR | O_CREAT, S_IRUSR | S_IWUSR );
  CHECK_COND(mem_fd < 0,
             destroy_outputs,
             "Could not open shared memory file",
             "Created shared memory file.\n");
  
  const size_t width = output_info[0].width;
  const size_t height = output_info[0].height;
  const size_t num_px = width * height;
  const size_t fb_bs = num_px * sizeof (struct pixel);

  CHECK_COND(ftruncate (mem_fd, fb_bs) != 0,
             unlink_shm,
             "Could not reized shared memory file",
             "Resized chared memory file descriptor.\n");
        
  struct pixel *raw_pixels =
    mmap (NULL, fb_bs, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd,  0);

  CHECK_COND(raw_pixels == MAP_FAILED,
             unlink_shm,
             "Could not map file to memory",
             "Mapped file to memory.\n");
  
  for (uint32_t i = 0; i < num_px; i++)
    {
      raw_pixels[i].a = 255;
      raw_pixels[i].r = 0;
      raw_pixels[i].g = 0;
      raw_pixels[i].b = 255;
    }
      
  
  struct wl_shm_pool *pool = wl_shm_create_pool(objs.shm, mem_fd, fb_bs);

  CHECK_NULL (pool,
              unmap_fb,
              "Could not create pool\n",
              "Created shared memory pool\n");

  /* Next that we have a chunk of shared memory and put it into a wl_shm_pool, we
     need to create a buffer from that pool and then bind that buffer to a surface.
     once that is done we should, fingers crossed, be able to display that surface
     on the screen. */
  
  /* A surface is just some rectangualar grid that the client can draw on */
  struct wl_surface *surface = wl_compositor_create_surface (objs.comp);
  
  CHECK_NULL (surface,
              destroy_shm_pool,
              "Could not create surface from compositor.\n",
              "Created surface from compositor.\n");
  

  /* A shell surface is like a surface but it is provided by the graphical shell 
     that started the client program. I am not fully certain of the rational behind
     having a distinction between a shell_surface and a normal surfae. */
  struct wl_shell_surface *shell_surface = wl_shell_get_shell_surface (objs.shell,
                                                                       surface);
  CHECK_NULL (shell_surface,
              destroy_surface,
              "Could not create shell surface.\n",
              "Created shell surface.\n");

  wl_shell_surface_add_listener (shell_surface, &shell_surface_listener, 0);

  struct wl_buffer *way_fb =
    wl_shm_pool_create_buffer (pool, 0,
                               output_info[0].width,
                               output_info[0].height,
                               output_info[0].width * sizeof(struct pixel),
                               WL_SHM_FORMAT_ARGB8888);
  
  CHECK_NULL (way_fb,
              destroy_shell_surface,
              "Could not create screen buffer.\n",
              "Created screen buffer.\n");
  
  wl_shell_surface_set_fullscreen (shell_surface,
                                   WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
                                   output_info[0].hertz, objs.outputs[0]);

  /* String must be utf-8 encoded */
  wl_shell_surface_set_title (shell_surface, "Wayland Example");

  /* Wait for half a second before drawing the next frame */
  struct timespec sleep;
  sleep.tv_nsec = 500000000;
  sleep.tv_sec = 0;

  uint32_t frame_num = 0;
  
  while (frame_num < 10)
    {
      uint8_t red = ((frame_num % 3) == 0) * 255;
      uint8_t green = ((frame_num % 3) == 1) * 255;
      uint8_t blue = ((frame_num % 3) == 2) * 255;

      /* Simple rendering, strobe effect. In a real program this is where we would
         do the rendering. */
      for (uint32_t i = 0; i < num_px; i++)
        {
          raw_pixels[i].r = red;
          raw_pixels[i].g = green;
          raw_pixels[i].b = blue;
        }
      
      /* Set the new contents of the surface */
      wl_surface_attach (surface, way_fb, 0, 0);
      /* Redraw the entire surface */
      wl_surface_damage (surface, 0, 0, width, height);

      /* Swap frame buffer */
      /* All wayland surfaces are double buffered by the server. So if we make a 
         change to the backing array of data that change will not be visible until 
         calling wl_surface_commit, at which point the server will replace what it 
         displays to the user with the updated buffer */
      wl_surface_commit (surface);

      /* Normal wl_display_dispatch will wait to recieve a message if there is not 
         one from the server. wl_display_dispatch pending not block on no messages */
      if (wl_display_dispatch_pending (display) < 0)
        {
          fprintf (stderr, "Could not dispatch messages in main loop.\n");
          break;
        }
      /* Send messages from the client to the server */
      if (wl_display_flush (display) < 0)
        {
          perror ("Could not flush data ");
          break;
        }

      nanosleep (&sleep, NULL);
      frame_num++;
   }
  
  /* Cleanup everything we created */
  wl_buffer_destroy (way_fb);
  printf ("Destroyed screen buffer.\n");
 destroy_shell_surface:
  wl_shell_surface_destroy (shell_surface);
  shell_surface = NULL;
  printf ("Destroyed shell surface.\n");
 destroy_surface:
  wl_surface_destroy (surface);
  surface = NULL;
  printf ("Destroyed surface.\n");
 destroy_shm_pool:
  wl_shm_pool_destroy(pool);
  pool = NULL;
  printf ("Destroyed shared memory pool.\n");
 unmap_fb:
  munmap (raw_pixels, fb_bs);
  raw_pixels = NULL;
  printf ("Unmaped framebuffer.\n");
 unlink_shm:
  shm_unlink ("unknown_animal_wayland_frame_buffer");
  printf ("Closed shared memory file.\n");
 destroy_outputs:
  for (size_t i = 0; i < objs.output_num; i++)
    {
      wl_output_destroy(objs.outputs[i]);
      objs.outputs[i] = NULL;
      printf ("Destroyed output %lu\n", i);
    }
 destroy_shm:
  wl_shm_destroy(objs.shm);
  objs.shm = NULL;
  printf ("Destroyed shared memory.\n");
 destroy_seat:
  wl_seat_destroy(objs.seat);
  objs.seat = NULL;
  printf ("Destroyed seat.\n");
 destroy_shell:
  wl_shell_destroy(objs.shell);
  objs.shell = NULL;
  printf ("Destroyed shell.\n");
 destroy_compositor:
  wl_compositor_destroy(objs.comp);
  objs.comp = NULL;
  printf ("Destroyed compositor.\n");
 disconnect_display:
  wl_display_disconnect (display);
  display = NULL;
  printf ("Disconnected from display.\n");
 exit:  
  return status;
}
