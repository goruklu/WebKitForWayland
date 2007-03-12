/*
 * Copyright (C) 2006 Apple Computer, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#ifndef SearchPopupMenu_h
#define SearchPopupMenu_h

#include "PopupMenu.h"
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace WebCore {

class AtomicString;

class SearchPopupMenu : public PopupMenu {
public:
    static PassRefPtr<SearchPopupMenu> create(PopupMenuClient* client) { return new SearchPopupMenu(client); }

    void saveRecentSearches(const AtomicString& name, const Vector<String>& searchItems);
    void loadRecentSearches(const AtomicString& name, Vector<String>& searchItems);

    bool enabled();
    
protected:
    SearchPopupMenu(PopupMenuClient* client);

};

}

#endif
