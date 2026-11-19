// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QImage>
#include <QString>
#include <QWidget>

namespace toolbox {
// Input is UTF-8, preserved verbatim; the limit bounds synchronous encoding work.
inline constexpr int qrMaximumBytes = 1024;
QImage generateQrModules(const QString& text);
QImage renderQrImage(const QImage& modules, int pixelsPerModule);

class QrGeneratorWindow final : public QWidget {
    Q_OBJECT
  public:
    explicit QrGeneratorWindow(QWidget* parent = nullptr);
};
} // namespace toolbox
