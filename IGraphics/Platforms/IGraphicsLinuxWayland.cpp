/*
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.
 See LICENSE.txt for more info.
*/

#if defined IPLUG_EXPERIMENTAL_WAYLAND && defined IGRAPHICS_SKIA && defined IGRAPHICS_CPU && !defined NO_IGRAPHICS

#include <gtk/gtk.h>
#include "IGraphicsLinux.h"
#include "include/core/SkPixmap.h"

using namespace iplug;
using namespace iplug::igraphics;

namespace
{
IMouseMod MouseModifiers(guint state, bool left, bool right)
{
  return {left, right, bool(state & GDK_SHIFT_MASK),
          bool(state & GDK_CONTROL_MASK), bool(state & GDK_MOD1_MASK)};
}
}

void* IGraphicsLinux::OpenWaylandWindow()
{
  if (mWaylandArea)
    return mWaylandArea;

  mWaylandArea = gtk_drawing_area_new();
  gtk_container_add(GTK_CONTAINER(mWaylandWindow), mWaylandArea);
  gtk_widget_set_can_focus(mWaylandArea, TRUE);
  gtk_widget_add_events(mWaylandArea, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
    GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK |
    GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);

  OnViewInitialized(nullptr);
  GetDelegate()->LayoutUI(this);
  AttachPopupMenuControl();
  SetAllControlsDirty();

  auto* im = gtk_im_multicontext_new();
  g_object_set_data_full(G_OBJECT(mWaylandArea), "iplug-im-context", im, g_object_unref);
  gtk_im_context_set_use_preedit(im, FALSE);
  g_signal_connect(mWaylandArea, "realize", G_CALLBACK(+[](GtkWidget* widget, gpointer data) {
    gtk_im_context_set_client_window(GTK_IM_CONTEXT(data), gtk_widget_get_window(widget));
  }), im);
  g_signal_connect(mWaylandArea, "unrealize", G_CALLBACK(+[](GtkWidget*, gpointer data) {
    gtk_im_context_set_client_window(GTK_IM_CONTEXT(data), nullptr);
  }), im);
  g_signal_connect(mWaylandArea, "focus-in-event", G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
    gtk_im_context_focus_in(GTK_IM_CONTEXT(data));
    return FALSE;
  }), im);
  g_signal_connect(mWaylandArea, "focus-out-event", G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
    gtk_im_context_focus_out(GTK_IM_CONTEXT(data));
    return FALSE;
  }), im);
  g_signal_connect(im, "commit", G_CALLBACK(+[](GtkIMContext* im, const gchar* text, gpointer data) {
    auto& graphics = *static_cast<IGraphicsLinux*>(data);
    if (!graphics.mCloseRequested)
    {
      DispatchScope dispatch(graphics.mDispatchDepth);
      auto* event = static_cast<GdkEventKey*>(g_object_get_data(G_OBJECT(im), "iplug-key-event"));
      const bool singleKey = event && *text && !*g_utf8_next_char(text) &&
                             gdk_keyval_to_unicode(event->keyval) == g_utf8_get_char(text);
      graphics.DispatchLinuxKey(true, singleKey ? event->keyval : 0, text, strlen(text),
                                singleKey && (event->state & GDK_SHIFT_MASK), false, false);
    }
  }), this);
  auto keyEvent = +[](GtkWidget* widget, GdkEventKey* event, gpointer data) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(data);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback())
      return FALSE;
    DispatchScope dispatch(graphics.mDispatchDepth);
    const bool pressed = event->type == GDK_KEY_PRESS;
    const bool shift = event->state & GDK_SHIFT_MASK;
    const bool control = event->state & GDK_CONTROL_MASK;
    const bool alt = event->state & GDK_MOD1_MASK;
    auto* im = GTK_IM_CONTEXT(g_object_get_data(G_OBJECT(widget), "iplug-im-context"));
    if (!control && !alt)
    {
      g_object_set_data(G_OBJECT(im), "iplug-key-event", event);
      const bool consumed = gtk_im_context_filter_keypress(im, event);
      g_object_set_data(G_OBJECT(im), "iplug-key-event", nullptr);
      if (consumed)
        return TRUE;
    }
    char text[8]{};
    const auto unicode = gdk_keyval_to_unicode(event->keyval);
    const int length = unicode && !g_unichar_iscntrl(unicode) ? g_unichar_to_utf8(unicode, text) : 0;
    return graphics.DispatchLinuxKey(pressed, event->keyval, text, length, shift, control, alt);
  };
  g_signal_connect(mWaylandArea, "key-press-event", G_CALLBACK(keyEvent), this);
  g_signal_connect(mWaylandArea, "key-release-event", G_CALLBACK(keyEvent), this);
  gtk_widget_grab_focus(mWaylandArea);

  g_signal_connect(mWaylandArea, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* pContext, gpointer pData) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback())
      return FALSE;

    DispatchScope dispatch(graphics.mDispatchDepth);
    cairo_set_source_rgb(pContext, 0, 0, 0);
    cairo_paint(pContext);
    IRECTList rects;
    rects.Add(graphics.GetBounds());
    graphics.mWaylandPaint = pContext;
    graphics.SetAllControlsClean();
    graphics.Draw(rects);
    graphics.mWaylandPaint = nullptr;
    return TRUE;
  }), this);

  g_signal_connect(mWaylandArea, "size-allocate", G_CALLBACK(+[](GtkWidget* pArea, GtkAllocation* pSize, gpointer pData) {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback() ||
        pSize->width <= 1 || pSize->height <= 1)
      return;

    DispatchScope dispatch(graphics.mDispatchDepth);
    graphics.mInConfigureNotify = true;
    const int scale = gtk_widget_get_scale_factor(pArea);
    if (scale != graphics.GetScreenScale())
      graphics.SetScreenScale(static_cast<float>(scale));
    graphics.ResizeFromHost(pSize->width * scale, pSize->height * scale);
    graphics.mInConfigureNotify = false;
  }), this);

  g_signal_connect(mWaylandArea, "button-press-event", G_CALLBACK((+[](GtkWidget* widget, GdkEventButton* pEvent, gpointer pData) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback() ||
        (pEvent->button != GDK_BUTTON_PRIMARY && pEvent->button != GDK_BUTTON_SECONDARY))
      return FALSE;

    DispatchScope dispatch(graphics.mDispatchDepth);
    gtk_widget_grab_focus(widget);
    graphics.mCursorX = pEvent->x / graphics.GetDrawScale();
    graphics.mCursorY = pEvent->y / graphics.GetDrawScale();
    const auto mod = MouseModifiers(pEvent->state, pEvent->button == GDK_BUTTON_PRIMARY,
                                   pEvent->button == GDK_BUTTON_SECONDARY);
    if (pEvent->type == GDK_2BUTTON_PRESS)
      graphics.OnMouseDblClick(graphics.mCursorX, graphics.mCursorY, mod);
    else if (pEvent->type == GDK_BUTTON_PRESS)
      graphics.OnMouseDown({{graphics.mCursorX, graphics.mCursorY, 0.f, 0.f, mod}});
    return TRUE;
  })), this);

  g_signal_connect(mWaylandArea, "button-release-event", G_CALLBACK((+[](GtkWidget*, GdkEventButton* pEvent, gpointer pData) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback() ||
        (pEvent->button != GDK_BUTTON_PRIMARY && pEvent->button != GDK_BUTTON_SECONDARY))
      return FALSE;

    DispatchScope dispatch(graphics.mDispatchDepth);
    graphics.mCursorX = pEvent->x / graphics.GetDrawScale();
    graphics.mCursorY = pEvent->y / graphics.GetDrawScale();
    const auto mod = MouseModifiers(pEvent->state, pEvent->button == GDK_BUTTON_PRIMARY,
                                   pEvent->button == GDK_BUTTON_SECONDARY);
    graphics.OnMouseUp({{graphics.mCursorX, graphics.mCursorY, 0.f, 0.f, mod}});
    return TRUE;
  })), this);

  g_signal_connect(mWaylandArea, "motion-notify-event", G_CALLBACK((+[](GtkWidget*, GdkEventMotion* pEvent, gpointer pData) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback())
      return FALSE;

    DispatchScope dispatch(graphics.mDispatchDepth);
    const float x = pEvent->x / graphics.GetDrawScale();
    const float y = pEvent->y / graphics.GetDrawScale();
    const auto mod = MouseModifiers(pEvent->state, pEvent->state & GDK_BUTTON1_MASK,
                                    pEvent->state & GDK_BUTTON3_MASK);
    const float dx = x - graphics.mCursorX;
    const float dy = y - graphics.mCursorY;
    graphics.mCursorX = x;
    graphics.mCursorY = y;
    if (mod.L || mod.R)
      graphics.OnMouseDrag({{x, y, dx, dy, mod}});
    else
      graphics.OnMouseOver(x, y, mod);
    return TRUE;
  })), this);

  g_signal_connect(mWaylandArea, "leave-notify-event", G_CALLBACK(+[](GtkWidget*, GdkEventCrossing*, gpointer pData) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (!graphics.mCloseRequested && !graphics.IsInPlatformCallback())
    {
      DispatchScope dispatch(graphics.mDispatchDepth);
      graphics.OnMouseOut();
    }
    return FALSE;
  }), this);

  GtkTargetEntry target{const_cast<char*>("text/uri-list"), 0, 0};
  gtk_drag_dest_set(mWaylandArea, GTK_DEST_DEFAULT_ALL, &target, 1, GDK_ACTION_COPY);
  g_signal_connect(mWaylandArea, "drag-data-received", G_CALLBACK(+[](GtkWidget*, GdkDragContext*, gint x, gint y,
    GtkSelectionData* selection, guint, guint, gpointer data) {
    auto& graphics = *static_cast<IGraphicsLinux*>(data);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback())
      return;
    const int length = gtk_selection_data_get_length(selection);
    if (length > 0)
    {
      DispatchScope dispatch(graphics.mDispatchDepth);
      graphics.DispatchLinuxDrop(gtk_selection_data_get_data(selection), length,
                                 x / graphics.GetDrawScale(), y / graphics.GetDrawScale());
    }
  }), this);

  g_signal_connect(mWaylandArea, "scroll-event", G_CALLBACK(+[](GtkWidget*, GdkEventScroll* pEvent, gpointer pData) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback())
      return FALSE;

    const float delta = pEvent->direction == GDK_SCROLL_UP ? 1.f :
                        pEvent->direction == GDK_SCROLL_DOWN ? -1.f :
                        pEvent->direction == GDK_SCROLL_SMOOTH ? -pEvent->delta_y : 0.f;
    if (delta == 0.f)
      return FALSE;
    DispatchScope dispatch(graphics.mDispatchDepth);
    graphics.OnMouseWheel(pEvent->x / graphics.GetDrawScale(), pEvent->y / graphics.GetDrawScale(),
                          MouseModifiers(pEvent->state, false, false), delta);
    return TRUE;
  }), this);

  mWaylandTimer = g_timeout_add(std::max(1, 1000 / FPS()), +[](gpointer pData) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (!graphics.mCloseRequested && !graphics.IsInPlatformCallback())
    {
      DispatchScope dispatch(graphics.mDispatchDepth);
      IRECTList dirty;
      if (graphics.IsDirty(dirty))
        gtk_widget_queue_draw(graphics.mWaylandArea);
    }
    return G_SOURCE_CONTINUE;
  }, this);

  {
    DispatchScope dispatch(mDispatchDepth);
    GetDelegate()->OnUIOpen();
  }
  return mWaylandArea;
}

void IGraphicsLinux::CloseWaylandWindow()
{
  if (mWaylandTimer)
  {
    g_source_remove(mWaylandTimer);
    mWaylandTimer = 0;
  }
  g_signal_handlers_disconnect_by_data(mWaylandArea, this);
  OnViewDestroyed();
  gtk_widget_destroy(mWaylandArea);
  mWaylandArea = nullptr;
  mWaylandWindow = nullptr;
}

void IGraphicsLinux::ResizeWaylandWindow()
{
  gtk_window_resize(GTK_WINDOW(mWaylandWindow), WindowWidth(), WindowHeight());
}

bool IGraphicsLinux::ReadGtkWaylandClipboard(WDL_String& text)
{
  DispatchScope dispatch(mDispatchDepth);
  auto* clipboard = gtk_widget_get_clipboard(mWaylandArea, GDK_SELECTION_CLIPBOARD);
  auto* value = gtk_clipboard_wait_for_text(clipboard);
  if (!value)
    return false;
  text.Set(value);
  g_free(value);
  return true;
}

bool IGraphicsLinux::WriteGtkWaylandClipboard(const char* text)
{
  auto* clipboard = gtk_widget_get_clipboard(mWaylandArea, GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(clipboard, text ? text : "", -1);
  return true;
}

void IGraphicsLinux::PaintWaylandFrame(const SkPixmap& pixels)
{
  auto* pImage = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels.width(), pixels.height());
  if (cairo_surface_status(pImage) == CAIRO_STATUS_SUCCESS)
  {
    auto* pData = cairo_image_surface_get_data(pImage);
    const int stride = cairo_image_surface_get_stride(pImage);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    const auto info = SkImageInfo::Make(pixels.width(), pixels.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    const bool copied = pixels.readPixels(info, pData, stride);
#else
    for (int y = 0; y < pixels.height(); ++y)
    {
      auto* pRow = reinterpret_cast<SkPMColor*>(pData + y * stride);
      for (int x = 0; x < pixels.width(); ++x)
        pRow[x] = SkPreMultiplyColor(pixels.getColor(x, y));
    }
    const bool copied = true;
#endif
    if (copied)
    {
      cairo_surface_mark_dirty(pImage);
      cairo_surface_set_device_scale(pImage, GetScreenScale(), GetScreenScale());
      cairo_save(mWaylandPaint);
      cairo_set_source_surface(mWaylandPaint, pImage, 0, 0);
      cairo_paint(mWaylandPaint);
      cairo_restore(mWaylandPaint);
    }
  }
  cairo_surface_destroy(pImage);
}

#endif
