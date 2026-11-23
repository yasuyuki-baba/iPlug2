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
  gtk_widget_add_events(mWaylandArea, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
    GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);

  OnViewInitialized(nullptr);
  GetDelegate()->LayoutUI(this);
  SetAllControlsDirty();

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

  g_signal_connect(mWaylandArea, "button-press-event", G_CALLBACK((+[](GtkWidget*, GdkEventButton* pEvent, gpointer pData) -> gboolean {
    auto& graphics = *static_cast<IGraphicsLinux*>(pData);
    if (graphics.mCloseRequested || graphics.IsInPlatformCallback() ||
        (pEvent->button != GDK_BUTTON_PRIMARY && pEvent->button != GDK_BUTTON_SECONDARY))
      return FALSE;

    DispatchScope dispatch(graphics.mDispatchDepth);
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
