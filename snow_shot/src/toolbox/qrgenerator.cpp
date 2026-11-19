// SPDX-License-Identifier: GPL-3.0-or-later
#include "qrgenerator.h"
#include <QtWidgets>
#include <QSaveFile>
#include <opencv2/objdetect.hpp>
#include <cstring>
#include <memory>

namespace toolbox {
QImage generateQrModules(const QString& text) {
    const auto utf8 = text.toUtf8();
    if (text.isEmpty() || utf8.size() > qrMaximumBytes || text.contains(QChar::Null))
        return {};
    try {
        cv::QRCodeEncoder::Params params;
        params.correction_level = cv::QRCodeEncoder::CORRECT_LEVEL_M;
        params.mode = cv::QRCodeEncoder::MODE_ECI;
        cv::Mat encoded;
        cv::QRCodeEncoder::create(params)->encode(
            std::string(utf8.constData(), static_cast<size_t>(utf8.size())), encoded);
        if (encoded.empty() || encoded.type() != CV_8UC1 || encoded.rows != encoded.cols)
            return {};
        // Retain the encoder's border and add four white modules on every side.
        QImage modules(encoded.cols + 8, encoded.rows + 8, QImage::Format_Grayscale8);
        modules.fill(255);
        for (int y = 0; y < encoded.rows; ++y)
            std::memcpy(modules.scanLine(y + 4) + 4, encoded.ptr(y),
                        static_cast<size_t>(encoded.cols));
        return modules;
    } catch (const cv::Exception&) {
        return {};
    }
}

QImage renderQrImage(const QImage& modules, int pixelsPerModule) {
    if (modules.isNull() || pixelsPerModule < 1 || pixelsPerModule > 16)
        return {};
    return modules.scaled(modules.size() * pixelsPerModule, Qt::IgnoreAspectRatio,
                          Qt::FastTransformation);
}

QrGeneratorWindow::QrGeneratorWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("QrGeneratorWindow"));
    setWindowTitle(tr("Generate QR code"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(660, 440);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);
    auto* columns = new QHBoxLayout;
    root->addLayout(columns, 1);
    auto* inputColumn = new QVBoxLayout;
    columns->addLayout(inputColumn, 1);
    auto* caption = new QLabel(tr("Text or URL"));
    inputColumn->addWidget(caption);
    auto* input = new QPlainTextEdit;
    input->setObjectName(QStringLiteral("QrInput"));
    input->setAccessibleName(caption->text());
    input->setPlaceholderText(tr("Enter text or a URL (up to %1 UTF-8 bytes)").arg(qrMaximumBytes));
    inputColumn->addWidget(input, 1);
    auto* counter = new QLabel;
    counter->setObjectName(QStringLiteral("QrByteCount"));
    inputColumn->addWidget(counter);
    auto* preview = new QLabel(tr("QR code preview"));
    preview->setObjectName(QStringLiteral("QrPreview"));
    preview->setAccessibleName(tr("QR code preview"));
    preview->setFixedSize(300, 300);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet(
        QStringLiteral("background:white;color:#64748b;border:1px solid #d8dfea;"));
    columns->addWidget(preview, 0, Qt::AlignCenter);
    auto* status = new QLabel(tr("Generated locally. Input is not saved or uploaded"));
    status->setObjectName(QStringLiteral("QrStatus"));
    status->setWordWrap(true);
    root->addWidget(status);
    auto* actions = new QHBoxLayout;
    root->addLayout(actions);
    auto* generate = new QPushButton(tr("Generate QR code"));
    generate->setObjectName(QStringLiteral("QrGenerate"));
    auto* copy = new QPushButton(tr("Copy image"));
    copy->setObjectName(QStringLiteral("QrCopy"));
    auto* save = new QPushButton(tr("Save PNG…"));
    save->setObjectName(QStringLiteral("QrSave"));
    actions->addWidget(generate);
    actions->addWidget(copy);
    actions->addWidget(save);
    actions->addStretch();
    auto* close = new QPushButton(tr("Close"));
    actions->addWidget(close);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    auto image = std::make_shared<QImage>();
    const auto invalidate = [=, this] {
        *image = {};
        preview->setText(tr("QR code preview"));
        copy->setEnabled(false);
        save->setEnabled(false);
        const auto text = input->toPlainText();
        const auto bytes = text.toUtf8().size();
        counter->setText(tr("%1 / %2 UTF-8 bytes").arg(bytes).arg(qrMaximumBytes));
        const bool valid =
            !text.isEmpty() && bytes <= qrMaximumBytes && !text.contains(QChar::Null);
        generate->setEnabled(valid);
        status->setText(
            bytes > qrMaximumBytes       ? tr("Input is too long; shorten it to generate a QR code")
            : text.contains(QChar::Null) ? tr("Input contains an unsupported null character")
                                         : tr("Generated locally. Input is not saved or uploaded"));
    };
    connect(input, &QPlainTextEdit::textChanged, this, invalidate);
    connect(generate, &QPushButton::clicked, this, [=, this] {
        const auto modules = generateQrModules(input->toPlainText());
        *image = renderQrImage(modules, 8);
        if (image->isNull()) {
            status->setText(tr("QR code generation failed; try shorter text"));
            return;
        }
        preview->setPixmap(QPixmap::fromImage(renderQrImage(modules, 296 / modules.width())));
        copy->setEnabled(true);
        save->setEnabled(true);
        status->setText(tr("Ready · %1 × %2 px").arg(image->width()).arg(image->height()));
    });
    connect(copy, &QPushButton::clicked, this, [=, this] {
        if (!image->isNull()) {
            QApplication::clipboard()->setImage(*image);
            status->setText(tr("QR code image copied"));
        }
    });
    connect(save, &QPushButton::clicked, this, [=, this] {
        if (image->isNull())
            return;
        QFileDialog dialog(this, tr("Save QR code"));
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setNameFilter(tr("PNG image (*.png)"));
        dialog.setDefaultSuffix(QStringLiteral("png"));
        dialog.selectFile(QStringLiteral("qrcode.png"));
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
            return;
        const auto path = dialog.selectedFiles().first();
        QSaveFile file(path);
        const bool saved =
            file.open(QIODevice::WriteOnly) && image->save(&file, "PNG") && file.commit();
        status->setText(saved ? tr("QR code image saved") : tr("Could not save the QR code image"));
    });
    invalidate();
}
} // namespace toolbox
