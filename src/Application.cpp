/*
    SPDX-FileCopyrightText: 2006-2008 Robert Knight <robertknight@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "Application.h"

// Qt
#include <QApplication>
#include <QHashIterator>
#include <QFileInfo>
#include <QDir>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QTimer>
#include <QDebug>
// KDE
#include <KActionCollection>
#include <KGlobalAccel>
#include <KLocalizedString>

// Konsole
#include "KonsoleSettings.h"
#include "MainWindow.h"
#include "ShellCommand.h"
#include "ViewManager.h"
#include "WindowSystemInfo.h"
#include "profile/ProfileManager.h"
#include "profile/ProfileCommandParser.h"
#include "session/Session.h"
#include "session/SessionManager.h"
#include "terminalDisplay/TerminalDisplay.h"
#include "widgets/ViewContainer.h"

using namespace Konsole;

Application::Application(QSharedPointer<QCommandLineParser> parser,
                         const QStringList &customCommand) :
    _backgroundInstance(nullptr),
    m_parser(parser),
    m_customCommand(customCommand)
{
}

void Application::populateCommandLineParser(QCommandLineParser *parser)
{
    const auto options = QVector<QCommandLineOption> {
        { { QStringLiteral("profile") },
            i18nc("@info:shell", "Name of profile to use for new Konsole instance"),
            QStringLiteral("name")
        },
        { { QStringLiteral("fallback-profile") },
            i18nc("@info:shell", "Use the internal FALLBACK profile")
        },
        { { QStringLiteral("workdir") },
            i18nc("@info:shell", "Set the initial working directory of the new tab or window to 'dir'"),
            QStringLiteral("dir")
        },
        { { QStringLiteral("hold"), QStringLiteral("noclose") },
           i18nc("@info:shell", "Do not close the initial session automatically when it ends.")
        },
        {  {QStringLiteral("new-tab") },
            i18nc("@info:shell", "Create a new tab in an existing window rather than creating a new window")
        },
        { { QStringLiteral("tabs-from-file") },
            i18nc("@info:shell","Create tabs as specified in given tabs configuration"" file"),
            QStringLiteral("file")
        },
        { { QStringLiteral("background-mode") },
            i18nc("@info:shell", "Start Konsole in the background and bring to the front when Ctrl+Shift+F12 (by default) is pressed")
        },
        { { QStringLiteral("separate"), QStringLiteral("nofork") },
            i18nc("@info:shell", "Run in a separate process")
        },
        { { QStringLiteral("show-menubar") },
            i18nc("@info:shell", "Show the menubar, overriding the default setting")
        },
        { { QStringLiteral("hide-menubar") },
            i18nc("@info:shell", "Hide the menubar, overriding the default setting")
        },
        { { QStringLiteral("show-tabbar") },
            i18nc("@info:shell", "Show the tabbar, overriding the default setting")
        },
        { { QStringLiteral("hide-tabbar") },
            i18nc("@info:shell", "Hide the tabbar, overriding the default setting")
        },
        { { QStringLiteral("fullscreen") },
            i18nc("@info:shell", "Start Konsole in fullscreen mode")
        },
        { { QStringLiteral("notransparency") },
            i18nc("@info:shell", "Disable transparent backgrounds, even if the system supports them.")
        },
        { { QStringLiteral("list-profiles") },
            i18nc("@info:shell", "List the available profiles")
        },
        { { QStringLiteral("list-profile-properties") },
            i18nc("@info:shell", "List all the profile properties names and their type (for use with -p)")
        },
        { { QStringLiteral("p") },
            i18nc("@info:shell", "Change the value of a profile property."),
            QStringLiteral("property=value")
        },
        { { QStringLiteral("e") },
            i18nc("@info:shell", "Command to execute. This option will catch all following arguments, so use it as the last option."),
            QStringLiteral("cmd")
        }
    };
    for (const auto &option : options) {
        parser->addOption(option);
    }

    parser->addPositionalArgument(QStringLiteral("[args]"),
                                  i18nc("@info:shell", "Arguments passed to command"));

    // Add a no-op compatibility option to make Konsole compatible with
    // Debian's policy on X terminal emulators.
    // -T is technically meant to set a title, that is not really meaningful
    // for Konsole as we have multiple user-facing options controlling
    // the title and overriding whatever is set elsewhere.
    // https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=532029
    // https://www.debian.org/doc/debian-policy/ch-customized-programs.html#s11.8.3
    auto titleOption = QCommandLineOption({ QStringLiteral("T") },
                                          QStringLiteral("Debian policy compatibility, not used"),
                                          QStringLiteral("value"));
    titleOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser->addOption(titleOption);
}

QStringList Application::getCustomCommand(QStringList &args)
{
    int i = args.indexOf(QStringLiteral("-e"));
    QStringList customCommand;
    if ((0 < i) && (i < (args.size() - 1))) {
        // -e was specified with at least one extra argument
        // if -e was specified without arguments, QCommandLineParser will deal
        // with that
        args.removeAt(i);
        while (args.size() > i) {
            customCommand << args.takeAt(i);
        }
    }
    return customCommand;
}

Application::~Application()
{
    SessionManager::instance()->closeAllSessions();
    ProfileManager::instance()->saveSettings();
}

MainWindow *Application::newMainWindow()
{
    WindowSystemInfo::HAVE_TRANSPARENCY = !m_parser->isSet(QStringLiteral("notransparency"));

    auto window = new MainWindow();

    connect(window, &Konsole::MainWindow::newWindowRequest, this,
            &Konsole::Application::createWindow);
    connect(window, &Konsole::MainWindow::terminalsDetached, this, &Konsole::Application::detachTerminals);

    return window;
}

void Application::createWindow(const Profile::Ptr &profile, const QString &directory)
{
    qDebug() << Q_FUNC_INFO << " profile " << profile << "  directory is " << directory;
    MainWindow *window = newMainWindow();
    window->createSession(profile, directory);
    window->show();
}

void Application::detachTerminals(ViewSplitter *splitter,const QHash<TerminalDisplay*, Session*>& sessionsMap)
{
    auto *currentWindow = qobject_cast<MainWindow*>(sender());
    MainWindow *window = newMainWindow();
    ViewManager *manager = window->viewManager();

    const QList<TerminalDisplay *> displays = splitter->findChildren<TerminalDisplay *>();
    for (TerminalDisplay* terminal : displays) {
        manager->attachView(terminal, sessionsMap[terminal]);
    }
    manager->activeContainer()->addSplitter(splitter);

    window->show();
    window->resize(currentWindow->width(), currentWindow->height());
    window->move(QCursor::pos());
}

int Application::newInstance()
{
    // handle session management

    // returns from processWindowArgs(args, createdNewMainWindow)
    // if a new window was created
    bool createdNewMainWindow = false;

    // check for arguments to print help or other information to the
    // terminal, quit if such an argument was found
    if (processHelpArgs()) {
        return 0;
    }


    qDebug() << Q_FUNC_INFO;
    // create a new window or use an existing one
    MainWindow *window = processWindowArgs(createdNewMainWindow);

    qDebug() << Q_FUNC_INFO << " window is " << window;
    if (m_parser->isSet(QStringLiteral("tabs-from-file"))) {
        // create new session(s) as described in file
        if (!processTabsFromFileArgs(window)) {
            return 0;
        }
    }

    // select profile to use
    Profile::Ptr baseProfile = processProfileSelectArgs();

    // process various command-line options which cause a property of the
    // selected profile to be changed
    Profile::Ptr newProfile = processProfileChangeArgs(baseProfile);

    // create new session
    Session *session = window->createSession(newProfile, QString());

    if (m_parser->isSet(QStringLiteral("noclose"))) {
        session->setAutoClose(false);
    }

    // if the background-mode argument is supplied, start the background
    // session ( or bring to the front if it already exists )
    if (m_parser->isSet(QStringLiteral("background-mode"))) {
        startBackgroundMode(window);
    } else {
        // Qt constrains top-level windows which have not been manually
        // resized (via QWidget::resize()) to a maximum of 2/3rds of the
        //  screen size.
        //
        // This means that the terminal display might not get the width/
        // height it asks for.  To work around this, the widget must be
        // manually resized to its sizeHint().
        //
        // This problem only affects the first time the application is run.
        // run. After that KMainWindow will have manually resized the
        // window to its saved size at this point (so the Qt::WA_Resized
        // attribute will be set)

        // If not restoring size from last time or only adding new tab,
        // resize window to chosen profile size (see Bug:345403)
        if (createdNewMainWindow) {
            qDebug() << Q_FUNC_INFO << "show window ";
            QTimer::singleShot(0, window, &MainWindow::show);
        } else {
            window->setWindowState(window->windowState() & (~Qt::WindowMinimized | Qt::WindowActive));
            window->show();
            window->activateWindow();
        }
    }

    return 1;
}

/* Documentation for tab file:
 *
 * ;; is the token separator
 * # at the beginning of line results in line being ignored.
 * supported tokens: title, command, profile and workdir
 *
 * Note that the title is static and the tab will close when the
 * command is complete (do not use --noclose).  You can start new tabs.
 *
 * Example below will create 6 tabs as listed and a 7th default tab
title: This is the title;; command: ssh localhost
title: This is the title;; command: ssh localhost;; profile: Shell
title: Top this!;; command: top
title: mc this!;; command: mc;; workdir: /tmp
#this line is comment
command: ssh localhost
profile: Shell
*/
bool Application::processTabsFromFileArgs(MainWindow *window)
{
    // Open tab configuration file
    const QString tabsFileName(m_parser->value(QStringLiteral("tabs-from-file")));
    QFile tabsFile(tabsFileName);
    if (!tabsFile.open(QFile::ReadOnly)) {
        qWarning() << "ERROR: Cannot open tabs file "
                   << tabsFileName.toLocal8Bit().data();
        return false;
    }

    unsigned int sessions = 0;
    while (!tabsFile.atEnd()) {
        QString lineString(QString::fromUtf8(tabsFile.readLine()).trimmed());
        if ((lineString.isEmpty()) || (lineString[0] == QLatin1Char('#'))) {
            continue;
        }

        QHash<QString, QString> lineTokens;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
        QStringList lineParts = lineString.split(QStringLiteral(";;"), Qt::SkipEmptyParts);
#else
        QStringList lineParts = lineString.split(QStringLiteral(";;"), QString::SkipEmptyParts);
#endif

        for (int i = 0; i < lineParts.size(); ++i) {
            QString key = lineParts.at(i).section(QLatin1Char(':'), 0, 0).trimmed().toLower();
            QString value = lineParts.at(i).section(QLatin1Char(':'), 1, -1).trimmed();
            lineTokens[key] = value;
        }
        // should contain at least one of 'command' and 'profile'
        if (lineTokens.contains(QStringLiteral("command"))
            || lineTokens.contains(QStringLiteral("profile"))) {
            createTabFromArgs(window, lineTokens);
            sessions++;
        } else {
            qWarning() << "Each line should contain at least one of 'command' and 'profile'.";
        }
    }
    tabsFile.close();

    if (sessions < 1) {
        qWarning() << "No valid lines found in "
                   << tabsFileName.toLocal8Bit().data();
        return false;
    }

    return true;
}

void Application::createTabFromArgs(MainWindow *window, const QHash<QString, QString> &tokens)
{
    const QString &title = tokens[QStringLiteral("title")];
    const QString &command = tokens[QStringLiteral("command")];
    const QString &profile = tokens[QStringLiteral("profile")];
    const QString &workdir = tokens[QStringLiteral("workdir")];
    const QColor &color = tokens[QStringLiteral("tabcolor")];

    Profile::Ptr baseProfile;
    if (!profile.isEmpty()) {
        baseProfile = ProfileManager::instance()->loadProfile(profile);
    }
    if (!baseProfile) {
        // fallback to default profile
        baseProfile = ProfileManager::instance()->defaultProfile();
    }

    Profile::Ptr newProfile = Profile::Ptr(new Profile(baseProfile));
    newProfile->setHidden(true);

    // FIXME: the method of determining whether to use newProfile does not
    // scale well when we support more fields in the future
    bool shouldUseNewProfile = false;

    if (!command.isEmpty()) {
        newProfile->setProperty(Profile::Command, command);
        newProfile->setProperty(Profile::Arguments, command.split(QLatin1Char(' ')));
        shouldUseNewProfile = true;
    }

    if (!title.isEmpty()) {
        newProfile->setProperty(Profile::LocalTabTitleFormat, title);
        newProfile->setProperty(Profile::RemoteTabTitleFormat, title);
        shouldUseNewProfile = true;
    }

    // For tab color support
    if (color.isValid()) {
        newProfile->setProperty(Profile::TabColor, color);
        shouldUseNewProfile = true;
    }

    if (m_parser->isSet(QStringLiteral("workdir"))) {
        newProfile->setProperty(Profile::Directory, m_parser->value(QStringLiteral("workdir")));
        shouldUseNewProfile = true;
    }

    if (!workdir.isEmpty()) {
        newProfile->setProperty(Profile::Directory, workdir);
        shouldUseNewProfile = true;
    }

    // Create the new session
    Profile::Ptr theProfile = shouldUseNewProfile ? newProfile : baseProfile;
    Session *session = window->createSession(theProfile, QString());

    if (m_parser->isSet(QStringLiteral("noclose"))) {
        session->setAutoClose(false);
    }

    if (!window->testAttribute(Qt::WA_Resized)) {
        window->resize(window->sizeHint());
    }

    // FIXME: this ugly hack here is to make the session start running, so that
    // its tab title is displayed as expected.
    //
    // This is another side effect of the commit fixing BKO 176902.
    window->show();
    window->hide();
}

// Creates a new Konsole window.
// If --new-tab is given, use existing window.
MainWindow *Application::processWindowArgs(bool &createdNewMainWindow)
{
    MainWindow *window = nullptr;
    if (m_parser->isSet(QStringLiteral("new-tab"))) {
        QListIterator<QWidget *> iter(QApplication::topLevelWidgets());
        iter.toBack();
        while (iter.hasPrevious()) {
            window = qobject_cast<MainWindow *>(iter.previous());
            if (window != nullptr) {
                break;
            }
        }
        qDebug() << Q_FUNC_INFO << " new - tab ";
    }

    if (window == nullptr) {
        createdNewMainWindow = true;
        qDebug() << Q_FUNC_INFO << "new main window";
        window = newMainWindow();

        // override default menubar visibility
        if (m_parser->isSet(QStringLiteral("show-menubar"))) {
            window->setMenuBarInitialVisibility(true);
        }
        if (m_parser->isSet(QStringLiteral("hide-menubar"))) {
            window->setMenuBarInitialVisibility(false);
        }
        if (m_parser->isSet(QStringLiteral("fullscreen"))) {
            window->viewFullScreen(true);
        }
        if (m_parser->isSet(QStringLiteral("show-tabbar"))) {
            window->viewManager()->setNavigationVisibility(ViewManager::AlwaysShowNavigation);
        }
        else if (m_parser->isSet(QStringLiteral("hide-tabbar"))) {
            window->viewManager()->setNavigationVisibility(ViewManager::AlwaysHideNavigation);
        }
    }
    return window;
}

// Loads a profile.
// If --profile <name> is given, loads profile <name>.
// If --fallback-profile is given, loads profile FALLBACK/.
// Else loads the default profile.
Profile::Ptr Application::processProfileSelectArgs()
{
    Profile::Ptr defaultProfile = ProfileManager::instance()->defaultProfile();

    if (m_parser->isSet(QStringLiteral("profile"))) {
        Profile::Ptr profile = ProfileManager::instance()->loadProfile(
            m_parser->value(QStringLiteral("profile")));
        if (profile) {
            return profile;
        }
    } else if (m_parser->isSet(QStringLiteral("fallback-profile"))) {
        Profile::Ptr profile = ProfileManager::instance()->loadProfile(QStringLiteral("FALLBACK/"));
        if (profile) {
            return profile;
        }
    }

    return defaultProfile;
}

bool Application::processHelpArgs()
{
    if (m_parser->isSet(QStringLiteral("list-profiles"))) {
        listAvailableProfiles();
        return true;
    } else if (m_parser->isSet(QStringLiteral("list-profile-properties"))) {
        listProfilePropertyInfo();
        return true;
    }
    return false;
}

void Application::listAvailableProfiles()
{
    const QStringList paths = ProfileManager::instance()->availableProfilePaths();

    for (const QString &path : paths) {
        QFileInfo info(path);
        printf("%s\n", info.completeBaseName().toLocal8Bit().constData());
    }
}

void Application::listProfilePropertyInfo()
{
    Profile::Ptr tempProfile = ProfileManager::instance()->defaultProfile();
    const QStringList names = tempProfile->propertiesInfoList();

    for (const QString &name : names) {
        printf("%s\n", name.toLocal8Bit().constData());
    }
}

Profile::Ptr Application::processProfileChangeArgs(Profile::Ptr baseProfile)
{
    bool shouldUseNewProfile = false;

    Profile::Ptr newProfile = Profile::Ptr(new Profile(baseProfile));
    newProfile->setHidden(true);

    // change the initial working directory
    if (m_parser->isSet(QStringLiteral("workdir"))) {
        newProfile->setProperty(Profile::Directory, m_parser->value(QStringLiteral("workdir")));
        shouldUseNewProfile = true;
    }

    // temporary changes to profile options specified on the command line
    const QStringList profileProperties = m_parser->values(QStringLiteral("p"));
    for (const QString &value : profileProperties) {
        ProfileCommandParser parser;

        QHashIterator<Profile::Property, QVariant> iter(parser.parse(value));
        while (iter.hasNext()) {
            iter.next();
            newProfile->setProperty(iter.key(), iter.value());
        }

        shouldUseNewProfile = true;
    }

    // run a custom command
    if (!m_customCommand.isEmpty()) {
        // Example: konsole -e man ls
        QString commandExec = m_customCommand[0];
        QStringList commandArguments(m_customCommand);
        if ((m_customCommand.size() == 1)
            && (QStandardPaths::findExecutable(commandExec).isEmpty())) {
            // Example: konsole -e "man ls"
            ShellCommand shellCommand(commandExec);
            commandExec = shellCommand.command();
            commandArguments = shellCommand.arguments();
        }

        if (commandExec.startsWith(QLatin1String("./"))) {
            commandExec = QDir::currentPath() + commandExec.mid(1);
        }

        newProfile->setProperty(Profile::Command, commandExec);
        newProfile->setProperty(Profile::Arguments, commandArguments);

        shouldUseNewProfile = true;
    }

    if (shouldUseNewProfile) {
        return newProfile;
    }
    return baseProfile;
}

void Application::startBackgroundMode(MainWindow *window)
{
    if (_backgroundInstance != nullptr) {
        return;
    }

    KActionCollection* collection = window->actionCollection();
    QAction* action = collection->addAction(QStringLiteral("toggle-background-window"));
    action->setObjectName(QStringLiteral("Konsole Background Mode"));
    action->setText(i18nc("@item", "Toggle Background Window"));
    KGlobalAccel::self()->setGlobalShortcut(action, QKeySequence(Konsole::ACCEL | Qt::SHIFT | Qt::Key_F12));
    connect(action, &QAction::triggered, this, &Application::toggleBackgroundInstance);

    _backgroundInstance = window;
}

void Application::toggleBackgroundInstance()
{
    Q_ASSERT(_backgroundInstance);

    if (!_backgroundInstance->isVisible()) {
        _backgroundInstance->show();
        // ensure that the active terminal display has the focus. Without
        // this, an odd problem occurred where the focus widget would change
        // each time the background instance was shown
        _backgroundInstance->setFocus();
    } else {
        _backgroundInstance->hide();
    }
}

void Application::slotActivateRequested(QStringList args, const QString & /*workingDir*/)
{
    // QCommandLineParser expects the first argument to be the executable name
    // In the current version it just strips it away
    args.prepend(qApp->applicationFilePath());

    m_customCommand = getCustomCommand(args);

    // We can't re-use QCommandLineParser instances, it preserves earlier parsed values
    auto parser = new QCommandLineParser;
    populateCommandLineParser(parser);
    parser->parse(args);
    m_parser.reset(parser);

    newInstance();
}
