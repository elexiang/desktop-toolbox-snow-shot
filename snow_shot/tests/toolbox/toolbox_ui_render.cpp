// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolboxcontroller.h"
#include "appfeedback.h"
#include "toolboxpaths.h"
#include <QApplication>
#include <QTranslator>
#include <QDir>
#include <QTimer>
#include <QPushButton>
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
#include <cstdlib>
#include <cstdio>
#include <source_location>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
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
    const bool verify = contextVerify || app.arguments().contains(QStringLiteral("--verify"));
    if (verify || app.arguments().contains(QStringLiteral("--render"))) {
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
    if (translator.load(app.applicationDirPath() + QStringLiteral("/toolbox_zh_CN.qm")))
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
            require(settingsPage->layout()->itemAt(0)->widget()->objectName() ==
                        QStringLiteral("ToolboxAutoStart"),
                    "startup is first setting");
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
            require(home->size() == QSize(900, 820), "original panel dimensions");
            require(home->findChild<QLabel*>(QStringLiteral("DragonAccent"))->size() ==
                        QSize(36, 36), "dragon remains a small title accent");
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
                            widget->grab().save(
                                QStringLiteral("artifacts/toolbox-ui/hover.png"));
                    app.quit();
                });
            });
        });
    return app.exec();
}
