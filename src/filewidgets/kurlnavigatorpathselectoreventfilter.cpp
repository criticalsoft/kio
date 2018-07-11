/*
 * Copyright 2018 Kai Uwe Broulik <kde@privat.broulik.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3, or any
 * later version accepted by the membership of KDE e.V. (or its
 * successor approved by the membership of KDE e.V.), which shall
 * act as a proxy defined in Section 6 of version 3 of the license.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "kurlnavigatorpathselectoreventfilter_p.h"

#include <QEvent>
#include <QMenu>
#include <QMouseEvent>

using namespace KDEPrivate;

KUrlNavigatorPathSelectorEventFilter::KUrlNavigatorPathSelectorEventFilter(QObject *parent)
    : QObject(parent)
{

}

KUrlNavigatorPathSelectorEventFilter::~KUrlNavigatorPathSelectorEventFilter()
{

}

bool KUrlNavigatorPathSelectorEventFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::MiddleButton) {
            if (QMenu *menu = qobject_cast<QMenu *>(watched)) {
                if (QAction *action = menu->activeAction()) {
                    const QUrl url(action->data().toString());
                    if (url.isValid()) {
                        menu->close();

                        emit tabRequested(url);
                        return true;
                    }
                }
            }
        }
    }

    return QObject::eventFilter(watched, event);
}

#include "moc_kurlnavigatorpathselectoreventfilter_p.cpp"