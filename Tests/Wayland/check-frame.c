// Test-only observer for the Skia image passed to GTK's Wayland draw callback.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>

static int stage;
static uint32_t knob_before;
static gboolean sent_scroll;

static gboolean advance_test(gpointer unused)
{
  (void)unused;
  GList* windows = gtk_window_list_toplevels();
  for (GList* item = windows; item; item = item->next)
  {
    GtkWindow* window = GTK_WINDOW(item->data);
    if (g_strcmp0(gtk_window_get_title(window), "IPlugEffect") != 0)
      continue;

    if (!GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(GTK_WIDGET(window))))
      exit(1);

    if (stage == 1)
    {
      GtkWidget* area = gtk_bin_get_child(GTK_BIN(window));
      GdkEvent* event = gdk_event_new(GDK_SCROLL);
      event->scroll.window = g_object_ref(gtk_widget_get_window(area));
      event->scroll.x = 300;
      event->scroll.y = 200;
      event->scroll.direction = GDK_SCROLL_SMOOTH;
      event->scroll.delta_y = -50;
      gboolean handled = FALSE;
      g_signal_emit_by_name(area, "scroll-event", &event->scroll, &handled);
      gdk_event_free(event);
      if (!handled)
        exit(1);
      sent_scroll = TRUE;
    }
    else if (stage == 2)
      gtk_window_resize(window, 720, 720);
    else
      gtk_window_close(window);
    break;
  }
  g_list_free(windows);
  return G_SOURCE_REMOVE;
}

void cairo_set_source_surface(cairo_t* cr, cairo_surface_t* surface, double x, double y)
{
  typedef void (*SetSource)(cairo_t*, cairo_surface_t*, double, double);
  static SetSource real_set_source;
  if (!real_set_source)
    real_set_source = (SetSource)dlsym(RTLD_NEXT, "cairo_set_source_surface");
  real_set_source(cr, surface, x, y);

  if (stage >= 3 || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
      cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
    return;

  const int expected = stage < 2 ? 600 : 720;
  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);
  double scale_x = 1, scale_y = 1;
  cairo_surface_get_device_scale(surface, &scale_x, &scale_y);
  if (width != expected * scale_x || height != expected * scale_y)
    return;

  // The panel background should be opaque gray, with text and control pixels
  // somewhere inside it. This also catches an unpainted or uniformly filled frame.
  cairo_surface_flush(surface);
  const unsigned char* data = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const uint32_t background = ((const uint32_t*)(data + 10 * stride))[10];
  const unsigned blue = background & 255;
  const unsigned green = (background >> 8) & 255;
  const unsigned red = (background >> 16) & 255;
  int varied = 0;
  for (int row = height / 5; row < 4 * height / 5; ++row)
  {
    const uint32_t* pixels = (const uint32_t*)(data + row * stride);
    for (int column = width / 5; column < 4 * width / 5; ++column)
      varied += pixels[column] != background;
  }
  if ((background >> 24) != 255 || red != green || red != blue ||
      red == 0 || red == 255 || varied < 100)
  {
    fprintf(stderr, "Wayland smoke test: unexpected pixels in %dx%d frame\n", width, height);
    exit(1);
  }

  if (stage < 2)
  {
    uint32_t knob_hash = 0;
    for (int row = 150 * scale_y; row < 250 * scale_y; ++row)
    {
      const uint32_t* pixels = (const uint32_t*)(data + row * stride);
      for (int column = 250 * scale_x; column < 350 * scale_x; ++column)
        knob_hash = knob_hash * 31 + pixels[column];
    }
    if (stage == 0)
      knob_before = knob_hash;
    else if (!sent_scroll || knob_hash == knob_before)
      return;
    else
      fprintf(stderr, "Wayland smoke test: scroll changed the rendered knob\n");
  }

  fprintf(stderr, "Wayland smoke test: painted %dx%d at %.0fx\n", width, height, scale_x);
  ++stage;
  // Allow GTK to commit the painted frame before asking for resize or close.
  g_timeout_add(120, advance_test, NULL);
}
