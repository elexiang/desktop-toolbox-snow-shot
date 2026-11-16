// SPDX-License-Identifier: GPL-3.0-or-later
#include "quotaservice.h"
#include "offlinetranslator.h"
#include "memoryservice.h"
#include "toolboxpaths.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/image/screenshotregiongeometry.h"
#include <QPainter>
#include <QApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QRegularExpression>
#include <iostream>
#include <cstdlib>

void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    qputenv("SNOW_TOOLBOX_DATA", temporary.path().toUtf8());
    const QRectF circleBounds(-200, -100, 800, 600);
    for (const auto& pointer : {QPointF(100, 50), QPointF(-100, -80), QPointF(900, 900)}) {
        const auto path = screenshotCircleRegionPath(QPointF(0, 0), pointer, circleBounds);
        const auto bounds = path.boundingRect();
        require(bounds.width() == bounds.height() && circleBounds.contains(bounds),
                "circle constrained and bounded");
        require(path.contains(bounds.center()) && !path.contains(bounds.topLeft()),
                "circle excludes transparent corners");
        const auto region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Circle);
        const auto restored = ScreenshotRegionGeometry::fromJson(region.toJson());
        require(restored && *restored == region, "circle region persistence");
        QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setClipPath(screenshotCircleRegionPath({0, 0}, {100, 100}, {0, 0, 100, 100}));
        painter.fillRect(image.rect(), Qt::blue);
        painter.end();
        require(qAlpha(image.pixel(0, 0)) == 0 && qAlpha(image.pixel(50, 50)) == 255,
                "circle export mask transparency");
    }
    ScreenshotColorPickerContext picker;
    picker.active = true;
    picker.intelligentSelecting = true;
    picker.selectionPixels = QRect(10, 10, 400, 200);
    require(screenshotColorPickerPreviewAllowed(picker),
            "preview before confirming smart selection");
    picker.intelligentSelecting = false;
    picker.manualSelecting = true;
    picker.dragging = true;
    picker.dragMode = ScreenshotSelectionDragMode::Marquee;
    require(screenshotColorPickerPreviewAllowed(picker), "preview during initial marquee drag");
    picker.manualSelecting = false;
    picker.dragging = false;
    picker.movingSelection = true;
    require(!screenshotColorPickerPreviewAllowed(picker),
            "hide preview immediately after selection");
    picker.manualSelecting = true;
    picker.dragging = true;
    picker.dragMode = ScreenshotSelectionDragMode::BottomRight;
    require(!screenshotColorPickerPreviewAllowed(picker), "hide preview while resizing selection");
    picker.manualSelecting = false;
    picker.movingSelection = false;
    require(!screenshotColorPickerPreviewAllowed(picker), "hide preview during annotation");
    const auto input =
        QJsonDocument::fromJson(
            R"({"codex":{"primary":{"usedPercent":5,"windowDurationMins":300,"resetsAt":1794641400},"secondary":{"usedPercent":29,"windowDurationMins":10080,"resetsAt":1794989386}},"other":{"primary":{"usedPercent":"unknown"}}})")
            .object();
    auto windows = toolbox::QuotaService::parse(input);
    require(windows.size() == 2, "invalid percentage must not become fake quota");
    require(windows[0].remaining == 95 && windows[1].remaining == 71, "remaining percentage");
    require(windows[0].compactTitle == QStringLiteral("5h") &&
                windows[1].compactTitle == QStringLiteral("w"),
            "compact quota labels");
    require(windows[0].resets.isValid(), "quota reset timestamp");
    require(toolbox::QuotaService::parse({}).isEmpty(), "empty quota stays empty");
    require(toolbox::OfflineTranslator::direction(QStringLiteral("Hello"), {}) ==
                QStringLiteral("en_zh"),
            "English routing");
    require(toolbox::OfflineTranslator::direction(QString::fromUtf8("你好"), {}) ==
                QStringLiteral("zh_en"),
            "Chinese routing");
    require(toolbox::OfflineTranslator::direction(QString::fromUtf8("标题 Hello world example"),
                                                  {}) == QStringLiteral("en_zh"),
            "mixed text follows the dominant language");
    require(toolbox::MemoryService::runDeepHelper(QStringLiteral("invalid")) == 64,
            "reject invalid helper request without cleaning");
    const auto memory = toolbox::MemoryService::collect();
    require(memory.total > 0 && memory.privateWorking > 0 && memory.ownedProcesses > 0,
            "whole owned tree memory counters");
    toolbox::QuotaService quota;
    require(quota.stale() && quota.windows().isEmpty(), "new service must not invent quota");
    auto& translator = toolbox::OfflineTranslator::instance();
    int calls = 0;
    auto id = translator.translate(QStringLiteral("cancelled"), {}, &app,
                                   [&](QString, QString) { ++calls; });
    translator.cancel(id);
    QEventLoop cancelled;
    QTimer::singleShot(100, &cancelled, &QEventLoop::quit);
    cancelled.exec();
    require(calls == 0 && translator.processId() == 0,
            "cancel queued translation without spawning a worker");
    QSettings(toolbox::settingsPath(), QSettings::IniFormat)
        .setValue(QStringLiteral("translation"), false);
    QEventLoop disabled;
    QString disabledError;
    translator.translate(QStringLiteral("Hello"), {}, &app, [&](QString, QString error) {
        disabledError = error;
        disabled.quit();
    });
    disabled.exec();
    require(!disabledError.isEmpty() && translator.processId() == 0,
            "disabled translation does not start engine");
    QSettings(toolbox::settingsPath(), QSettings::IniFormat)
        .setValue(QStringLiteral("translation"), true);
    if (app.arguments().contains(QStringLiteral("--integration"))) {
        for (const QString& source :
             {QStringLiteral("Hello, world!"), QString::fromUtf8("这是一个本地翻译测试。")}) {
            QEventLoop loop;
            QString translated, error;
            translator.translate(source, {}, &app, [&](QString text, QString failure) {
                translated = text;
                error = failure;
                loop.quit();
            });
            QTimer::singleShot(125000, &loop, &QEventLoop::quit);
            loop.exec();
            require(error.isEmpty() && !translated.trimmed().isEmpty() && translated != source,
                    "real offline bilingual inference");
            std::cout << "offline: " << translated.toUtf8().constData() << '\n';
        }
        translator.releaseIdle();
        require(translator.processId() == 0, "release idle model process");
    }
    if (app.arguments().contains(QStringLiteral("--quota"))) {
        QEventLoop loop;
        bool received = false;
        QObject::connect(&quota, &toolbox::QuotaService::changed, &loop, [&] {
            if (!quota.stale() && !quota.windows().isEmpty()) {
                received = true;
                loop.quit();
            }
        });
        quota.setEnabled(true);
        QTimer::singleShot(25000, &loop, &QEventLoop::quit);
        loop.exec();
        require(received, "real local Codex CLI quota query");
        quota.setEnabled(false);
    }
    translator.stop();
    std::cout << "PASS: quota parsing, cancellation, module shutdown, memory accounting" << '\n';
    return 0;
}
