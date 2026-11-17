// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QtWidgets>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace toolbox::AppFeedback {
inline bool isForeground() {
#ifdef Q_OS_WIN
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
#else
    return QGuiApplication::applicationState() == Qt::ApplicationActive;
#endif
}
inline QPointer<QWidget>& current() {
    static QPointer<QWidget> toast;
    return toast;
}
inline void show(const QString& title, const QString& message = {}) {
    if (current())
        current()->close();
    auto* toast =
        new QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    current() = toast;
    toast->setObjectName(QStringLiteral("ToolboxFeedback"));
    toast->setAttribute(Qt::WA_DeleteOnClose);
    toast->setAttribute(Qt::WA_ShowWithoutActivating);
    toast->setAttribute(Qt::WA_TransparentForMouseEvents);
    toast->setFocusPolicy(Qt::NoFocus);
    toast->setStyleSheet(
        QStringLiteral("QWidget#ToolboxFeedback{background:#f9f7fc;border:1px solid #d9d2e7;"
                       "border-radius:8px;} QLabel{background:transparent;border:0;"
                       "color:#334155;font:10pt 'Microsoft YaHei UI';}"));
    auto* layout = new QHBoxLayout(toast);
    layout->setContentsMargins(12, 10, 14, 10);
    layout->setSpacing(9);
    auto* avatar = new QLabel;
    avatar->setFixedSize(26, 26);
    avatar->setPixmap(QPixmap(QStringLiteral(":/toolbox/dragon/avatar.png"))
                          .scaled(26, 26, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(avatar);
    auto* text = new QLabel(message.isEmpty() ? title : title + QLatin1Char('\n') + message);
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* host = isForeground() ? QApplication::activeWindow() : nullptr;
    auto* screen = host ? host->screen() : QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const auto bounds = screen->availableGeometry();
    text->setMaximumWidth(qMax(100, qMin(350, bounds.width() - 90)));
    toast->setMinimumSize(qMin(280, bounds.width() - 24), 52);
    toast->adjustSize();
    QPoint position = host ? host->mapToGlobal(QPoint((host->width() - toast->width()) / 2,
                                                      host->height() - toast->height() - 24))
                           : QPoint(bounds.center().x() - toast->width() / 2,
                                    bounds.bottom() - toast->height() - 48);
    position.setX(qBound(bounds.left(), position.x(), bounds.right() - toast->width() + 1));
    position.setY(qBound(bounds.top(), position.y(), bounds.bottom() - toast->height() + 1));
    toast->move(position);
    toast->show();
    QTimer::singleShot(2500, toast, &QWidget::close);
}
inline void notify(QSystemTrayIcon* tray, const QString& title, const QString& message,
                   QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::Information) {
    if (isForeground())
        show(title, message);
    else
        tray->showMessage(title, message, icon);
}
} // namespace toolbox::AppFeedback
