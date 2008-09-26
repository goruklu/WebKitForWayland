/*
 * Copyright (C) 2006, 2007, 2008 Apple Computer, Inc. All rights reserved.
 * Copyright (C) 2006 Michael Emmel mike.emmel@gmail.com
 * Copyright (C) 2007 Holger Hans Peter Freyther
 * Copyright (C) 2008 Collabora Ltd.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ScrollView.h"

#include "FloatRect.h"
#include "FocusController.h"
#include "Frame.h"
#include "FrameView.h"
#include "GraphicsContext.h"
#include "IntRect.h"
#include "NotImplemented.h"
#include "PlatformMouseEvent.h"
#include "PlatformWheelEvent.h"
#include "Page.h"
#include "RenderLayer.h"
#include "ScrollbarGtk.h"
#include "ScrollbarTheme.h"

#include <gtk/gtk.h>

using namespace std;

namespace WebCore {

class ScrollViewScrollbar : public ScrollbarGtk {
public:
    static PassRefPtr<ScrollViewScrollbar> create(ScrollbarClient* client, ScrollbarOrientation orientation, ScrollbarControlSize size)
    {
        return adoptRef(new ScrollViewScrollbar(client, orientation, size));
    }

private:
    ScrollViewScrollbar(ScrollbarClient*, ScrollbarOrientation, ScrollbarControlSize);

    virtual void geometryChanged() const;
};

class ScrollView::ScrollViewPrivate : public ScrollbarClient
{
public:
    ScrollViewPrivate(ScrollView* _view)
        : view(_view)
        , scrollbarsSuppressed(false)
        , vScrollbarMode(ScrollbarAuto)
        , hScrollbarMode(ScrollbarAuto)
        , inUpdateScrollbars(false)
        , horizontalAdjustment(0)
        , verticalAdjustment(0)
    {}

    ~ScrollViewPrivate()
    {
        setHasHorizontalScrollbar(false);
        setHasVerticalScrollbar(false);

        if (horizontalAdjustment) {
            g_signal_handlers_disconnect_by_func(G_OBJECT(horizontalAdjustment), (gpointer)ScrollViewPrivate::adjustmentChanged, this);
            g_object_unref(horizontalAdjustment);
        }

        if (verticalAdjustment) {
            g_signal_handlers_disconnect_by_func(G_OBJECT(verticalAdjustment), (gpointer)ScrollViewPrivate::adjustmentChanged, this);
            g_object_unref(verticalAdjustment);
        }
    }

    void scrollBackingStore(const IntSize& scrollDelta);

    void setHasHorizontalScrollbar(bool hasBar);
    void setHasVerticalScrollbar(bool hasBar);

    virtual void valueChanged(Scrollbar*);
    virtual IntRect windowClipRect() const;
    virtual bool isActive() const;

    static void adjustmentChanged(GtkAdjustment*, gpointer);

    ScrollView* view;
    bool scrollbarsSuppressed;
    ScrollbarMode vScrollbarMode;
    ScrollbarMode hScrollbarMode;
    RefPtr<ScrollViewScrollbar> vBar;
    RefPtr<ScrollViewScrollbar> hBar;
    IntSize viewPortSize;
    bool inUpdateScrollbars;
    HashSet<Widget*> children;
    GtkAdjustment* horizontalAdjustment;
    GtkAdjustment* verticalAdjustment;
};

ScrollViewScrollbar::ScrollViewScrollbar(ScrollbarClient* client, ScrollbarOrientation orientation, ScrollbarControlSize size)
    : ScrollbarGtk(client, orientation, size)
{
}

void ScrollViewScrollbar::geometryChanged() const
{
    if (!parent())
        return;

    ASSERT(parent()->isFrameView());

    FrameView* frameView = static_cast<FrameView*>(parent());
    IntPoint loc = frameView->convertToContainingWindow(frameGeometry().location());

    // Don't allow the allocation size to be negative
    IntSize sz = frameGeometry().size();
    sz.clampNegativeToZero();

    GtkAllocation allocation = { loc.x(), loc.y(), sz.width(), sz.height() };
    gtk_widget_size_allocate(platformWidget(), &allocation);
}

void ScrollView::ScrollViewPrivate::setHasHorizontalScrollbar(bool hasBar)
{
    if (hasBar && !hBar && !horizontalAdjustment) {
        hBar = ScrollViewScrollbar::create(this, HorizontalScrollbar, RegularScrollbar);
        view->addChild(hBar.get());
    } else if (!hasBar && hBar) {
        view->removeChild(hBar.get());
        hBar = 0;
    }
}

void ScrollView::ScrollViewPrivate::setHasVerticalScrollbar(bool hasBar)
{
    if (hasBar && !vBar && !verticalAdjustment) {
        vBar = ScrollViewScrollbar::create(this, VerticalScrollbar, RegularScrollbar);
        view->addChild(vBar.get());
    } else if (!hasBar && vBar) {
        view->removeChild(vBar.get());
        vBar = 0;
    }
}

Scrollbar* ScrollView::horizontalScrollbar() const
{
    return m_data->hBar.get();
}

Scrollbar* ScrollView::verticalScrollbar() const
{
    return m_data->vBar.get();
}

void ScrollView::ScrollViewPrivate::scrollBackingStore(const IntSize& scrollDelta)
{
    // Since scrolling is double buffered, we will be blitting the scroll view's intersection
    // with the clip rect every time to keep it smooth.
    IntRect clipRect = view->windowClipRect();
    IntRect scrollViewRect = view->convertToContainingWindow(IntRect(0, 0, view->visibleWidth(), view->visibleHeight()));

    IntRect updateRect = clipRect;
    updateRect.intersect(scrollViewRect);

    //FIXME update here?

    if (view->canBlitOnScroll()) // The main frame can just blit the WebView window
       // FIXME: Find a way to blit subframes without blitting overlapping content
       view->scrollBackingStore(-scrollDelta.width(), -scrollDelta.height(), scrollViewRect, clipRect);
    else  {
       // We need to go ahead and repaint the entire backing store.  Do it now before moving the
       // plugins.
       view->addToDirtyRegion(updateRect);
       view->updateBackingStore();
    }

    view->geometryChanged();

    // Now update the window (which should do nothing but a blit of the backing store's updateRect and so should
    // be very fast).
    view->update();
}

void ScrollView::ScrollViewPrivate::adjustmentChanged(GtkAdjustment* adjustment, gpointer _that)
{
    ScrollViewPrivate* that = reinterpret_cast<ScrollViewPrivate*>(_that);

    // Figure out if we really moved.
    IntSize newOffset = m_view->m_scrollOffset;
    if (adjustment == that->horizontalAdjustment)
        newOffset.setWidth(static_cast<int>(gtk_adjustment_get_value(adjustment)));
    else if (adjustment == that->verticalAdjustment)
        newOffset.setHeight(static_cast<int>(gtk_adjustment_get_value(adjustment)));

    IntSize scrollDelta = newOffset - m_view->m_scrollOffset;
    if (scrollDelta == IntSize())
        return;
    m_view->m_scrollOffset = newOffset;

    if (that->scrollbarsSuppressed)
        return;

    that->scrollBackingStore(scrollDelta);
    static_cast<FrameView*>(that->view)->frame()->sendScrollEvent();
}

void ScrollView::ScrollViewPrivate::valueChanged(Scrollbar* bar)
{
    // Figure out if we really moved.
    IntSize newOffset = m_scrollOffset;
    if (bar) {
        if (bar == hBar)
            newOffset.setWidth(bar->value());
        else if (bar == vBar)
            newOffset.setHeight(bar->value());
    }
    IntSize scrollDelta = newOffset - m_scrollOffset;
    if (scrollDelta == IntSize())
        return;
    m_scrollOffset = newOffset;

    if (scrollbarsSuppressed)
        return;

    scrollBackingStore(scrollDelta);
    static_cast<FrameView*>(view)->frame()->sendScrollEvent();
}

IntRect ScrollView::ScrollViewPrivate::windowClipRect() const
{
    return static_cast<const FrameView*>(view)->windowClipRect(false);
}

bool ScrollView::ScrollViewPrivate::isActive() const
{
    Page* page = static_cast<const FrameView*>(view)->frame()->page();
    return page && page->focusController()->isActive();
}

ScrollView::ScrollView()
    : m_data(new ScrollViewPrivate(this))
{
    init();
}

ScrollView::~ScrollView()
{
    delete m_data;
}

/*
 * The following is assumed:
 *   (hadj && vadj) || (!hadj && !vadj)
 */
void ScrollView::setGtkAdjustments(GtkAdjustment* hadj, GtkAdjustment* vadj)
{
    ASSERT(!hadj == !vadj);

    if (m_data->horizontalAdjustment) {
        g_signal_handlers_disconnect_by_func(G_OBJECT(m_data->horizontalAdjustment), (gpointer)ScrollViewPrivate::adjustmentChanged, m_data);
        g_signal_handlers_disconnect_by_func(G_OBJECT(m_data->verticalAdjustment), (gpointer)ScrollViewPrivate::adjustmentChanged, m_data);
        g_object_unref(m_data->horizontalAdjustment);
        g_object_unref(m_data->verticalAdjustment);
    }

    m_data->horizontalAdjustment = hadj;
    m_data->verticalAdjustment = vadj;

    if (m_data->horizontalAdjustment) {
        g_signal_connect(m_data->horizontalAdjustment, "value-changed", G_CALLBACK(ScrollViewPrivate::adjustmentChanged), m_data);
        g_signal_connect(m_data->verticalAdjustment, "value-changed", G_CALLBACK(ScrollViewPrivate::adjustmentChanged), m_data);

        /*
         * disable the scrollbars (if we have any) as the GtkAdjustment over
         */
        m_data->setHasVerticalScrollbar(false);
        m_data->setHasHorizontalScrollbar(false);

        g_object_ref(m_data->horizontalAdjustment);
        g_object_ref(m_data->verticalAdjustment);
    }

    updateScrollbars(m_scrollOffset);
}

void ScrollView::updateContents(const IntRect& updateRect, bool now)
{
    if (updateRect.isEmpty())
        return;

    IntPoint windowPoint = contentsToWindow(updateRect.location());
    IntRect containingWindowRect = updateRect;
    containingWindowRect.setLocation(windowPoint);

    GdkRectangle rect = containingWindowRect;
    GdkWindow* window = GTK_WIDGET(containingWindow())->window;

    if (window)
        gdk_window_invalidate_rect(window, &rect, true);

    // Cache the dirty spot.
    addToDirtyRegion(containingWindowRect);

    if (now && window)
        gdk_window_process_updates(window, true);
}

void ScrollView::update()
{
    ASSERT(containingWindow());

    if (!containingWindow() || !GTK_WIDGET_REALIZED(containingWindow()))
        return;

    GdkRectangle rect = frameGeometry();
    gdk_window_invalidate_rect(GTK_WIDGET(containingWindow())->window, &rect, true);
}

void ScrollView::setScrollPosition(const IntPoint& scrollPoint)
{
    IntPoint newScrollPosition = scrollPoint.shrunkTo(maximumScrollPosition());
    newScrollPosition.clampNegativeToZero();

    if (newScrollPosition == scrollPosition())
        return;

    updateScrollbars(IntSize(newScrollPosition.x(), newScrollPosition.y()));
}

ScrollbarMode ScrollView::hScrollbarMode() const
{
    return m_data->hScrollbarMode;
}

ScrollbarMode ScrollView::vScrollbarMode() const
{
    return m_data->vScrollbarMode;
}

bool ScrollView::isScrollable() 
{ 
    return true; // FIXME : return whether or not the view is scrollable
}

void ScrollView::suppressScrollbars(bool suppressed, bool repaintOnSuppress)
{
    m_data->scrollbarsSuppressed = suppressed;
    if (repaintOnSuppress)
        updateScrollbars(m_scrollOffset);
}

void ScrollView::setHScrollbarMode(ScrollbarMode newMode)
{
    if (m_data->hScrollbarMode != newMode) {
        m_data->hScrollbarMode = newMode;
        updateScrollbars(m_scrollOffset);
    }
}

void ScrollView::setVScrollbarMode(ScrollbarMode newMode)
{
    if (m_data->vScrollbarMode != newMode) {
        m_data->vScrollbarMode = newMode;
        updateScrollbars(m_scrollOffset);
    }
}

void ScrollView::setScrollbarsMode(ScrollbarMode newMode)
{
    m_data->hScrollbarMode = m_data->vScrollbarMode = newMode;
    updateScrollbars(m_scrollOffset);
}

void ScrollView::setFrameGeometry(const IntRect& newGeometry)
{
    ASSERT(isFrameView());
    IntRect oldGeometry = frameGeometry();
    Widget::setFrameGeometry(newGeometry);

    if (newGeometry == oldGeometry)
        return;
    if (newGeometry.width() != oldGeometry.width() || newGeometry.height() != oldGeometry.height()) {
        updateScrollbars(m_scrollOffset);
        static_cast<FrameView*>(this)->setNeedsLayout();
    }

    geometryChanged();
}

void ScrollView::platformAddChild(Widget* child)
{
    if (!GTK_IS_SOCKET(child->platformWidget()))
        gtk_container_add(GTK_CONTAINER(containingWindow()), child->platformWidget());
}

void ScrollView::platformRemoveChild(Widget* child)
{
    if (GTK_WIDGET(containingWindow()) == GTK_WIDGET(child->platformWidget())->parent)
        gtk_container_remove(GTK_CONTAINER(containingWindow()), child->platformWidget());
}

bool ScrollView::inWindow() const
{
    notImplemented();
    return true;
}

void ScrollView::wheelEvent(PlatformWheelEvent& e)
{
    // Determine how much we want to scroll.  If we can move at all, we will accept the event.
    IntSize maxScrollDelta = maximumScrollPosition() - scrollPosition();
    if ((e.deltaX() < 0 && maxScrollDelta.width() > 0) ||
        (e.deltaX() > 0 && scrollOffset().width() > 0) ||
        (e.deltaY() < 0 && maxScrollDelta.height() > 0) ||
        (e.deltaY() > 0 && scrollOffset().height() > 0))
        e.accept();

    scrollBy(IntSize(-e.deltaX() * LINE_STEP, -e.deltaY() * LINE_STEP));
}

void ScrollView::updateScrollbars(const IntSize& desiredOffset)
{
    // Don't allow re-entrancy into this function.
    if (m_data->inUpdateScrollbars)
        return;

    // FIXME: This code is here so we don't have to fork FrameView.h/.cpp.
    // In the end, FrameView should just merge with ScrollView.
    if (static_cast<const FrameView*>(this)->frame()->prohibitsScrolling())
        return;

    m_data->inUpdateScrollbars = true;

    bool hasVerticalScrollbar = m_data->vBar;
    bool hasHorizontalScrollbar = m_data->hBar;
    bool oldHasVertical = hasVerticalScrollbar;
    bool oldHasHorizontal = hasHorizontalScrollbar;
    ScrollbarMode hScroll = m_data->hScrollbarMode;
    ScrollbarMode vScroll = m_data->vScrollbarMode;

    const int scrollbarThickness = ScrollbarTheme::nativeTheme()->scrollbarThickness();

    for (int pass = 0; pass < 2; pass++) {
        bool scrollsVertically;
        bool scrollsHorizontally;

        if (!m_data->scrollbarsSuppressed && (hScroll == ScrollbarAuto || vScroll == ScrollbarAuto)) {
            // Do a layout if pending before checking if scrollbars are needed.
            if (hasVerticalScrollbar != oldHasVertical || hasHorizontalScrollbar != oldHasHorizontal)
                static_cast<FrameView*>(this)->layout();

            scrollsVertically = (vScroll == ScrollbarAlwaysOn) || (vScroll == ScrollbarAuto && contentsHeight() > height());
            if (scrollsVertically)
                scrollsHorizontally = (hScroll == ScrollbarAlwaysOn) || (hScroll == ScrollbarAuto && contentsWidth() + scrollbarThickness > width());
            else {
                scrollsHorizontally = (hScroll == ScrollbarAlwaysOn) || (hScroll == ScrollbarAuto && contentsWidth() > width());
                if (scrollsHorizontally)
                    scrollsVertically = (vScroll == ScrollbarAlwaysOn) || (vScroll == ScrollbarAuto && contentsHeight() + scrollbarThickness > height());
            }
        }
        else {
            scrollsHorizontally = (hScroll == ScrollbarAuto) ? hasHorizontalScrollbar : (hScroll == ScrollbarAlwaysOn);
            scrollsVertically = (vScroll == ScrollbarAuto) ? hasVerticalScrollbar : (vScroll == ScrollbarAlwaysOn);
        }

        if (hasVerticalScrollbar != scrollsVertically) {
            m_data->setHasVerticalScrollbar(scrollsVertically);
            hasVerticalScrollbar = scrollsVertically;
        }

        if (hasHorizontalScrollbar != scrollsHorizontally) {
            m_data->setHasHorizontalScrollbar(scrollsHorizontally);
            hasHorizontalScrollbar = scrollsHorizontally;
        }
    }

    // Set up the range (and page step/line step).
    IntSize maxScrollPosition(contentsWidth() - visibleWidth(), contentsHeight() - visibleHeight());
    IntSize scroll = desiredOffset.shrunkTo(maxScrollPosition);
    scroll.clampNegativeToZero();

    if (m_data->horizontalAdjustment) {
        m_data->horizontalAdjustment->page_size = visibleWidth();
        m_data->horizontalAdjustment->step_increment = visibleWidth() / 10.0;
        m_data->horizontalAdjustment->page_increment = visibleWidth() * 0.9;
        m_data->horizontalAdjustment->lower = 0;
        m_data->horizontalAdjustment->upper = contentsWidth();
        gtk_adjustment_changed(m_data->horizontalAdjustment);

        if (m_scrollOffset.width() != scroll.width()) {
            m_data->horizontalAdjustment->value = scroll.width();
            gtk_adjustment_value_changed(m_data->horizontalAdjustment);
        }
    } else if (m_data->hBar) {
        int clientWidth = visibleWidth();
        m_data->hBar->setEnabled(contentsWidth() > clientWidth);
        int pageStep = (clientWidth - PAGE_KEEP);
        if (pageStep < 0) pageStep = clientWidth;
        IntRect oldRect(m_data->hBar->frameGeometry());
        IntRect hBarRect = IntRect(0,
                                   height() - m_data->hBar->height(),
                                   width() - (m_data->vBar ? m_data->vBar->width() : 0),
                                   m_data->hBar->height());
        m_data->hBar->setFrameGeometry(hBarRect);
        if (!m_data->scrollbarsSuppressed && oldRect != m_data->hBar->frameGeometry())
            m_data->hBar->invalidate();

        if (m_data->scrollbarsSuppressed)
            m_data->hBar->setSuppressInvalidation(true);
        m_data->hBar->setSteps(LINE_STEP, pageStep);
        m_data->hBar->setProportion(clientWidth, contentsWidth());
        m_data->hBar->setValue(scroll.width());
        if (m_data->scrollbarsSuppressed)
            m_data->hBar->setSuppressInvalidation(false);
    }

    if (m_data->verticalAdjustment) {
        m_data->verticalAdjustment->page_size = visibleHeight();
        m_data->verticalAdjustment->step_increment = visibleHeight() / 10.0;
        m_data->verticalAdjustment->page_increment = visibleHeight() * 0.9;
        m_data->verticalAdjustment->lower = 0;
        m_data->verticalAdjustment->upper = contentsHeight();
        gtk_adjustment_changed(m_data->verticalAdjustment);

        if (m_scrollOffset.height() != scroll.height()) {
            m_data->verticalAdjustment->value = scroll.height();
            gtk_adjustment_value_changed(m_data->verticalAdjustment);
        }
    } else if (m_data->vBar) {
        int clientHeight = visibleHeight();
        m_data->vBar->setEnabled(contentsHeight() > clientHeight);
        int pageStep = (clientHeight - PAGE_KEEP);
        if (pageStep < 0) pageStep = clientHeight;
        IntRect oldRect(m_data->vBar->frameGeometry());
        IntRect vBarRect = IntRect(width() - m_data->vBar->width(),
                                   0,
                                   m_data->vBar->width(),
                                   height() - (m_data->hBar ? m_data->hBar->height() : 0));
        m_data->vBar->setFrameGeometry(vBarRect);
        if (!m_data->scrollbarsSuppressed && oldRect != m_data->vBar->frameGeometry())
            m_data->vBar->invalidate();

        if (m_data->scrollbarsSuppressed)
            m_data->vBar->setSuppressInvalidation(true);
        m_data->vBar->setSteps(LINE_STEP, pageStep);
        m_data->vBar->setProportion(clientHeight, contentsHeight());
        m_data->vBar->setValue(scroll.height());
        if (m_data->scrollbarsSuppressed)
            m_data->vBar->setSuppressInvalidation(false);
    }

    if (oldHasVertical != (m_data->vBar != 0) || oldHasHorizontal != (m_data->hBar != 0))
        geometryChanged();

    // See if our offset has changed in a situation where we might not have scrollbars.
    // This can happen when editing a body with overflow:hidden and scrolling to reveal selection.
    // It can also happen when maximizing a window that has scrollbars (but the new maximized result
    // does not).
    IntSize scrollDelta = scroll - m_scrollOffset;
    if (scrollDelta != IntSize()) {
       m_scrollOffset = scroll;
       m_data->scrollBackingStore(scrollDelta);
    }

    m_data->inUpdateScrollbars = false;
}

IntPoint ScrollView::windowToContents(const IntPoint& windowPoint) const
{
    IntPoint viewPoint = convertFromContainingWindow(windowPoint);
    return viewPoint + scrollOffset();
}

IntPoint ScrollView::contentsToWindow(const IntPoint& contentsPoint) const
{
    IntPoint viewPoint = contentsPoint - scrollOffset();
    return convertToContainingWindow(viewPoint);
}

Scrollbar* ScrollView::scrollbarUnderMouse(const PlatformMouseEvent& mouseEvent)
{
    IntPoint viewPoint = convertFromContainingWindow(mouseEvent.pos());
    if (m_data->hBar && m_data->hBar->frameGeometry().contains(viewPoint))
        return m_data->hBar.get();
    if (m_data->vBar && m_data->vBar->frameGeometry().contains(viewPoint))
        return m_data->vBar.get();
    return 0;
}

bool ScrollView::isScrollViewScrollbar(const Widget* child) const
{
    return m_data->hBar == child || m_data->vBar == child;
}

void ScrollView::paint(GraphicsContext* context, const IntRect& rect)
{
    // FIXME: This code is here so we don't have to fork FrameView.h/.cpp.
    // In the end, FrameView should just merge with ScrollView.
    ASSERT(isFrameView());

    if (context->paintingDisabled())
        return;

    IntRect documentDirtyRect = rect;
    documentDirtyRect.intersect(frameGeometry());

    context->save();

    context->translate(x(), y());
    documentDirtyRect.move(-x(), -y());

    context->translate(-scrollX(), -scrollY());
    documentDirtyRect.move(scrollX(), scrollY());

    context->clip(enclosingIntRect(visibleContentRect()));
    static_cast<const FrameView*>(this)->frame()->paint(context, documentDirtyRect);
    context->restore();

    // Now paint the scrollbars.
    if (!m_data->scrollbarsSuppressed && (m_data->hBar || m_data->vBar)) {
        context->save();
        IntRect scrollViewDirtyRect = rect;
        scrollViewDirtyRect.intersect(frameGeometry());
        context->translate(x(), y());
        scrollViewDirtyRect.move(-x(), -y());
        if (m_data->hBar)
            m_data->hBar->paint(context, scrollViewDirtyRect);
        if (m_data->vBar)
            m_data->vBar->paint(context, scrollViewDirtyRect);

        /*
         * FIXME: TODO: Check if that works with RTL
         */
        // Fill the scroll corner with white.
        IntRect hCorner;
        if (m_data->hBar && width() - m_data->hBar->width() > 0) {
            hCorner = IntRect(m_data->hBar->width(),
                              height() - m_data->hBar->height(),
                              width() - m_data->hBar->width(),
                              m_data->hBar->height());
            if (hCorner.intersects(scrollViewDirtyRect))
                context->fillRect(hCorner, Color::white);
        }

        if (m_data->vBar && height() - m_data->vBar->height() > 0) {
            IntRect vCorner(width() - m_data->vBar->width(),
                            m_data->vBar->height(),
                            m_data->vBar->width(),
                            height() - m_data->vBar->height());
            if (vCorner != hCorner && vCorner.intersects(scrollViewDirtyRect))
                context->fillRect(vCorner, Color::white);
        }

        context->restore();
    }
}

/*
 * update children but nor our scrollbars. They should not scroll when
 * we scroll our content.
 */
void ScrollView::geometryChanged() const
{
    HashSet<Widget*>::const_iterator end = m_children.end();
    for (HashSet<Widget*>::const_iterator current = m_children.begin(); current != end; ++current)
        (*current)->geometryChanged();
}

bool ScrollView::scroll(ScrollDirection direction, ScrollGranularity granularity)
{
    if (direction == ScrollUp || direction == ScrollDown) {
        if (m_data->vBar)
            return m_data->vBar->scroll(direction, granularity);
    } else {
        if (m_data->hBar)
            return m_data->hBar->scroll(direction, granularity);
    }
    return false;
}

void ScrollView::addToDirtyRegion(const IntRect& containingWindowRect)
{
    ASSERT(isFrameView());
    const FrameView* frameView = static_cast<const FrameView*>(this);
    Page* page = frameView->frame() ? frameView->frame()->page() : 0;
    if (!page)
        return;
    page->chrome()->addToDirtyRegion(containingWindowRect);
}

void ScrollView::scrollBackingStore(int dx, int dy, const IntRect& scrollViewRect, const IntRect& clipRect)
{
    ASSERT(isFrameView());
    const FrameView* frameView = static_cast<const FrameView*>(this);
    Page* page = frameView->frame() ? frameView->frame()->page() : 0;
    if (!page)
        return;
    page->chrome()->scrollBackingStore(dx, dy, scrollViewRect, clipRect);
}

void ScrollView::updateBackingStore()
{
    ASSERT(isFrameView());
    const FrameView* frameView = static_cast<const FrameView*>(this);
    Page* page = frameView->frame() ? frameView->frame()->page() : 0;
    if (!page)
        return;
    page->chrome()->updateBackingStore();
}

}
