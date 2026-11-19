// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>

namespace toolbox {
inline void setLibraryClipboard(const QString& text, const QImage& image, const QString& html) {
    auto* data = new QMimeData;
    if (!text.isEmpty())
        data->setText(text);
    if (!image.isNull())
        data->setImageData(image);
    if (!html.isEmpty())
        data->setHtml(html);
    QGuiApplication::clipboard()->setMimeData(data);
}
} // namespace toolbox
