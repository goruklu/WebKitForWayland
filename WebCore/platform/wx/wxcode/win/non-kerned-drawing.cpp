/*
 * Copyright (C) 2007 Kevin Watters, Kevin Ollivier.  All rights reserved.
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
#include "GlyphBuffer.h"
#include "GraphicsContext.h"
#include "SimpleFontData.h"

#include <wx/defs.h>
#include <wx/dcclient.h>
#include <wx/gdicmn.h>
#include <vector>

using namespace std;

//-----------------------------------------------------------------------------
// constants
//-----------------------------------------------------------------------------

const double RAD2DEG = 180.0 / M_PI;

//-----------------------------------------------------------------------------
// Local functions
//-----------------------------------------------------------------------------

static inline double dmin(double a, double b) { return a < b ? a : b; }
static inline double dmax(double a, double b) { return a > b ? a : b; }

static inline double DegToRad(double deg) { return (deg * M_PI) / 180.0; }
static inline double RadToDeg(double deg) { return (deg * 180.0) / M_PI; }

#include "wx/msw/private.h"

// TODO remove this dependency (gdiplus needs the macros)

#ifndef max
#define max(a,b)            (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a,b)            (((a) < (b)) ? (a) : (b))
#endif

#include "gdiplus.h"
 

namespace WebCore {

void drawTextWithSpacing(GraphicsContext* graphicsContext, const SimpleFontData* font, const wxColour& color, const GlyphBuffer& glyphBuffer, int from, int numGlyphs, const FloatPoint& point)
{
#if USE(WXGC)
    wxGCDC* dc = static_cast<wxGCDC*>(graphicsContext->platformContext());
#else
    wxDC* dc = graphicsContext->platformContext();
#endif

    // get the native HDC handle to draw using native APIs
    HDC hdc = 0;
#if USE(WXGC)
    wxGraphicsContext* gc = dc->GetGraphicsContext();
    Gdiplus::Graphics* g;
    if (gc) {
        g = (Gdiplus::Graphics*)gc->GetNativeContext();
        hdc = g->GetHDC();
    }
#else
    hdc = static_cast<HDC>(dc->GetHDC());
#endif

    // ExtTextOut wants the offsets as an array of ints, so extract them
    // from the glyph buffer
    const GlyphBufferGlyph*   glyphs   = glyphBuffer.glyphs(from);
    const GlyphBufferAdvance* advances = glyphBuffer.advances(from);

    float y = point.y() - font->ascent();
    float x = point.x();

    int* spacing = new int[numGlyphs - from];
    for (unsigned i = 0; i < numGlyphs; ++i)
        spacing[i] = advances[i].width();

    ::SelectObject(hdc, GetHfontOf(font->getWxFont()));

    if (color.Ok())
        ::SetTextColor(hdc, color.GetPixel());

    // do not draw background behind characters
    ::SetBkMode(hdc, TRANSPARENT);

    // draw text with optional character widths array
    wxString string = wxString((wxChar*)(&glyphs[from]), numGlyphs);
    ::ExtTextOut(hdc, x, y, 0, NULL, string.c_str(), string.length(), spacing);

    ::SetBkMode(hdc, TRANSPARENT);

    #if USE(WXGC)
        g->ReleaseHDC(hdc);
    #endif

    delete [] spacing;
}

}
