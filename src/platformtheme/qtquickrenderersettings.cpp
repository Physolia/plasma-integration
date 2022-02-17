/*
    SPDX-FileCopyrightText: 2016 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2020 Piotr Henryk Dabrowski <phd@phd.re>
    SPDX-FileCopyrightText: 2021 David Redondo <kde@david-redondo.de>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "renderersettings.h"

#include <QGuiApplication>
#include <QLibraryInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QVersionNumber>

/**
 * If QtQuick is configured (QQuickWindow::sceneGraphBackend()) to use the OpenGL backend,
 * check if it is supported or otherwise reconfigure QtQuick to fallback to software mode.
 * This function is called by init().
 *
 * @returns true if the selected backend is supported, false on fallback to software mode.
 */
static bool checkBackend(QOpenGLContext &checkContext)
{
    if (!QQuickWindow::sceneGraphBackend().isEmpty()) {
        return true; // QtQuick is not configured to use the OpenGL backend
    }

    // kwin wayland has it's own QPA, it is unable to create a GL context at this point.
    // KF6 TODO, drop this . The issue will be resolved in future kwin releases.
    QString platformName = qApp->platformName();
    if (platformName == QLatin1String("wayland-org.kde.kwin.qpa")) {
        return true;
    }

#ifdef QT_NO_OPENGL
    bool ok = false;
#else
    bool ok = checkContext.create();
#endif
    if (!ok) {
        qWarning("Warning: fallback to QtQuick software backend.");
        QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));
    }
    return ok;
}

void initializeRendererSessions()
{
    static bool firstCall = true; // Otherwise this gets called twice, see QTBUG-54479
    if (firstCall) {
        return;
    }
    firstCall = false;

    PlasmaQtQuickSettings::RendererSettings s;
    QOpenGLContext checkContext;
    if (!s.sceneGraphBackend().isEmpty()) {
        QQuickWindow::setSceneGraphBackend(s.sceneGraphBackend());
    } else {
        QQuickWindow::setSceneGraphBackend(QStringLiteral(""));
        checkBackend(checkContext);
    }

    if (!qEnvironmentVariableIsSet("QSG_RENDER_LOOP")) {
        if (!s.renderLoop().isEmpty()) {
            qputenv("QSG_RENDER_LOOP", s.renderLoop().toLatin1());
        }
    }
}

// Because this file gets loaded in the platform plugin, the QGuiApplication already exists
Q_COREAPP_STARTUP_FUNCTION(initializeRendererSessions)