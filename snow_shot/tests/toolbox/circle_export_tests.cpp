// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include <QGuiApplication>
#include <QBuffer>
#include <cstdlib>
#include <iostream>

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const auto path = screenshotCircleRegionPath({-150, -80}, {-50, 20}, {-200, -100, 800, 600});
    const auto region =
        ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Circle).translated(150, 80);
    for (qreal scale : {1.0, 1.25, 1.5, 2.0}) {
        QImage pixels(qRound(100 * scale), qRound(100 * scale),
                      QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::blue);
        ScreenshotResultStyle style;
        style.region = region;
        style = ScreenshotResultCompositor::scaledStyle(style, scale);
        const auto result = ScreenshotResultCompositor::compose(pixels, style);
        require(!result.isNull(), "circle composition result");
        require(result.pixelColor(0, 0).alpha() == 0 &&
                    result.pixelColor(result.width() / 2, result.height() / 2).blue() == 255,
                "real compositor circle transparency");
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        require(result.save(&buffer, "PNG"), "circle PNG encode");
        const auto decoded = QImage::fromData(bytes, "PNG");
        require(decoded.pixelColor(0, 0).alpha() == 0 && decoded.size() == result.size(),
                "PNG retains circle mask");
    }
    std::cout << "PASS: circle compositor and PNG at four scales\n";
}
