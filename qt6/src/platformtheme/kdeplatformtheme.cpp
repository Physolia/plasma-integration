/*  This file is part of the KDE libraries
    SPDX-FileCopyrightText: 2013 Kevin Ottens <ervin+bluesystems@kde.org>
    SPDX-FileCopyrightText: 2013 Aleix Pol Gonzalez <aleixpol@blue-systems.com>
    SPDX-FileCopyrightText: 2014 Lukáš Tinkl <ltinkl@redhat.com>
    SPDX-FileCopyrightText: 2022 Harald Sitter <sitter@kde.org>

    SPDX-License-Identifier: LGPL-2.0-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <config-platformtheme.h>

#include "kdeplatformfiledialoghelper.h"
#include "kdeplatformfontdialoghelper.h"
#include "kdeplatformsystemtrayicon.h"
#include "kdeplatformtheme.h"
#include "kfontsettingsdata.h"
#include "khintssettings.h"
#include "kwaylandintegration.h"
#include "qdbusmenubarwrapper.h"
#include "x11integration.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDebug>
#include <QFont>
#include <QPalette>
#include <QString>
#include <QVariant>
#include <QtQuickControls2/QQuickStyle>
#include <private/qgenericunixservices_p.h>
#include <private/qguiapplication_p.h>
#include <qpa/qplatformintegration.h>

#include <KIO/Global>
#include <KIO/JobUiDelegate>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenWithHandlerInterface>
#include <KJobWidgets>
#include <KLocalizedString>
#include <KStandardGuiItem>
#include <KWindowSystem>
#include <kiconengine.h>
#include <kiconloader.h>
#include <kstandardshortcut.h>

#include "qxdgdesktopportalfiledialog_p.h"

#include <KCountryFlagEmojiIconEngine>

using namespace Qt::StringLiterals;

static const QByteArray s_x11AppMenuServiceNamePropertyName = QByteArrayLiteral("_KDE_NET_WM_APPMENU_SERVICE_NAME");
static const QByteArray s_x11AppMenuObjectPathPropertyName = QByteArrayLiteral("_KDE_NET_WM_APPMENU_OBJECT_PATH");

static bool checkDBusGlobalMenuAvailable()
{
    if (qEnvironmentVariableIsSet("KDE_NO_GLOBAL_MENU")) {
        return false;
    }

    QDBusConnection connection = QDBusConnection::sessionBus();
    QString registrarService = QStringLiteral("com.canonical.AppMenu.Registrar");
    return connection.interface()->isServiceRegistered(registrarService);
}

static bool isDBusGlobalMenuAvailable()
{
    static bool dbusGlobalMenuAvailable = checkDBusGlobalMenuAvailable();
    return dbusGlobalMenuAvailable;
}

static QString desktopPortalService()
{
    return QStringLiteral("org.freedesktop.impl.portal.desktop.kde");
}

static QString desktopPortalPath()
{
    return QStringLiteral("/org/freedesktop/portal/desktop");
}

class KIOOpenWith : public KIO::OpenWithHandlerInterface
{
    Q_OBJECT
public:
    explicit KIOOpenWith(QWidget *parentWidget, QObject *parent = nullptr)
        : KIO::OpenWithHandlerInterface(parent)
        , m_parentWidget(parentWidget)
    {
    }

    void promptUserForApplication(KJob *job, const QList<QUrl> &urls, const QString &mimeType) override
    {
        Q_UNUSED(mimeType);

        QWidget *widget = nullptr;
        if (job) {
            widget = KJobWidgets::window(job);
        }

        if (!widget) {
            widget = m_parentWidget;
        }

        QString windowId;
        if (widget) {
            widget->winId(); // ensure we have a handle so we can export a window (without this windowHandle() may be null)
            auto services = QGuiApplicationPrivate::platformIntegration()->services();
            if (auto unixServices = dynamic_cast<QGenericUnixServices *>(services)) {
                windowId = unixServices->portalWindowIdentifier(widget->windowHandle());
            }
        }

        QDBusMessage message = QDBusMessage::createMethodCall(desktopPortalService(),
                                                              desktopPortalPath(),
                                                              QStringLiteral("org.freedesktop.impl.portal.AppChooser"),
                                                              QStringLiteral("ChooseApplicationPrivate"));

        QStringList urlStrings;
        for (const auto &url : urls) {
            urlStrings << url.toString();
        }
        message << windowId << urlStrings << QVariantMap{{QStringLiteral("ask"), true}};

        QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(message, std::numeric_limits<int>::max());
        auto watcher = new QDBusPendingCallWatcher(pendingCall, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
            watcher->deleteLater();

            QDBusPendingReply<uint, QVariantMap> reply = *watcher;
            if (reply.isError()) {
                qWarning() << "Couldn't get reply";
                qWarning() << "Error: " << reply.error().message();
                Q_EMIT canceled();
            } else {
                if (reply.argumentAt<0>() == 0) {
                    Q_EMIT serviceSelected(KService::serviceByDesktopName(reply.argumentAt<1>().value(QStringLiteral("choice")).toString()));
                } else {
                    Q_EMIT canceled();
                }
            }
        });
    }

private:
    QWidget *const m_parentWidget;
};

class KIOUiDelegate : public KIO::JobUiDelegate
{
public:
    explicit KIOUiDelegate(KJobUiDelegate::Flags flags = AutoHandlingDisabled, QWidget *window = nullptr)
        : KIO::JobUiDelegate(flags, window, {new KIOOpenWith(window, nullptr)})
    {
    }
};

class KIOUiFactory : public KIO::JobUiDelegateFactory
{
public:
    KIOUiFactory() = default;

    KJobUiDelegate *createDelegate() const override
    {
        return new KIOUiDelegate;
    }

    KJobUiDelegate *createDelegate(KJobUiDelegate::Flags flags, QWidget *window) const override
    {
        return new KIOUiDelegate(flags, window);
    }
};

KdePlatformTheme::KdePlatformTheme()
{
    loadSettings();

    // explicitly not KWindowSystem::isPlatformWayland to not include the kwin process
    if (QGuiApplication::platformName() == QLatin1String("wayland")) {
        m_kwaylandIntegration.reset(new KWaylandIntegration(this));
    }

#if HAVE_X11
    if (KWindowSystem::isPlatformX11()) {
        m_x11Integration.reset(new X11Integration(this));
        m_x11Integration->init();
    }
#endif

    QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar, false);
    setQtQuickControlsTheme();

    static KIOUiFactory factory;
    KIO::setDefaultJobUiDelegateFactory(&factory);

    static KIOUiDelegate delegateExtension;
    KIO::setDefaultJobUiDelegateExtension(&delegateExtension);

    KCountryFlagEmojiIconEngine::setGlobalDefaultFont(QFont(u"Noto Color Emoji, emoji"_qs));
}

KdePlatformTheme::~KdePlatformTheme()
{
    delete m_fontsData;
    delete m_hints;
}

QVariant KdePlatformTheme::themeHint(QPlatformTheme::ThemeHint hintType) const
{
    QVariant hint = m_hints->hint(hintType);
    if (hint.isValid()) {
        return hint;
    } else {
        return QPlatformTheme::themeHint(hintType);
    }
}

QIcon KdePlatformTheme::fileIcon(const QFileInfo &fileInfo, QPlatformTheme::IconOptions iconOptions) const
{
    if (iconOptions.testFlag(DontUseCustomDirectoryIcons) && fileInfo.isDir()) {
        return QIcon::fromTheme(QLatin1String("inode-directory"));
    }

    return QIcon::fromTheme(KIO::iconNameForUrl(QUrl::fromLocalFile(fileInfo.absoluteFilePath())));
}

const QPalette *KdePlatformTheme::palette(Palette type) const
{
    QPalette *palette = m_hints->palette(type);
    if (palette) {
        return palette;
    } else {
        return QPlatformTheme::palette(type);
    }
}

const QFont *KdePlatformTheme::font(Font type) const
{
    KFontSettingsData::FontTypes fdtype;
    switch (type) {
    case SystemFont:
        fdtype = KFontSettingsData::GeneralFont;
        break;
    case MenuFont:
    case MenuBarFont:
    case MenuItemFont:
        fdtype = KFontSettingsData::MenuFont;
        break;
    case MessageBoxFont:
    case LabelFont:
    case TipLabelFont:
    case StatusBarFont:
    case PushButtonFont:
    case ItemViewFont:
    case ListViewFont:
    case HeaderViewFont:
    case ListBoxFont:
    case ComboMenuItemFont:
    case ComboLineEditFont:
        fdtype = KFontSettingsData::GeneralFont;
        break;
    case TitleBarFont:
    case MdiSubWindowTitleFont:
    case DockWidgetTitleFont:
        fdtype = KFontSettingsData::WindowTitleFont;
        break;
    case SmallFont:
    case MiniFont:
        fdtype = KFontSettingsData::SmallestReadableFont;
        break;
    case FixedFont:
        fdtype = KFontSettingsData::FixedFont;
        break;
    case ToolButtonFont:
        fdtype = KFontSettingsData::ToolbarFont;
        break;
    default:
        fdtype = KFontSettingsData::GeneralFont;
        break;
    }

    return m_fontsData->font(fdtype);
}

QIconEngine *KdePlatformTheme::createIconEngine(const QString &iconName) const
{
    return new KIconEngine(iconName, KIconLoader::global());
}

void KdePlatformTheme::loadSettings()
{
    m_fontsData = new KFontSettingsData;
    m_hints = new KHintsSettings;
}

QList<QKeySequence> KdePlatformTheme::keyBindings(QKeySequence::StandardKey key) const
{
    switch (key) {
    case QKeySequence::HelpContents:
        return KStandardShortcut::shortcut(KStandardShortcut::Help);
    case QKeySequence::WhatsThis:
        return KStandardShortcut::shortcut(KStandardShortcut::WhatsThis);
    case QKeySequence::Open:
        return KStandardShortcut::shortcut(KStandardShortcut::Open);
    case QKeySequence::Close:
        return KStandardShortcut::shortcut(KStandardShortcut::Close);
    case QKeySequence::Save:
        return KStandardShortcut::shortcut(KStandardShortcut::Save);
    case QKeySequence::New:
        return KStandardShortcut::shortcut(KStandardShortcut::New);
    case QKeySequence::Cut:
        return KStandardShortcut::shortcut(KStandardShortcut::Cut);
    case QKeySequence::Copy:
        return KStandardShortcut::shortcut(KStandardShortcut::Copy);
    case QKeySequence::Paste:
        return KStandardShortcut::shortcut(KStandardShortcut::Paste);
    case QKeySequence::Undo:
        return KStandardShortcut::shortcut(KStandardShortcut::Undo);
    case QKeySequence::Redo:
        return KStandardShortcut::shortcut(KStandardShortcut::Redo);
    case QKeySequence::Back:
        return KStandardShortcut::shortcut(KStandardShortcut::Back);
    case QKeySequence::Forward:
        return KStandardShortcut::shortcut(KStandardShortcut::Forward);
    case QKeySequence::Refresh:
        return KStandardShortcut::shortcut(KStandardShortcut::Reload);
    case QKeySequence::ZoomIn:
        return KStandardShortcut::shortcut(KStandardShortcut::ZoomIn);
    case QKeySequence::ZoomOut:
        return KStandardShortcut::shortcut(KStandardShortcut::ZoomOut);
    case QKeySequence::Print:
        return KStandardShortcut::shortcut(KStandardShortcut::Print);
    case QKeySequence::Find:
        return KStandardShortcut::shortcut(KStandardShortcut::Find);
    case QKeySequence::FindNext:
        return KStandardShortcut::shortcut(KStandardShortcut::FindNext);
    case QKeySequence::FindPrevious:
        return KStandardShortcut::shortcut(KStandardShortcut::FindPrev);
    case QKeySequence::Replace:
        return KStandardShortcut::shortcut(KStandardShortcut::Replace);
    case QKeySequence::SelectAll:
        return KStandardShortcut::shortcut(KStandardShortcut::SelectAll);
    case QKeySequence::MoveToNextWord:
        return KStandardShortcut::shortcut(KStandardShortcut::ForwardWord);
    case QKeySequence::MoveToPreviousWord:
        return KStandardShortcut::shortcut(KStandardShortcut::BackwardWord);
    case QKeySequence::MoveToNextPage:
        return KStandardShortcut::shortcut(KStandardShortcut::Next);
    case QKeySequence::MoveToPreviousPage:
        return KStandardShortcut::shortcut(KStandardShortcut::Prior);
    case QKeySequence::MoveToStartOfLine:
        return KStandardShortcut::shortcut(KStandardShortcut::BeginningOfLine);
    case QKeySequence::MoveToEndOfLine:
        return KStandardShortcut::shortcut(KStandardShortcut::EndOfLine);
    case QKeySequence::MoveToStartOfDocument:
        return KStandardShortcut::shortcut(KStandardShortcut::Begin);
    case QKeySequence::MoveToEndOfDocument:
        return KStandardShortcut::shortcut(KStandardShortcut::End);
    case QKeySequence::SaveAs:
        return KStandardShortcut::shortcut(KStandardShortcut::SaveAs);
    case QKeySequence::Preferences:
        return KStandardShortcut::shortcut(KStandardShortcut::Preferences);
    case QKeySequence::Quit:
        return KStandardShortcut::shortcut(KStandardShortcut::Quit);
    case QKeySequence::FullScreen:
        return KStandardShortcut::shortcut(KStandardShortcut::FullScreen);
    case QKeySequence::Deselect:
        return KStandardShortcut::shortcut(KStandardShortcut::Deselect);
    case QKeySequence::DeleteStartOfWord:
        return KStandardShortcut::shortcut(KStandardShortcut::DeleteWordBack);
    case QKeySequence::DeleteEndOfWord:
        return KStandardShortcut::shortcut(KStandardShortcut::DeleteWordForward);
    case QKeySequence::NextChild:
        return KStandardShortcut::shortcut(KStandardShortcut::TabNext);
    case QKeySequence::PreviousChild:
        return KStandardShortcut::shortcut(KStandardShortcut::TabPrev);
    case QKeySequence::Delete:
        return KStandardShortcut::shortcut(KStandardShortcut::MoveToTrash);
    default:
        return QPlatformTheme::keyBindings(key);
    }
}

bool KdePlatformTheme::usePlatformNativeDialog(QPlatformTheme::DialogType type) const
{
    return (type == QPlatformTheme::FileDialog || type == QPlatformTheme::FontDialog) && qobject_cast<QApplication *>(QCoreApplication::instance());
}

QString KdePlatformTheme::standardButtonText(int button) const
{
    switch (static_cast<QPlatformDialogHelper::StandardButton>(button)) {
    case QPlatformDialogHelper::NoButton:
        qWarning() << Q_FUNC_INFO << "Unsupported standard button:" << button;
        return QString();
    case QPlatformDialogHelper::Ok:
        return KStandardGuiItem::ok().text();
    case QPlatformDialogHelper::Save:
        return KStandardGuiItem::save().text();
    case QPlatformDialogHelper::SaveAll:
        return i18nc("@action:button", "Save All");
    case QPlatformDialogHelper::Open:
        return KStandardGuiItem::open().text();
    case QPlatformDialogHelper::Yes:
        return i18nc("@action:button", "&Yes");
    case QPlatformDialogHelper::YesToAll:
        return i18nc("@action:button", "Yes to All");
    case QPlatformDialogHelper::No:
        return i18nc("@action:button", "&No");
    case QPlatformDialogHelper::NoToAll:
        return i18nc("@action:button", "No to All");
    case QPlatformDialogHelper::Abort:
        // FIXME KStandardGuiItem::stop() doesn't seem right here
        return i18nc("@action:button", "Abort");
    case QPlatformDialogHelper::Retry:
        return i18nc("@action:button", "Retry");
    case QPlatformDialogHelper::Ignore:
        return i18nc("@action:button", "Ignore");
    case QPlatformDialogHelper::Close:
        return KStandardGuiItem::close().text();
    case QPlatformDialogHelper::Cancel:
        return KStandardGuiItem::cancel().text();
    case QPlatformDialogHelper::Discard:
        return KStandardGuiItem::discard().text();
    case QPlatformDialogHelper::Help:
        return KStandardGuiItem::help().text();
    case QPlatformDialogHelper::Apply:
        return KStandardGuiItem::apply().text();
    case QPlatformDialogHelper::Reset:
        return KStandardGuiItem::reset().text();
    case QPlatformDialogHelper::RestoreDefaults:
        return KStandardGuiItem::defaults().text();
    default:
        return QPlatformTheme::defaultStandardButtonText(button);
    }
}

QPlatformDialogHelper *KdePlatformTheme::createPlatformDialogHelper(QPlatformTheme::DialogType type) const
{
    switch (type) {
    case QPlatformTheme::FileDialog:
        if (useXdgDesktopPortal()) {
            return new QXdgDesktopPortalFileDialog;
        }
        return new KDEPlatformFileDialogHelper;
    case QPlatformTheme::FontDialog:
        return new KDEPlatformFontDialogHelper;
    case QPlatformTheme::ColorDialog:
    case QPlatformTheme::MessageDialog:
    default:
        return nullptr;
    }
}

QPlatformSystemTrayIcon *KdePlatformTheme::createPlatformSystemTrayIcon() const
{
    return new KDEPlatformSystemTrayIcon;
}

QPlatformMenuBar *KdePlatformTheme::createPlatformMenuBar() const
{
    if (isDBusGlobalMenuAvailable()) {
        auto dbusMenu = QGenericUnixTheme::createPlatformMenuBar();
        if (!dbusMenu) {
            return nullptr;
        }
        auto menu = new QDBusMenuBarWrapper(dbusMenu);

        QObject::connect(menu, &QDBusMenuBarWrapper::windowChanged, menu, [this, menu](QWindow *newWindow, QWindow *oldWindow) {
            const QString &serviceName = QDBusConnection::sessionBus().baseService();
            const QString &objectPath = menu->objectPath();

            setMenuBarForWindow(oldWindow, {}, {});
            setMenuBarForWindow(newWindow, serviceName, objectPath);
        });

        return menu;
    }
    return nullptr;
}

// force QtQuickControls2 to use the desktop theme as default
void KdePlatformTheme::setQtQuickControlsTheme()
{
    // if the user is running only a QGuiApplication, explicitly unset the QQC1 desktop style and abort
    // as this style is all about QWidgets and we know setting this will make it crash
    if (!qobject_cast<QApplication *>(qApp)) {
        if (qgetenv("QT_QUICK_CONTROLS_1_STYLE").right(7) == "Desktop") {
            qunsetenv("QT_QUICK_CONTROLS_1_STYLE");
        }
        return;
    }
    // if the user has explicitly set something else, don't meddle
    // Also ignore the default Fusion style
    if (!QQuickStyle::name().isEmpty() && QQuickStyle::name() != QLatin1String("Fusion")) {
        return;
    }
    QQuickStyle::setStyle(QLatin1String("org.kde.desktop"));
}

bool KdePlatformTheme::useXdgDesktopPortal()
{
    static bool usePortal = qEnvironmentVariableIntValue("PLASMA_INTEGRATION_USE_PORTAL") == 1;
    return usePortal;
}

inline bool windowRelevantForGlobalMenu(QWindow *window)
{
    return !(window->type() & Qt::WindowType::Popup);
}

void KdePlatformTheme::setMenuBarForWindow(QWindow *window, const QString &serviceName, const QString &objectPath) const
{
    if (!window)
        return;

    if (m_x11Integration) {
        m_x11Integration->setWindowProperty(window, s_x11AppMenuServiceNamePropertyName, serviceName.toUtf8());
        m_x11Integration->setWindowProperty(window, s_x11AppMenuObjectPathPropertyName, objectPath.toUtf8());
    }

    if (m_kwaylandIntegration) {
        m_kwaylandIntegration->setAppMenu(window, serviceName, objectPath);
    }
}
#include "kdeplatformtheme.moc"
