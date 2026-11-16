// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolboxcontroller.h"
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
#include <cstdlib>
#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
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
    if (verify) {
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
        settings.setValue(QStringLiteral("memory"), false);
        settings.setValue(QStringLiteral("translation"), false);
    }
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    app.setQuitOnLastWindowClosed(false);
    QTranslator translator;
    if (translator.load(app.applicationDirPath() + QStringLiteral("/toolbox_zh_CN.qm")))
        app.installTranslator(&translator);
    toolbox::ToolboxController controller({[] {}, [] {}, [] {}, [] {}, [] {}}, &app);
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
            const auto require = [](bool value, const char* message = "UI state assertion failed") {
                if (!value) {
                    std::fprintf(stderr, "%s\n", message);
                    std::exit(1);
                }
            };
            require(home && card && home->isVisible() && card->isVisible());
            require(!QImage(QStringLiteral(":/toolbox/reset.svg")).isNull(),
                    "reset icon must load");
            auto* cardValues = card->findChild<QLabel*>(QStringLiteral("QuotaCardValues"));
            require(cardValues && cardValues->text().contains(QStringLiteral("5h")) &&
                        cardValues->text().contains(QStringLiteral("w")) &&
                        cardValues->text().contains(QStringLiteral("reset.svg")) &&
                        !cardValues->text().contains(QStringLiteral("Codex")),
                    "compact quota content");
            require(!cardValues->wordWrap() && card->height() <= 36 &&
                        cardValues->contentsRect().width() >= cardValues->sizeHint().width(),
                    "compact card geometry");
            home->close();
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
                require(config.open(QIODevice::WriteOnly));
                const QByteArray original("# preserved\nmodel = \"test-model\"\n");
                config.write(original);
                config.close();
                auto* toggle = home->findChild<QCheckBox*>(QStringLiteral("CodexContextToggle"));
                const auto wait = [&] {
                    QElapsedTimer timer;
                    timer.start();
                    while (!toggle->isEnabled() && timer.elapsed() < 5000)
                        QApplication::processEvents(QEventLoop::AllEvents, 20);
                    require(toggle->isEnabled());
                };
                home->findChild<QTabWidget*>()->setCurrentIndex(1);
                wait();
                require(!toggle->isChecked());
                toggle->click();
                wait();
                require(toggle->isChecked());
                require(config.open(QIODevice::ReadOnly));
                require(config.readAll().contains("model_context_window = 1000000"));
                config.close();
                toggle->click();
                wait();
                require(!toggle->isChecked());
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
                    if (name == QStringLiteral("ToolboxHome")) {
                        auto* values = widget->findChild<QLabel*>(QStringLiteral("QuotaValues"));
                        qInfo() << "Quota label" << values->geometry() << values->minimumHeight()
                                << values->heightForWidth(values->width());
                        widget->findChild<QTabWidget*>()->setCurrentIndex(1);
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
                app.quit();
            });
        });
    return app.exec();
}
