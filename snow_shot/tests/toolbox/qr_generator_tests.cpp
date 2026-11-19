// SPDX-License-Identifier: GPL-3.0-or-later
#include "qrgenerator.h"
#include <QtWidgets>
#include <QTemporaryDir>
#include <opencv2/objdetect.hpp>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
QString decode(const QImage& image) {
    auto gray = image.convertToFormat(QImage::Format_Grayscale8);
    cv::Mat view(gray.height(), gray.width(), CV_8UC1, gray.bits(), gray.bytesPerLine());
    const auto value = cv::QRCodeDetector().detectAndDecode(view);
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
} // namespace

int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    QTranslator translator;
    require(translator.load(app.applicationDirPath() + QStringLiteral("/toolbox_zh_CN.qm")),
            "Chinese UI catalog must load");
    app.installTranslator(&translator);
    require(toolbox::generateQrModules({}).isNull(), "Empty input must be rejected");
    require(toolbox::generateQrModules(QString(1025, QLatin1Char('a'))).isNull(),
            "Oversized input must be rejected");
    require(toolbox::generateQrModules(QStringLiteral("a") + QChar::Null).isNull(),
            "Embedded null must be rejected");
    require(toolbox::generateQrModules(QString(342, QChar(0x4e2d))).isNull(),
            "Length limit must count UTF-8 bytes");
    const QStringList payloads{QStringLiteral("https://example.com/path?q=hello&n=1"),
                               QString::fromUtf8("中文二维码\n第二行 😀"),
                               QStringLiteral("  leading and trailing spaces  \n"),
                               QStringLiteral("1234567890"), QString(1024, QLatin1Char('x'))};
    QTemporaryDir directory;
    require(directory.isValid(), "Temporary directory must exist");
    for (const auto& payload : payloads) {
        const auto modules = toolbox::generateQrModules(payload);
        require(!modules.isNull(), "Valid content must generate a QR code");
        for (int i = 0; i < modules.width(); ++i)
            require(modules.constScanLine(0)[i] == 255 && modules.constScanLine(i)[0] == 255,
                    "QR code must retain a white quiet zone");
        const auto image = toolbox::renderQrImage(modules, 8);
        require(decode(image) == payload, "Generated image must decode to the exact input");
        const auto path = directory.filePath(QStringLiteral("qr.png"));
        require(image.save(path, "PNG"), "PNG export must succeed");
        require(decode(QImage(path)) == payload, "Saved PNG must decode without data loss");
    }
    toolbox::QrGeneratorWindow window;
    require(window.windowTitle() == QString::fromUtf8("生成二维码"),
            "QR generator must display its Chinese title");
    window.show();
    app.processEvents();
    auto* input = window.findChild<QPlainTextEdit*>(QStringLiteral("QrInput"));
    auto* generate = window.findChild<QPushButton*>(QStringLiteral("QrGenerate"));
    auto* copy = window.findChild<QPushButton*>(QStringLiteral("QrCopy"));
    auto* save = window.findChild<QPushButton*>(QStringLiteral("QrSave"));
    require(input && generate && copy && save, "QR window controls must exist");
    require(!generate->isEnabled() && !copy->isEnabled() && !save->isEnabled(),
            "Empty state must disable generation and export");
    input->setPlainText(payloads[1]);
    generate->click();
    require(copy->isEnabled() && save->isEnabled(), "Generation must enable export");
    copy->click();
    require(decode(QApplication::clipboard()->image()) == payloads[1],
            "Clipboard must contain the generated image");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        require(dialog, "Save action must open a file dialog");
        dialog->selectFile(directory.filePath(QStringLiteral("ui-export")));
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    save->click();
    require(decode(QImage(directory.filePath(QStringLiteral("ui-export.png")))) == payloads[1],
            "Save action must write a decodable PNG and add the default suffix");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        require(dialog, "Save dialog must reopen");
        dialog->reject();
    });
    save->click();
    require(copy->isEnabled() && save->isEnabled(), "Canceling save must retain the image");
    input->setPlainText(QStringLiteral("changed"));
    require(!copy->isEnabled() && !save->isEnabled(), "Editing must invalidate stale output");
    input->setPlainText(QString(1025, QLatin1Char('x')));
    require(!generate->isEnabled(), "Oversized content must disable generation");
    input->setPlainText(payloads[0]);
    generate->click();
    if (app.arguments().contains(QStringLiteral("--render"))) {
        const auto output = app.arguments().last();
        require(window.grab().save(output), "QR preview capture must save");
    }
    // Stack-owned fixture: production windows are heap-owned and delete on close.
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    std::cout << "QR generator round-trip, PNG, clipboard and UI state checks passed\n";
    return 0;
}
