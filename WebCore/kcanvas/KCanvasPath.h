/*
    Copyright (C) 2005 Apple Computer, Inc.
    Copyright (C) 2004, 2005 Nikolas Zimmermann <wildfox@kde.org>
                  2004, 2005 Rob Buis <buis@kde.org>

    This file is part of the KDE project

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    aint with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
    Boston, MA 02111-1307, USA.
*/

#ifndef KCanvasPath_H
#define KCanvasPath_H

#include <q3valuelist.h>
#include <kxmlcore/RefPtr.h>
#include <kxmlcore/Assertions.h>
#include "misc/shared.h"

class QTextStream;

typedef enum
{
    RULE_NONZERO = 0,
    RULE_EVENODD = 1
} KCWindRule;

QTextStream &operator<<(QTextStream &ts, KCWindRule rule);

// Path related data structures
typedef enum
{
    CMD_MOVE = 0,
    CMD_LINE = 1,
    CMD_CURVE = 2,
    CMD_CLOSE_SUBPATH = 3
} KCPathCommand;

class KCanvasPath : public khtml::Shared<KCanvasPath>
{
public:
    virtual ~KCanvasPath() { }
    
    virtual bool isEmpty() const = 0;

    virtual void moveTo(float x, float y) = 0;
    virtual void lineTo(float x, float y) = 0;
    virtual void curveTo(float x1, float y1, float x2, float y2, float x3, float y3) = 0;
    virtual void closeSubpath() = 0;
};

// Clipping paths
struct KCClipData
{
    KCWindRule windRule : 1;
    bool bboxUnits : 1;
    RefPtr<KCanvasPath> path;
};

QTextStream &operator<<(QTextStream &ts, const KCClipData &d);

class KCClipDataList : public Q3ValueList<KCClipData>
{
public:
    KCClipDataList() { }

    inline void addPath(KCanvasPath *pathData, KCWindRule windRule, bool bboxUnits)
    {
        ASSERT(pathData);
        KCClipData clipData;
        clipData.windRule = windRule;
        clipData.bboxUnits = bboxUnits;
        clipData.path = pathData;
        append(clipData);
    }
};

#endif

// vim:ts=4:noet
