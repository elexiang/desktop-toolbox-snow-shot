// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolboxcontroller.h"
#include "appfeedback.h"
#include "toolboxpaths.h"
#include "taskbardock.h"
#include "librarywindows.h"
#include <QSqlQuery>
#include <QApplication>
#include <QTranslator>
#include <QDir>
#include <QTimer>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QFontDatabase>
#include <QDebug>
#include <QTemporaryDir>
#include <QSettings>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QClipboard>
#include <QPainter>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QScrollBar>
#include <QComboBox>
#include <QButtonGroup>
#include <QToolButton>
#include <QKeySequenceEdit>
#include <QGroupBox>
#include <QScreen>
#include <cstdlib>
#include <cstdio>
#include <source_location>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--raise-taskbar-fixture"))) {
        const auto bar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!bar ||
            !SetWindowPos(bar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
            return 1;
        NotifyWinEvent(EVENT_OBJECT_REORDER, bar, OBJID_WINDOW, CHILDID_SELF);
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--snapshot-taskbar"))) {
        const auto caption = app.arguments().last().toStdWString();
        const auto nativeCard = FindWindowW(nullptr, caption.c_str());
        RECT nativeRect{};
        const auto dock = toolbox::currentTaskbarDock();
        if (!nativeCard || !IsWindowVisible(nativeCard) || !GetWindowRect(nativeCard, &nativeRect))
            return 1;
        std::fprintf(stderr,
                     "Live window visible=1 rect=(%ld,%ld %ldx%ld) foreground=%d minimized=%d\n",
                     nativeRect.left, nativeRect.top, nativeRect.right - nativeRect.left,
                     nativeRect.bottom - nativeRect.top, int(GetForegroundWindow() == nativeCard),
                     int(IsIconic(nativeCard)));
        auto* screen = QGuiApplication::primaryScreen();
        const auto desktop = screen->grabWindow(0);
        const qreal scale = screen->devicePixelRatio();
        const int left = std::max(0, int(nativeRect.left) - 10);
        const int top = std::max(0, qRound((dock.bar.y() - screen->geometry().y() - 10) * scale));
        QDir().mkpath(QStringLiteral("artifacts/toolbox-ui"));
        return desktop.copy(left, top, desktop.width() - left, desktop.height() - top)
                       .save(QStringLiteral("artifacts/toolbox-ui/taskbar-live.png"))
                   ? 0
                   : 1;
    }
#endif
    if (app.arguments().contains(QStringLiteral("--fullscreen-fixture"))) {
        QWidget window;
        window.setWindowTitle(QStringLiteral("Toolbox fullscreen test"));
        window.showFullScreen();
        window.activateWindow();
        QTimer::singleShot(2500, &app, &QApplication::quit);
        return app.exec();
    }
    if (app.arguments().contains(QStringLiteral("--render-card-samples"))) {
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        QDir().mkpath(QStringLiteral("artifacts/toolbox-ui"));
        toolbox::QuotaCard card;
        const QList<toolbox::QuotaWindow> windows{
            {{}, 60, QDateTime(QDate::currentDate(), QTime(15, 16)), QStringLiteral("5h")},
            {{},
             37,
             QDateTime(QDate::currentDate().addDays(2), QTime(16, 9)),
             QStringLiteral("w")}};
        for (bool stacked : {false, true}) {
            card.present(windows, false, true, stacked, {});
            card.show();
            app.processEvents();
            card.grab().save(stacked ? QStringLiteral("artifacts/toolbox-ui/v031-stacked.png")
                                     : QStringLiteral("artifacts/toolbox-ui/v031-compact.png"));
        }
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--make-ocr-fixture"))) {
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        QImage image(700, 200, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 28));
        painter.drawText(25, 75, QStringLiteral("Snow Shot 12345"));
        painter.drawText(25, 150, QString::fromUtf8("文字识别"));
        painter.end();
        return image.save(app.arguments().last()) ? 0 : 1;
    }
    if (app.arguments().size() == 1 ||
        app.arguments().contains(QStringLiteral("--capture-fixture"))) {
        QWidget window;
        window.setWindowTitle(QStringLiteral("Snow Toolbox capture fixture"));
        window.resize(780, 510);
        window.move(100, 100);
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(30, 25, 30, 25);
        auto* english = new QLabel(QStringLiteral("The meeting starts at nine in the morning."));
        english->setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 18));
        layout->addWidget(english);
        auto* chinese = new QLabel(QString::fromUtf8("会议将在早上九点开始。"));
        chinese->setFont(english->font());
        layout->addWidget(chinese);
        auto* color = new QLabel(QStringLiteral("RGB sample · 32, 128, 224"));
        color->setMinimumHeight(90);
        color->setAlignment(Qt::AlignCenter);
        color->setStyleSheet(QStringLiteral(
            "background:rgb(32,128,224);color:white;font:16pt 'Microsoft YaHei UI'"));
        layout->addWidget(color);
        auto* output = new QLineEdit;
        output->setPlaceholderText(QStringLiteral("Paste copied RGB or HEX here"));
        layout->addWidget(output);
        auto* controls = new QHBoxLayout;
        layout->addLayout(controls);
        auto* copy = new QPushButton(QStringLiteral("Copy next unique test image"));
        controls->addWidget(copy);
        auto* paste = new QPushButton(QStringLiteral("Preview clipboard image"));
        controls->addWidget(paste);
        auto* preview = new QLabel;
        preview->setMinimumHeight(140);
        layout->addWidget(preview);
        int counter = 0;
        QObject::connect(copy, &QPushButton::clicked, &window, [&] {
            QImage image(300, 180, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor(32, 128, 224));
            QPainter painter(&image);
            painter.setPen(Qt::white);
            painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 18));
            painter.drawText(image.rect(), Qt::AlignCenter,
                             QStringLiteral("Snow Toolbox pin %1").arg(++counter));
            painter.end();
            QApplication::clipboard()->setImage(image);
        });
        QObject::connect(paste, &QPushButton::clicked, &window, [&] {
            const auto image = QApplication::clipboard()->pixmap();
            preview->setPixmap(
                image.scaled(650, 140, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        });
        window.show();
        return app.exec();
    }
    QTemporaryDir profile;
    const bool contextVerify = app.arguments().contains(QStringLiteral("--verify-context"));
    const bool taskbarVerify = app.arguments().contains(QStringLiteral("--verify-taskbar-native"));
    const bool verify =
        contextVerify || taskbarVerify || app.arguments().contains(QStringLiteral("--verify"));
    if (verify || app.arguments().contains(QStringLiteral("--render")) ||
        app.arguments().contains(QStringLiteral("--render-library"))) {
        qputenv("SNOW_TOOLBOX_DATA", profile.path().toUtf8());
        qputenv("CODEX_HOME", profile.path().toUtf8());
        QSettings settings(toolbox::settingsPath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("quota"), true);
        settings.setValue(QStringLiteral("codexExecutable"),
                          profile.filePath(QStringLiteral("missing.exe")));
        settings.setValue(
            QStringLiteral("quotaCache"),
            QByteArray(
                R"({"codex":{"primary":{"usedPercent":18,"windowDurationMins":300,"resetsAt":1794813360},"secondary":{"usedPercent":63,"windowDurationMins":10080,"resetsAt":1794989386}}})"));
        if (taskbarVerify) {
            settings.setValue(QStringLiteral("cardStyle"), QStringLiteral("taskbar"));
            settings.setValue(QStringLiteral("cardPosition"), QPoint(100, 100));
        }
        settings.setValue(QStringLiteral("memory"), !verify);
        settings.setValue(QStringLiteral("translation"), !verify);
        if (contextVerify) {
            QFile config(profile.filePath(QStringLiteral("config.toml")));
            if (!config.open(QIODevice::WriteOnly))
                return 1;
            config.write("# preserved\nmodel = \"test-model\"\n");
        }
    }
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    app.setQuitOnLastWindowClosed(false);
    QTranslator translator;
    const auto libraryCatalog = qEnvironmentVariable("SNOW_TOOLBOX_TEST_CATALOG");
    if (translator.load(libraryCatalog.isEmpty()
                            ? app.applicationDirPath() + QStringLiteral("/toolbox_zh_CN.qm")
                            : libraryCatalog))
        app.installTranslator(&translator);
    bool autoStartState = false;
    int autoStartChanges = 0;
    toolbox::ToolboxActions actions{[] {}, [] {}, [] {}, [] {}, [] {}};
    actions.autoStartEnabled = [&] { return autoStartState; };
    actions.setAutoStart = [&](bool value) {
        autoStartState = value;
        ++autoStartChanges;
        return QString();
    };
    bool shortcutEditing = false;
    QStringList appliedShortcuts;
    actions.setShortcutEditing = [&](bool editing) { shortcutEditing = editing; };
    actions.applyCaptureShortcut = [&] {
        if (shortcutEditing)
            return QStringLiteral("Cannot apply while editing");
        appliedShortcuts.append(
            QSettings(toolbox::settingsPath(), QSettings::IniFormat)
                .value(QStringLiteral("captureShortcut"), QStringLiteral("Ctrl+Alt+1"))
                .toString());
        return QString();
    };
    toolbox::ToolboxController controller(actions, &app);
    controller.showHome();
    if (app.arguments().contains(QStringLiteral("--render-library"))) {
        qint64 noteId = 0;
        {
            toolbox::LocalLibrary seed(toolbox::dataPath() + QStringLiteral("/library"));
            if (!seed.isOpen())
                return 1;
            seed.recordText(QStringLiteral("SELECT order_no, status\nFROM production_order\nWHERE status = '进行中';"));
            noteId = seed.clipToNote(seed.clipboard().first().id);
            seed.saveNote(noteId, QStringLiteral("生产订单排查"),
                          QStringLiteral("# 排查记录\n\n- [x] 核对订单状态\n- [ ] 确认接口返回\n\nSELECT order_no, status\nFROM production_order;\n\n操作结果与常用 SQL 都可以保存在这里。"),
                          QStringLiteral("SQL、项目、备忘"), true);
            QImage image(600, 220, QImage::Format_RGB32);
            image.fill(QColor(QStringLiteral("#f0f5ff")));
            QPainter painter(&image);
            painter.setPen(QColor(QStringLiteral("#2259ad")));
            painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 24));
            painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("本地保存 · 随时查找\n文字和图片都可以转存为笔记"));
            painter.end();
            seed.recordImage(image);
            seed.clipToNote(seed.clipboard().first().id, noteId);
        }
        QWidget* home = nullptr;
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->objectName() == QStringLiteral("ToolboxHome"))
                home = widget;
        if (!home)
            return 1;
        QDir().mkpath(QStringLiteral("artifacts/toolbox-ui/local-library"));
        home->findChild<QPushButton*>(QStringLiteral("OpenClipboardHistory"))->click();
        auto* library = home->findChild<toolbox::LibraryWindow*>(QStringLiteral("LibraryWindow"));
        app.processEvents();
        bool saved = library->grab().save(QStringLiteral("artifacts/toolbox-ui/local-library/clipboard.png"));
        home->findChild<QPushButton*>(QStringLiteral("OpenLocalNotes"))->click();
        library->notes()->selectNote(noteId);
        app.processEvents();
        saved = saved && library->grab().save(QStringLiteral("artifacts/toolbox-ui/local-library/notes.png"));
        library->findChild<QPushButton*>(QStringLiteral("NoteBatch"))->click();
        app.processEvents();
        saved = saved && library->grab().save(QStringLiteral("artifacts/toolbox-ui/local-library/notes-batch.png"));
        return saved ? 0 : 1;
    }
    if (verify)
        QTimer::singleShot(0, &app, [&] {
            QWidget *home = nullptr, *card = nullptr;
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (widget->objectName() == QStringLiteral("ToolboxHome"))
                    home = widget;
                if (widget->objectName() == QStringLiteral("QuotaCard"))
                    card = widget;
            }
            const auto require = [](bool value, const char* message = "UI state assertion failed",
                                    std::source_location location =
                                        std::source_location::current()) {
                if (!value) {
                    std::fprintf(stderr, "%s (line %u)\n", message, location.line());
                    std::exit(1);
                }
            };
            require(home && card && home->isVisible() && card->isVisible());
            if (!taskbarVerify && !contextVerify) {
                auto* quota = controller.findChild<toolbox::QuotaService*>();
                auto* timer = quota ? quota->findChild<QTimer*>() : nullptr;
                auto* manual = home->findChild<QPushButton*>(QStringLiteral("ManualQuotaRefresh"));
                require(timer && manual, "quota timer and manual refresh are available");
                quota->setEnabled(false);
                quota->setEnabled(true);
                home->hide();
                controller.showHome();
                require(timer->isActive() && timer->interval() == 120'000,
                        "opening the panel queries immediately and schedules 120 seconds");

                quota->setEnabled(false);
                quota->setEnabled(true);
                timer->start(30'000);
                QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), card->mapToGlobal(QPoint(1, 1)));
                QApplication::sendEvent(card, &enter);
                require(timer->interval() == 120'000,
                        "entering the desktop card immediately resets the quota timer");

                timer->start(30'000);
                QApplication::sendEvent(card, &enter);
                home->hide();
                controller.showHome();
                require(timer->interval() == 30'000, "card and panel share the cooldown");
                manual->click();
                require(timer->interval() == 120'000, "manual refresh bypasses the cooldown");

                timer->start(30'000);
                require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection) &&
                            timer->interval() == 120'000,
                        "a scheduled refresh runs and restarts the 120-second timer");
            }
            const auto workArea = QGuiApplication::primaryScreen()->availableGeometry();
            const auto centered = [&] { return home->frameGeometry().center() == workArea.center(); };
            require(centered(), "toolbox opens centered in the primary work area");
            QSettings placement(toolbox::settingsPath(), QSettings::IniFormat);
            const auto positionKey = QStringLiteral("homePosition");
            require(!placement.contains(positionKey), "automatic centering does not save a position");
            auto* homeTabs = home->findChild<QTabWidget*>();
            require(homeTabs && homeTabs->count() == 3, "toolbox has three pages");
            const auto initialFrame = home->frameGeometry();
            for (const int page : {2, 1, 0}) {
                homeTabs->setCurrentIndex(page);
                controller.refreshUi();
                app.processEvents();
                require(home->frameGeometry() == initialFrame,
                        "switching pages and refreshing keep the toolbox frame unchanged");
            }
            const auto savedPosition = workArea.topLeft();
#ifdef Q_OS_WIN
            if (QGuiApplication::platformName() == QStringLiteral("windows")) {
                const auto handle = reinterpret_cast<HWND>(home->winId());
                require(IsWindowVisible(handle), "toolbox is visible at native startup");
                SendMessageW(handle, WM_ENTERSIZEMOVE, 0, 0);
                home->move(savedPosition);
                SendMessageW(handle, WM_EXITSIZEMOVE, 0, 0);
                placement.sync();
                require(placement.value(positionKey).toPoint() == savedPosition,
                        "native manual move saves the toolbox position");
            } else
#endif
            {
                placement.setValue(positionKey, savedPosition);
            }
            home->close();
            controller.showHome();
            require(home->frameGeometry().topLeft() == savedPosition,
                    "reopening the toolbox restores a saved position");
            home->showMinimized();
            controller.showHome();
            require(!home->isMinimized() && home->frameGeometry().topLeft() == savedPosition,
                    "restoring a minimized toolbox keeps the saved position");
            placement.setValue(positionKey, QPoint(-100000, -100000));
            home->hide();
            controller.showHome();
            require(centered(), "an unavailable saved position falls back to the center");
            placement.remove(positionKey);
            auto* openClipboard = home->findChild<QPushButton*>(QStringLiteral("OpenClipboardHistory"));
            auto* openNotes = home->findChild<QPushButton*>(QStringLiteral("OpenLocalNotes"));
            require(openClipboard && openNotes && openClipboard->isEnabled() && openNotes->isEnabled());
            openClipboard->click();
            auto* library = home->findChild<toolbox::LibraryWindow*>(QStringLiteral("LibraryWindow"));
            auto* clips = library ? library->findChild<QWidget*>(QStringLiteral("ClipboardWindow")) : nullptr;
            auto* menu = library ? library->findChild<QTabBar*>(QStringLiteral("LibraryViewMenu")) : nullptr;
            require(library && clips && menu && clips->isVisible() && menu->count() == 2,
                    "The tool must open one shared window with two tabs");
            if (!libraryCatalog.isEmpty())
                require(menu->tabText(0) == QStringLiteral("剪贴板") &&
                            menu->tabText(1) == QStringLiteral("笔记"),
                        "The production catalog must translate the library tabs");
            auto* notes = library->notes();
            const auto windowId = library->winId();
            clips->findChild<QLineEdit*>(QStringLiteral("ClipboardSearch"))->setText(QStringLiteral("记住搜索"));
            menu->setCurrentIndex(1);
            require(notes->isVisible() && !clips->isVisible() && notes->window() == library &&
                        library->winId() == windowId,
                    "Switching to notes must only change the content of the same native window");
            menu->setCurrentIndex(0);
            require(clips->isVisible() && !notes->isVisible() &&
                        clips->findChild<QLineEdit*>(QStringLiteral("ClipboardSearch"))->text() == QStringLiteral("记住搜索"),
                    "Returning to clipboard must retain its search and reuse the same page");
            openNotes->click();
            require(notes->isVisible() && library->winId() == windowId &&
                        home->findChildren<toolbox::LibraryWindow*>().size() == 1,
                    "Both toolbox entries must reuse the same native window");
            library->close();
            require(home->findChild<QKeySequenceEdit*>(QStringLiteral("QuickClipboardShortcut"))->keySequence() == QKeySequence(QStringLiteral("Ctrl+Alt+V")) &&
                        home->findChild<QKeySequenceEdit*>(QStringLiteral("QuickNotesShortcut"))->keySequence() == QKeySequence(QStringLiteral("Ctrl+Alt+N")),
                    "Quick access defaults must be visible in settings");
            controller.showQuickPicker(true);
            require(library->isVisible() && menu->currentIndex() == 1 &&
                        home->findChild<toolbox::LibraryWindow*>(QStringLiteral("LibraryWindow")) == library,
                    "The notes hotkey must reopen the main-panel library window");
            const auto reopenedWindowId = library->winId();
            controller.showQuickPicker(false);
            require(library->isVisible() && clips->isVisible() && menu->currentIndex() == 0 &&
                        home->findChildren<toolbox::LibraryWindow*>().size() == 1 &&
                        clips->findChild<QLineEdit*>(QStringLiteral("ClipboardSearch"))->text() == QStringLiteral("记住搜索"),
                    "The clipboard hotkey must reuse the same tab, search state and native window");
            library->showMinimized();
            controller.showQuickPicker(false);
            require(library->isVisible() && !library->isMinimized() && clips->isVisible(),
                    "The clipboard hotkey must restore the shared window when minimized");
            openNotes->click();
            require(notes->isVisible() && library->winId() == reopenedWindowId,
                    "The main-panel action must switch the same window back to notes");
            auto* editingNotes = home->findChild<toolbox::NotesWindow*>(QStringLiteral("NotesWindow"));
            editingNotes->findChild<QPushButton*>(QStringLiteral("NoteNew"))->click();
            editingNotes->findChild<QTextEdit*>(QStringLiteral("NoteBody"))->setPlainText(QStringLiteral("退出前未保存"));
            QString libraryConnection;
            for (const auto& name : QSqlDatabase::connectionNames())
                if (QSqlDatabase::database(name).databaseName() == toolbox::dataPath() + QStringLiteral("/library/library.sqlite"))
                    libraryConnection = name;
            require(!libraryConnection.isEmpty(), "Controller library connection must exist");
            {
                QSqlQuery query(QSqlDatabase::database(libraryConnection));
                require(query.exec(QStringLiteral("PRAGMA query_only=ON")), "Write failure fixture must configure");
            }
            menu->setCurrentIndex(0);
            require(menu->currentIndex() == 1 && editingNotes->isVisible(),
                    "A failed autosave must keep the editor and the notes tab active");
            QEvent quitAttempt(QEvent::Quit);
            QCoreApplication::sendEvent(qApp, &quitAttempt);
            require(!quitAttempt.isAccepted() && editingNotes->isVisible(),
                    "Quit must be blocked when pending notes cannot be saved");
            {
                QSqlQuery query(QSqlDatabase::database(libraryConnection));
                require(query.exec(QStringLiteral("PRAGMA query_only=OFF")), "Write failure fixture must reset");
            }
            require(editingNotes->flush(), "Pending notes must save after write recovery");
            library->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            const auto* toolboxTabs = home->findChild<QTabWidget*>();
            require(toolboxTabs && toolboxTabs->tabText(0) == QString::fromUtf8("工具") &&
                        toolboxTabs->tabText(2) == QString::fromUtf8("设置"),
                    "Toolbox navigation must remain Chinese");
            bool chineseSave = false;
            bool chineseBrowse = false;
            for (const auto* item : home->findChildren<QPushButton*>()) {
                chineseSave |= item->text() == QString::fromUtf8("保存并应用");
                chineseBrowse |= item->text() == QString::fromUtf8("浏览…");
                require(item->text() != QStringLiteral("Save and apply") &&
                            item->text() != QStringLiteral("Browse…"),
                        "Settings actions must not fall back to English");
            }
            require(chineseSave && chineseBrowse, "Settings actions must remain Chinese");
            auto* openQr = home->findChild<QPushButton*>(QStringLiteral("OpenQrGenerator"));
            require(openQr && openQr->isEnabled(), "QR generator entry must exist");
            openQr->click();
            auto* qrWindow = home->findChild<QWidget*>(QStringLiteral("QrGeneratorWindow"));
            require(qrWindow && qrWindow->isVisible(), "QR generator entry must open its window");
            openQr->click();
            require(home->findChildren<QWidget*>(QStringLiteral("QrGeneratorWindow")).size() == 1,
                    "Repeated QR entry clicks must reuse the window");
            qrWindow->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            openQr->click();
            qrWindow = home->findChild<QWidget*>(QStringLiteral("QrGeneratorWindow"));
            require(qrWindow && qrWindow->isVisible(), "QR generator must reopen after closing");
            qrWindow->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
#ifdef Q_OS_WIN
            const auto homeHandle = reinterpret_cast<HWND>(home->winId());
            if (taskbarVerify)
                require(IsWindowVisible(homeHandle) && !IsIconic(homeHandle),
                        "toolbox must actually be shown by Windows, including a hidden startup");
#endif
            const auto verifyHomeOpened = [&] {
                app.processEvents();
                require(home->isVisible() && !home->isMinimized(),
                        "toolbox restored by entry point");
#ifdef Q_OS_WIN
                if (QGuiApplication::platformName() == QStringLiteral("windows"))
                    require(IsWindowVisible(homeHandle) && !IsIconic(homeHandle),
                            "entry point shows native toolbox window");
#endif
            };
            home->hide();
            const QPoint clickPoint(20, 20), clickGlobal = card->mapToGlobal(clickPoint);
            QMouseEvent clickPress(QEvent::MouseButtonPress, clickPoint, clickGlobal,
                                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent clickRelease(QEvent::MouseButtonRelease, clickPoint, clickGlobal,
                                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(card, &clickPress);
            QApplication::sendEvent(card, &clickRelease);
            verifyHomeOpened();
            home->hide();
            QWidget* detail = nullptr;
            for (auto* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == QStringLiteral("QuotaDetail"))
                    detail = widget;
            require(detail, "hover detail exists");
            auto* open = detail->findChild<QPushButton*>(QStringLiteral("OpenToolbox"));
            require(open, "hover detail has open toolbox action");
            detail->show();
            open->click();
            verifyHomeOpened();
            require(!detail->isVisible(), "opening toolbox dismisses hover detail");
            home->showMinimized();
            auto* tray = controller.findChild<QSystemTrayIcon*>(QStringLiteral("ToolboxTray"));
            require(tray, "tray activation entry point exists");
            tray->activated(QSystemTrayIcon::Trigger);
            verifyHomeOpened();
            home->hide();
            tray->contextMenu()->actions().first()->trigger();
            verifyHomeOpened();
            home->hide();
            controller.showHome();
            verifyHomeOpened();
            if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
                // The offscreen backend cannot regain OS focus after a minimize/restore cycle.
                QApplication::setActiveWindow(home);
                app.processEvents();
            }
            if (taskbarVerify) {
                auto* styles = home->findChild<QButtonGroup*>(QStringLiteral("QuotaCardStyle"));
                auto* save = home->findChild<QPushButton*>(QStringLiteral("SaveToolboxSettings"));
                require(styles && styles->buttons().size() == 3 && save, "three native styles");
                require(styles->checkedId() == 2, "saved taskbar style is restored on startup");
                const auto freePosition = QSettings(toolbox::settingsPath(), QSettings::IniFormat)
                                              .value(QStringLiteral("cardPosition"))
                                              .toPoint();
                styles->button(2)->click();
                save->click();
                const auto dock = toolbox::currentTaskbarDock();
                const auto target = toolbox::taskbarCardPosition(dock, card->size());
                std::fprintf(stderr,
                             "Native bar=(%d,%d %dx%d), tray=(%d,%d %dx%d), dockVisible=%d, "
                             "card=(%d,%d %dx%d), cardVisible=%d\n",
                             dock.bar.x(), dock.bar.y(), dock.bar.width(), dock.bar.height(),
                             dock.tray.x(), dock.tray.y(), dock.tray.width(), dock.tray.height(),
                             int(dock.visible), card->x(), card->y(), card->width(), card->height(),
                             int(card->isVisible()));
                qInfo() << "Native taskbar" << dock.bar << "tray" << dock.tray << "capsule"
                        << card->geometry() << "visible" << card->isVisible();
                require(target && card->pos() == *target && card->isVisible(),
                        "native capsule must be visible inside taskbar left of tray");
#ifdef Q_OS_WIN
                const auto cardHandle = reinterpret_cast<HWND>(card->winId());
                require(card->property("TaskbarPeekExcluded").isValid() &&
                            card->property("TaskbarPeekExcluded").toBool(),
                        "DWM accepted the docked capsule Peek exclusion");
                auto* tracking = controller.findChild<QTimer*>(QStringLiteral("TaskbarTracking"));
                require(tracking, "fallback tracking timer exists");
                tracking->stop();
                QProcess raiseTaskbar;
                raiseTaskbar.start(app.applicationFilePath(),
                                   {QStringLiteral("--raise-taskbar-fixture")});
                require(raiseTaskbar.waitForFinished(3000) && raiseTaskbar.exitCode() == 0,
                        "separate process raises taskbar without changing foreground");
                QElapsedTimer recovery;
                recovery.start();
                while (toolbox::taskbarCoversCard(cardHandle) && recovery.elapsed() < 350)
                    app.processEvents(QEventLoop::AllEvents, 10);
                require(!toolbox::taskbarCoversCard(cardHandle),
                        "shell event repairs stacking before the 750ms fallback timer");
                std::fprintf(stderr, "Taskbar event recovery: %lld ms\n", recovery.elapsed());
#endif
                controller.captureActivity(true);
                controller.refreshUi();
                require(!card->isVisible(), "native capture hides capsule");
                controller.captureActivity(false);
                require(card->isVisible() && card->pos() == *target, "capture restores docking");
                home->hide();
                const auto settle = [&](int milliseconds) {
                    QElapsedTimer timer;
                    timer.start();
                    while (timer.elapsed() < milliseconds)
                        app.processEvents(QEventLoop::AllEvents, 20);
                };
                settle(1000);
                require(card->isVisible(), "tracking preserves native visibility");
                auto* screen = QGuiApplication::screenAt(card->geometry().center());
                require(screen, "dock monitor exists");
                const auto desktop = screen->grabWindow(0);
                const qreal scale = desktop.devicePixelRatio();
                const QRect strip(qRound((card->x() - screen->geometry().x() - 10) * scale),
                                  qRound((dock.bar.y() - screen->geometry().y() - 10) * scale),
                                  qRound((screen->geometry().right() - card->x() + 11) * scale),
                                  qRound((dock.bar.height() + 10) * scale));
                QDir().mkpath(QStringLiteral("artifacts/toolbox-ui"));
                require(desktop.copy(strip).save(
                            QStringLiteral("artifacts/toolbox-ui/taskbar-native.png")),
                        "save actual composed taskbar preview");
                QProcess fullscreen;
                fullscreen.start(app.applicationFilePath(),
                                 {QStringLiteral("--fullscreen-fixture")});
                require(fullscreen.waitForStarted(), "launch separate fullscreen fixture");
                settle(1500);
                require(!card->isVisible(), "fullscreen hides docked capsule");
                QElapsedTimer fullscreenWait;
                fullscreenWait.start();
                while (fullscreen.state() != QProcess::NotRunning &&
                       fullscreenWait.elapsed() < 5000)
                    app.processEvents(QEventLoop::AllEvents, 20);
                require(fullscreen.state() == QProcess::NotRunning, "fullscreen fixture exits");
                settle(1000);
                require(card->isVisible(), "leaving fullscreen restores capsule");
                styles->button(0)->click();
                save->click();
                require(card->isVisible() && card->pos() == freePosition && card->height() == 36,
                        "native undocking restores free geometry");
#ifdef Q_OS_WIN
                require(card->property("TaskbarPeekExcluded").isValid() &&
                            !card->property("TaskbarPeekExcluded").toBool(),
                        "DWM accepted normal Peek behavior after undocking");
#endif
                qInfo() << "Native taskbar docking and fullscreen checks passed";
                app.quit();
                return;
            }
            require(toolbox::taskbarLogicalRect(QRect(-2560, 1380, 2560, 60), QPoint(-2560, 0),
                                                QPoint(-1707, 0),
                                                1.5) == QRect(-1707, 920, 1707, 40),
                    "native geometry maps to logical pixels at 150 percent DPI");
            require(toolbox::taskbarShellClass(QStringLiteral("TaskListThumbnailWnd")) &&
                        toolbox::taskbarShellClass(QStringLiteral("MultitaskingViewFrame")) &&
                        !toolbox::taskbarShellClass(QStringLiteral("Chrome_WidgetWin_1")),
                    "shell previews are distinguished from real application fullscreen windows");
            const toolbox::TaskbarDock bottom{QRect(0, 1040, 1920, 40), QRect(1660, 1040, 260, 40),
                                              true};
            require(toolbox::taskbarCardPosition(bottom, QSize(300, 36)) == QPoint(1354, 1042),
                    "capsule sits inside taskbar immediately left of tray");
            const toolbox::TaskbarDock otherMonitor{QRect(-1920, 1040, 1920, 40),
                                                    QRect(-260, 1040, 260, 40), true};
            require(toolbox::taskbarCardPosition(otherMonitor, QSize(300, 36)) ==
                        QPoint(-566, 1042),
                    "negative monitor origin is preserved");
            auto hidden = bottom;
            hidden.visible = false;
            require(!toolbox::taskbarCardPosition(hidden, QSize(300, 36)),
                    "hidden taskbar and fullscreen cannot display capsule");
            require(!toolbox::taskbarCardPosition(bottom, QSize(1800, 36)) &&
                        !toolbox::taskbarCardPosition(bottom, QSize(300, 50)),
                    "insufficient space cannot cover tray or overflow taskbar height");
            require(!toolbox::taskbarCardPosition(
                        {QRect(0, 0, 40, 1080), QRect(0, 900, 40, 180), true}, QSize(300, 36)),
                    "vertical taskbar cannot fit horizontal capsule");
            require(home && card && home->isVisible() && card->isVisible());
            auto* tabs = home->findChild<QTabWidget*>();
            auto* contextBox = home->findChild<QGroupBox*>(QStringLiteral("CodexContextBox"));
            auto* overview = home->findChild<QWidget*>(QStringLiteral("OverviewPage"));
            auto* settingsPage = home->findChild<QWidget*>(QStringLiteral("SettingsPage"));
            auto* activity = home->findChild<QPlainTextEdit*>(QStringLiteral("ActivityLog"));
            require(tabs && tabs->tabText(0) == QString::fromUtf8("工具"),
                    "overview renamed Tools");
            require(!home->findChild<QLabel*>(QStringLiteral("ToolboxStatus"))->isVisible(),
                    "tools page has no bottom activity status");
            require(tabs && tabs->count() == 3 &&
                        tabs->widget(1)->objectName() == QStringLiteral("LogPage"),
                    "logs tab between overview and settings");
            require(contextBox && overview->isAncestorOf(contextBox) &&
                        !settingsPage->isAncestorOf(contextBox),
                    "context controls belong to overview");
            require(activity && activity->isReadOnly() && !activity->toPlainText().isEmpty(),
                    "activity log contains startup events");
            controller.reportClipboardCopy(42, QStringLiteral("publish_failed"), 3, 5);
            require(activity->toPlainText().contains(
                        QStringLiteral("operation=42 stage=publish_failed attempts=3 code=0x00000005")),
                    "Native clipboard error must be visible in the activity log");
            const auto popupRecord = qEnvironmentVariable("SNOW_TOOLBOX_TEST_POPUP_RECORD");
            if (!popupRecord.isEmpty())
                require(activity->toPlainText().contains(
                            QStringLiteral("[Windows.ApplicationPopup] record=%1 ").arg(popupRecord)),
                        "An existing Windows popup must be imported without replaying the action");
            for (auto* b : home->findChildren<QPushButton*>())
                require(b->text() != QString::fromUtf8("退出工具箱") &&
                            b->text() != QStringLiteral("Quit toolbox"),
                        "home has no quit button");
            auto* refresh = home->findChild<QPushButton*>(QStringLiteral("ManualQuotaRefresh"));
            auto* quotaStatus = home->findChild<QLabel*>(QStringLiteral("QuotaStatus"));
            auto* updated = home->findChild<QLabel*>(QStringLiteral("QuotaUpdated"));
            app.processEvents();
            require(refresh && quotaStatus && updated &&
                        refresh->parentWidget() == quotaStatus->parentWidget() &&
                        std::abs(refresh->geometry().center().y() -
                                 updated->geometry().center().y()) <= 1,
                    "quota status, timestamp and manual refresh share a row");
            require(refresh->height() <= 40, "compact action buttons");
            auto* toggle = home->findChild<QCheckBox*>(QStringLiteral("CodexContextToggle"));
            auto* help = home->findChild<QToolButton*>(QStringLiteral("CodexContextHelp"));
            auto* defaults = home->findChild<QPushButton*>(QStringLiteral("CodexContextDefault"));
            require(toggle && help && defaults && contextBox->isAncestorOf(toggle) &&
                        contextBox->isAncestorOf(refresh) &&
                        std::abs(toggle->geometry().center().y() -
                                 defaults->geometry().center().y()) <= 1,
                    "quota and single row context controls in Codex panel");
            require(!help->toolTip().isEmpty(), "context explanation is in question mark");
            require(!home->findChild<QPushButton*>(QStringLiteral("Primary")),
                    "capture action has no arbitrary primary emphasis");
            tabs->setCurrentIndex(2);
            app.processEvents();
            auto* shortcut = home->findChild<QKeySequenceEdit*>(QStringLiteral("CaptureShortcut"));
            auto* session =
                home->findChild<QPushButton*>(QStringLiteral("ScreenshotSessionShortcuts"));
            require(shortcut && session && shortcut->x() < session->x() &&
                        std::abs(shortcut->geometry().center().y() -
                                 session->geometry().center().y()) <= 1,
                    "session shortcuts next to screenshot shortcut");
            auto* startupRow = settingsPage->layout()->itemAt(0)->layout();
            require(startupRow && startupRow->itemAt(0)->widget()->objectName() ==
                        QStringLiteral("ToolboxAutoStart") &&
                        startupRow->itemAt(1)->widget()->objectName() ==
                        QStringLiteral("ClipboardRecordingSetting"),
                    "startup and clipboard recording share the first settings row");
            auto* quickClipboard =
                settingsPage->findChild<QKeySequenceEdit*>(QStringLiteral("QuickClipboardShortcut"));
            auto* quickNotes =
                settingsPage->findChild<QKeySequenceEdit*>(QStringLiteral("QuickNotesShortcut"));
            require(quickClipboard && quickNotes && quickClipboard->x() < quickNotes->x() &&
                        quickClipboard->geometry().center().y() == quickNotes->geometry().center().y(),
                    "quick clipboard and notes shortcuts share a settings row");
            auto* settingsScroll = qobject_cast<QScrollArea*>(tabs->currentWidget());
            require(settingsScroll && settingsScroll->verticalScrollBar()->maximum() == 0 &&
                        settingsScroll->horizontalScrollBar()->maximum() == 0,
                    "all settings fit in the default panel without scrollbars");
            tabs->setCurrentIndex(0);
            auto* retention = home->findChild<QComboBox*>(QStringLiteral("LogRetention"));
            require(retention && retention->currentData().toInt() == 30,
                    "default retention 30 days");
            retention->setCurrentIndex(retention->findData(7));
            require(QSettings(toolbox::settingsPath(), QSettings::IniFormat)
                            .value(QStringLiteral("logRetentionDays"))
                            .toInt() == 7,
                    "retention selection persists immediately");
            require(!QImage(QStringLiteral(":/toolbox/reset.svg")).isNull(),
                    "reset icon must load");
            require(card->accessibleDescription().contains(QStringLiteral("5h")) &&
                        card->accessibleDescription().contains(QStringLiteral("w")),
                    "two quota groups");
            require(workArea.contains(home->frameGeometry()) &&
                        home->frameSize() == initialFrame.size(),
                    "the default panel size stays within the work area");
            require(home->findChild<QLabel*>(QStringLiteral("DragonAccent"))->size() ==
                        QSize(36, 36),
                    "dragon remains a small title accent");
            require(!QImage(QStringLiteral(":/toolbox/dragon/avatar.png")).isNull(),
                    "dragon asset embedded");
            require(card->height() == 36 && card->testAttribute(Qt::WA_TranslucentBackground),
                    "capsule geometry and transparency");
            auto* styles = home->findChild<QButtonGroup*>(QStringLiteral("QuotaCardStyle"));
            auto* startup = home->findChild<QCheckBox*>(QStringLiteral("ToolboxAutoStart"));
            auto* save = home->findChild<QPushButton*>(QStringLiteral("SaveToolboxSettings"));
            require(styles && startup && save, "new settings controls");
            tabs->setCurrentIndex(2);
            app.processEvents();
            auto* theme = home->findChild<QComboBox*>(QStringLiteral("ToolboxThemeColor"));
            require(theme && theme->count() == 6 && theme->currentData() == QStringLiteral("blue"),
                    "theme defaults to blue and offers six colors");
            const auto originalSize = card->size();
            const auto blueSheet = home->styleSheet();
            for (int i = 1; i < theme->count(); ++i) {
                theme->setCurrentIndex(i);
                app.processEvents();
                require(QSettings(toolbox::settingsPath(), QSettings::IniFormat)
                                .value(QStringLiteral("themeColor")) == theme->currentData(),
                        "theme selection is persisted immediately");
                require(home->styleSheet() != blueSheet && card->size() == originalSize,
                        "theme applies immediately without changing card geometry");
                require(home->styleSheet().contains(home->palette().color(QPalette::Highlight).name()),
                        "palette highlight matches the theme stylesheet");
            }
            theme->setCurrentIndex(0);
            require(home->styleSheet() == blueSheet, "default theme can be restored");
            shortcut->setFocus();
            app.processEvents();
            require(shortcutEditing, "editing suspends registered shortcuts before typing");
            require(shortcut->width() == 220, "shortcut editor stays compact");
            shortcut->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+9")));
            styles->button(1)->click();
            startup->setChecked(true);
            save->click();
            require(!shortcutEditing && appliedShortcuts.last() == QStringLiteral("Ctrl+Alt+9"),
                    "save resumes shortcuts and applies saved binding immediately");
            require(card->height() == 112 && autoStartState && autoStartChanges == 1,
                    "four rows and startup change");
            require(QSettings(toolbox::settingsPath(), QSettings::IniFormat)
                            .value(QStringLiteral("cardStyle"))
                            .toString() == QStringLiteral("stacked"),
                    "card style persistence");
            shortcut->setFocus();
            app.processEvents();
            require(shortcutEditing, "editing can suspend again");
            session->setFocus();
            app.processEvents();
            require(!shortcutEditing, "leaving editor restores current registered shortcut");
            shortcut->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+0")));
            styles->button(0)->click();
            startup->setChecked(false);
            save->click();
            require(appliedShortcuts.last() == QStringLiteral("Ctrl+Alt+0"),
                    "subsequent saves update live registration again");
            require(card->height() == 36 && !autoStartState && autoStartChanges == 2,
                    "compact style and disable startup");
            require(activity->toPlainText().contains(QString::fromUtf8("已保存")) ||
                        activity->toPlainText().contains(QStringLiteral("Settings saved")),
                    "settings events are logged");
            require(activity->toPlainText()
                            .section(QLatin1Char('\n'), 0, 0)
                            .contains(QString::fromUtf8("已保存")) ||
                        activity->toPlainText()
                            .section(QLatin1Char('\n'), 0, 0)
                            .contains(QStringLiteral("Settings saved")),
                    "new events appear first even within the same second");
            const auto moduleSwitch = [settingsPage](const QString& key) -> QCheckBox* {
                for (auto* check : settingsPage->findChildren<QCheckBox*>())
                    if (check->property("module").toString() == key)
                        return check;
                return nullptr;
            };
            auto* clipboardSwitch = moduleSwitch(QStringLiteral("clipboardHistory"));
            auto* notesSwitch = moduleSwitch(QStringLiteral("localNotes"));
            auto* qrSwitch = moduleSwitch(QStringLiteral("qrGenerator"));
            auto* barcodeSwitch = moduleSwitch(QStringLiteral("barcode"));
            auto* recording = settingsPage->findChild<QCheckBox*>(
                QStringLiteral("ClipboardRecordingSetting"));
            require(clipboardSwitch && notesSwitch && qrSwitch && barcodeSwitch && recording &&
                        clipboardSwitch->isChecked() && notesSwitch->isChecked() &&
                        qrSwitch->isChecked() && recording->isChecked(),
                    "new modules default on for existing profiles");
            const auto trayAction = [tray](const QString& key) -> QAction* {
                for (auto* action : tray->contextMenu()->actions())
                    if (action->property("requiresModule").toString() == key)
                        return action;
                return nullptr;
            };
            auto* clipboardAction = trayAction(QStringLiteral("clipboardHistory"));
            auto* notesAction = trayAction(QStringLiteral("localNotes"));
            require(clipboardAction && notesAction);
            toolbox::LocalLibrary featureData(toolbox::dataPath() + QStringLiteral("/library"));
            const auto keptClip = QStringLiteral("clipboard entry before disabling history");
            const auto ignoredClip = QStringLiteral("clipboard entry while history is disabled");
            const auto resumedClip = QStringLiteral("clipboard entry after re-enabling history");
            require(featureData.recordText(keptClip));
            const auto sampleClipboard = [](const QString& value) {
                QApplication::clipboard()->setText(value);
                QEventLoop loop;
                QTimer::singleShot(200, &loop, &QEventLoop::quit);
                loop.exec();
            };
            openQr->click();
            require(home->findChild<QWidget*>(QStringLiteral("QrGeneratorWindow"))->isVisible());
            controller.showQuickPicker(false);
            clipboardSwitch->setChecked(false);
            qrSwitch->setChecked(false);
            save->click();
            require(!openClipboard->isEnabled() && !clipboardAction->isEnabled() &&
                        !recording->isEnabled() && recording->isChecked() &&
                        openNotes->isEnabled() && notesAction->isEnabled() &&
                        !menu->isTabVisible(0) && menu->isTabVisible(1) && library->notesMode() &&
                        !openQr->isEnabled() && barcodeSwitch->isChecked(),
                    "disabling history and QR generation leaves notes and QR recognition enabled");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(!home->findChild<QWidget*>(QStringLiteral("QrGeneratorWindow")));
            require(!library->selectPage(false));
            sampleClipboard(ignoredClip);
            require(featureData.clipboard(ignoredClip).isEmpty() &&
                        featureData.clipboard(keptClip).size() == 1,
                    "disabling history stops recording and preserves earlier entries");
            notesSwitch->setChecked(false);
            save->click();
            controller.showQuickPicker(true);
            notesAction->trigger();
            require(!library->isVisible() && !openNotes->isEnabled() && !notesAction->isEnabled(),
                    "disabled notes cannot open through the quick picker or tray");
            QSettings savedModules(toolbox::settingsPath(), QSettings::IniFormat);
            require(!savedModules.value(QStringLiteral("clipboardHistory"), true).toBool() &&
                        !savedModules.value(QStringLiteral("localNotes"), true).toBool() &&
                        !savedModules.value(QStringLiteral("qrGenerator"), true).toBool() &&
                        savedModules.value(QStringLiteral("quickClipboardShortcut")).toString() ==
                            quickClipboard->keySequence().toString(QKeySequence::PortableText) &&
                        savedModules.value(QStringLiteral("quickNotesShortcut")).toString() ==
                            quickNotes->keySequence().toString(QKeySequence::PortableText),
                    "disabled modules persist without clearing shortcut preferences");
            clipboardSwitch->setChecked(true);
            qrSwitch->setChecked(true);
            save->click();
            controller.showQuickPicker(false);
            require(library->isVisible() && !library->notesMode() && menu->isTabVisible(0) &&
                        !menu->isTabVisible(1) && openQr->isEnabled() && recording->isEnabled(),
                    "clipboard and QR generator can return while notes stay disabled");
            sampleClipboard(resumedClip);
            require(featureData.clipboard(resumedClip).size() == 1 &&
                        !clips->findChild<QToolButton*>(QStringLiteral("ClipboardRowSave")),
                    "history resumes without exposing clipboard-to-note actions");
            notesSwitch->setChecked(true);
            save->click();
            require(openNotes->isEnabled() && notesAction->isEnabled() && menu->isTabVisible(1),
                    "notes become accessible again after re-enabling");
            library->close();
            QFile log(toolbox::dataPath() + QStringLiteral("/activity.log"));
            require(log.open(QIODevice::ReadOnly) && !log.readAll().isEmpty(),
                    "activity log persists locally");
            controller.presentText({});
            app.processEvents();
            auto* textWindow = home;
            for (auto* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == QStringLiteral("TextTranslationWindow"))
                    textWindow = widget;
            require(textWindow != home, "translation window opens");
            auto* direction =
                textWindow->findChild<QComboBox*>(QStringLiteral("TranslationDirection"));
            auto* translate = textWindow->findChild<QPushButton*>(QStringLiteral("TranslateText"));
            auto* originalCopy =
                textWindow->findChild<QPushButton*>(QStringLiteral("CopyOriginal"));
            auto* translationCopy =
                textWindow->findChild<QPushButton*>(QStringLiteral("CopyTranslation"));
            auto* closeText =
                textWindow->findChild<QPushButton*>(QStringLiteral("CloseTranslation"));
            require(direction && translate && originalCopy && translationCopy && closeText &&
                        direction->x() == translate->x() &&
                        originalCopy->x() == translationCopy->x() &&
                        translate->x() < originalCopy->x() && originalCopy->x() < closeText->x(),
                    "translation actions form three columns");
            QPointer<QWidget> textGuard = textWindow;
            closeText->click();
            require(!textGuard || !textGuard->isVisible(), "exit closes only translation panel");
            require(home->isVisible() && card->isVisible(), "toolbox stays running");
            home->activateWindow();
            app.processEvents();
            tabs->setCurrentIndex(2);
            shortcut->setFocus();
            app.processEvents();
            require(shortcutEditing, "focus suspends before closing panel");
            home->close();
            require(!shortcutEditing, "closing panel cannot leave shortcuts suspended");
            require(!home->isVisible());
            controller.showHome();
            require(home->isVisible());
            controller.captureActivity(true);
            require(!card->isVisible());
            controller.captureActivity(false);
            require(card->isVisible());
            card->move(50, 50);
            const auto original = card->pos();
            const QPoint local(20, 20), global = card->mapToGlobal(local), delta(20, 15);
            QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(card, &press);
            QMouseEvent move(QEvent::MouseMove, local + delta, global + delta, Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(card, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, local, global + delta, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(card, &release);
            require(card->pos() == original + delta);
            require(QSettings(toolbox::settingsPath(), QSettings::IniFormat)
                        .value(QStringLiteral("cardPosition"))
                        .toPoint() == card->pos());
            const auto freePosition = card->pos();
            styles->button(2)->click();
            save->click();
            require(QSettings(toolbox::settingsPath(), QSettings::IniFormat)
                            .value(QStringLiteral("cardStyle"))
                            .toString() == QStringLiteral("taskbar"),
                    "third taskbar style is persisted");
            require(!static_cast<toolbox::QuotaCard*>(card)->stacked(),
                    "taskbar uses compact horizontal capsule");
            const auto dockPosition = card->pos();
            QMouseEvent dockPress(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(card, &dockPress);
            QApplication::sendEvent(card, &move);
            QApplication::sendEvent(card, &release);
            require(card->pos() == dockPosition &&
                        QSettings(toolbox::settingsPath(), QSettings::IniFormat)
                                .value(QStringLiteral("cardPosition"))
                                .toPoint() == freePosition,
                    "taskbar mode cannot be dragged or overwrite free position");
            controller.captureActivity(true);
            controller.refreshUi();
            require(!card->isVisible(), "capture keeps docked capsule hidden during refresh");
            controller.captureActivity(false);
            styles->button(0)->click();
            save->click();
            require(card->isVisible() && card->height() == 36 && card->pos() == freePosition,
                    "switching back restores original free position and dimensions");
            toolbox::QuotaCard smallTaskbarCard;
            smallTaskbarCard.present({}, true, true, false, {});
            const auto normalSize = smallTaskbarCard.size();
            smallTaskbarCard.setTaskbarHeight(26);
            require(smallTaskbarCard.height() == 26 &&
                        smallTaskbarCard.width() < normalSize.width(),
                    "small taskbar scales the complete capsule proportionally");
            smallTaskbarCard.setTaskbarHeight(0);
            require(smallTaskbarCard.size() == normalSize, "free capsule dimensions are unchanged");
            int visibleMenus = 0;
            QTimer::singleShot(0, &app, [&] {
                for (auto* widget : QApplication::topLevelWidgets())
                    if (auto* menu = qobject_cast<QMenu*>(widget); menu && menu->isVisible()) {
                        ++visibleMenus;
                        menu->close();
                    }
            });
            QContextMenuEvent context(QContextMenuEvent::Mouse, local, card->mapToGlobal(local));
            QApplication::sendEvent(card, &context);
            require(visibleMenus == 1);
            if (contextVerify) {
                QFile config(profile.filePath(QStringLiteral("config.toml")));
                const QByteArray original("# preserved\nmodel = \"test-model\"\n");
                auto* toggle = home->findChild<QCheckBox*>(QStringLiteral("CodexContextToggle"));
                const auto wait = [&] {
                    QElapsedTimer timer;
                    timer.start();
                    while (!toggle->isEnabled() && timer.elapsed() < 5000)
                        QApplication::processEvents(QEventLoop::AllEvents, 20);
                    require(toggle->isEnabled());
                };
                tabs->setCurrentIndex(2);
                tabs->setCurrentIndex(0);
                wait();
                require(!toggle->isChecked());
                toggle->click();
                wait();
                require(toggle->isChecked());
                require(home->findChild<QLabel*>(QStringLiteral("ToolboxStatus"))
                            ->text()
                            .contains(QString::fromUtf8("已启用")),
                        "enable has visible feedback");
                require(config.open(QIODevice::ReadOnly));
                require(config.readAll().contains("model_context_window = 1000000"));
                config.close();
                toggle->click();
                wait();
                require(!toggle->isChecked());
                require(home->findChild<QLabel*>(QStringLiteral("ToolboxStatus"))
                            ->text()
                            .contains(QString::fromUtf8("默认")),
                        "disable has visible feedback");
                require(config.open(QIODevice::ReadOnly));
                require(config.readAll() == original);
                config.close();
            }
            app.quit();
        });
    if (app.arguments().contains(QStringLiteral("--render")))
        QTimer::singleShot(4000, &app, [&] {
            QDir().mkpath(QStringLiteral("artifacts/toolbox-ui"));
            for (auto* widget : QApplication::topLevelWidgets()) {
                const auto name = widget->objectName();
                if (name == QStringLiteral("ToolboxHome") || name == QStringLiteral("QuotaCard")) {
                    widget->grab().save(QStringLiteral("artifacts/toolbox-ui/") + name +
                                        QStringLiteral(".png"));
                    if (name == QStringLiteral("QuotaCard")) {
                        QSettings settings(toolbox::settingsPath(), QSettings::IniFormat);
                        settings.setValue(QStringLiteral("cardStyle"), QStringLiteral("stacked"));
                        settings.sync();
                        controller.refreshUi();
                        widget->grab().save(
                            QStringLiteral("artifacts/toolbox-ui/QuotaCard-stacked.png"));
                        settings.setValue(QStringLiteral("cardStyle"), QStringLiteral("compact"));
                        settings.sync();
                        controller.refreshUi();
                    }
                    if (name == QStringLiteral("ToolboxHome")) {
                        auto* values = widget->findChild<QLabel*>(QStringLiteral("QuotaValues"));
                        qInfo() << "Quota label" << values->geometry() << values->minimumHeight()
                                << values->heightForWidth(values->width());
                        widget->findChild<QTabWidget*>()->setCurrentIndex(1);
                        app.processEvents();
                        widget->grab().save(QStringLiteral("artifacts/toolbox-ui/logs.png"));
                        widget->findChild<QTabWidget*>()->setCurrentIndex(2);
                        app.processEvents();
                        widget->grab().save(QStringLiteral("artifacts/toolbox-ui/settings.png"));
                        auto* theme = widget->findChild<QComboBox*>(QStringLiteral("ToolboxThemeColor"));
                        for (int i = 0; i < theme->count(); ++i) {
                            theme->setCurrentIndex(i);
                            app.processEvents();
                            widget->grab().save(QStringLiteral("artifacts/toolbox-ui/theme-%1.png")
                                                    .arg(theme->currentData().toString()));
                        }
                        theme->setCurrentIndex(0);
                    }
                }
            }
            QTimer::singleShot(500, &app, [&] {
                for (auto* widget : QApplication::topLevelWidgets())
                    if (widget->objectName() == QStringLiteral("ToolboxHome")) {
                        auto* tabs = widget->findChild<QTabWidget*>();
                        auto* scroll = qobject_cast<QScrollArea*>(tabs->currentWidget());
                        if (scroll)
                            scroll->verticalScrollBar()->setValue(
                                scroll->verticalScrollBar()->maximum());
                        widget->grab().save(
                            QStringLiteral("artifacts/toolbox-ui/context-setting.png"));
                    }
                controller.presentText({});
                app.processEvents();
                for (auto* widget : QApplication::topLevelWidgets())
                    if (widget->objectName() == QStringLiteral("TextTranslationWindow"))
                        widget->grab().save(QStringLiteral("artifacts/toolbox-ui/translation.png"));
                toolbox::AppFeedback::show(QStringLiteral("Settings"),
                                           QStringLiteral("Saved and applied"));
                app.processEvents();
                toolbox::AppFeedback::current()->grab().save(
                    QStringLiteral("artifacts/toolbox-ui/feedback.png"));
                for (auto* widget : QApplication::topLevelWidgets())
                    if (widget->objectName() == QStringLiteral("QuotaCard")) {
                        QEvent enter(QEvent::Enter);
                        QApplication::sendEvent(widget, &enter);
                    }
                QTimer::singleShot(350, &app, [&] {
                    for (auto* widget : QApplication::topLevelWidgets())
                        if (widget->objectName() == QStringLiteral("QuotaDetail"))
                            widget->grab().save(QStringLiteral("artifacts/toolbox-ui/hover.png"));
                    app.quit();
                });
            });
        });
    return app.exec();
}
