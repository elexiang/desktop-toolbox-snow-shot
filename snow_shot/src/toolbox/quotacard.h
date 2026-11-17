// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "quotaservice.h"
#include <QWidget>
#include <QPainter>
#include <QFontMetrics>

namespace toolbox {
class QuotaCard final : public QWidget {
  public:
    explicit QuotaCard()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                               Qt::NoDropShadowWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
    }
    void present(const QList<QuotaWindow>& windows, bool stale, bool enabled, bool stacked,
                 const QString& unavailable) {
        m_stale = stale;
        m_enabled = enabled;
        m_stacked = stacked;
        m_unavailable = unavailable;
        m_windows = windows.mid(0, 2);
        while (m_windows.size() < 2)
            m_windows.append(
                {{}, -1, {}, m_windows.isEmpty() ? QStringLiteral("5h") : QStringLiteral("w")});
        setAccessibleDescription(summary());
        setFixedSize(sizeHint());
        update();
    }
    bool stacked() const {
        return m_stacked;
    }
    QString summary() const {
        if (!m_enabled)
            return m_unavailable;
        QStringList lines;
        for (const auto& window : m_windows)
            lines << window.compactTitle + QStringLiteral(" ") + percent(window) +
                         QStringLiteral(" / ") + reset(window);
        return lines.join(QStringLiteral("; "));
    }
    QSize sizeHint() const override {
        if (!m_enabled)
            return {std::max(150, QFontMetrics(font(10)).horizontalAdvance(m_unavailable) + 28),
                    36};
        if (m_stacked)
            return {std::max(groupWidth(0), groupWidth(1)), 112};
        return {groupWidth(0) + groupWidth(1) + 1, 36};
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing);
        if (!m_enabled) {
            surface(painter, rect(), 18);
            painter.setPen(QColor(185, 198, 218));
            painter.setFont(font(10));
            painter.drawText(rect(), Qt::AlignCenter, m_unavailable);
            return;
        }
        if (m_stacked) {
            surface(painter, rect(), 26);
            painter.setPen(QColor(65, 81, 108));
            painter.drawLine(16, 56, width() - 16, 56);
        } else {
            surface(painter, rect(), 18);
            painter.setPen(QColor(65, 81, 108));
            painter.drawLine(groupWidth(0), 9, groupWidth(0), height() - 9);
        }
        int left = 0;
        for (int i = 0; i < 2; ++i) {
            const auto& window = m_windows.at(i);
            const QColor accent = i == 0 ? QColor(98, 220, 230) : QColor(186, 161, 255);
            const QColor number = m_stale ? QColor(142, 157, 181) : accent;
            const int top = m_stacked ? i * 56 : 0;
            painter.setFont(font(10, true));
            painter.setPen(accent.lighter(120));
            painter.drawText(left + 12, top + 23, window.compactTitle);
            const int percentageX =
                left + 12 + QFontMetrics(font(10, true)).horizontalAdvance(window.compactTitle) + 8;
            painter.setFont(font(m_stacked ? 13 : 11, true));
            painter.setPen(number);
            painter.drawText(percentageX, top + 23, percent(window));
            int resetX =
                m_stacked ? 12
                          : percentageX +
                                QFontMetrics(font(11, true)).horizontalAdvance(percent(window)) + 9;
            const int baseline = top + (m_stacked ? 43 : 23);
            painter.setFont(font(9));
            painter.setPen(QColor(161, 177, 199));
            painter.drawArc(QRectF(resetX, baseline - 9, 8, 8), 40 * 16, 290 * 16);
            painter.drawLine(resetX + 8, baseline - 9, resetX + 8, baseline - 5);
            painter.drawLine(resetX + 8, baseline - 5, resetX + 4, baseline - 6);
            painter.drawText(resetX + 13, baseline, reset(window));
            if (m_stale) {
                painter.setPen(QColor(239, 184, 91));
                painter.drawText(m_stacked ? width() - 14 : left + groupWidth(i) - 11, top + 23,
                                 QStringLiteral("!"));
            }
            left += groupWidth(i) + 1;
            if (m_stacked)
                left = 0;
        }
    }

  private:
    static QFont font(int points, bool bold = false) {
        QFont result(QStringLiteral("Microsoft YaHei UI"), points);
        result.setWeight(bold ? QFont::DemiBold : QFont::Normal);
        return result;
    }
    static QString percent(const QuotaWindow& window) {
        return window.remaining < 0
                   ? QStringLiteral("--")
                   : QString::number(window.remaining, 'f', 0) + QStringLiteral("%");
    }
    static QString reset(const QuotaWindow& window) {
        return !window.resets.isValid()
                   ? QStringLiteral("--")
                   : window.resets.toString(window.compactTitle == QStringLiteral("5h") &&
                                                    window.resets.date() == QDate::currentDate()
                                                ? QStringLiteral("HH:mm")
                                                : QStringLiteral("MM-dd HH:mm"));
    }
    int groupWidth(int index) const {
        if (index >= m_windows.size())
            return 150;
        const auto& window = m_windows.at(index);
        const int titleWidth =
            QFontMetrics(font(10, true)).horizontalAdvance(window.compactTitle) + 8 +
            QFontMetrics(font(m_stacked ? 13 : 11, true)).horizontalAdvance(percent(window));
        const int resetWidth = 13 + QFontMetrics(font(9)).horizontalAdvance(reset(window));
        if (m_stacked)
            return 12 + std::max(titleWidth, resetWidth) + (m_stale ? 22 : 12);
        return 12 + QFontMetrics(font(10, true)).horizontalAdvance(window.compactTitle) + 8 +
               QFontMetrics(font(11, true)).horizontalAdvance(percent(window)) + 9 + 13 +
               QFontMetrics(font(9)).horizontalAdvance(reset(window)) + (m_stale ? 22 : 12);
    }
    static void surface(QPainter& painter, QRectF area, int radius) {
        QLinearGradient gradient(area.topLeft(), area.bottomRight());
        gradient.setColorAt(0, QColor(33, 48, 70));
        gradient.setColorAt(1, QColor(20, 29, 47));
        painter.setPen(QPen(QColor(75, 95, 124, 180), 1));
        painter.setBrush(gradient);
        painter.drawRoundedRect(area.adjusted(.5, .5, -.5, -.5), radius, radius);
    }
    QList<QuotaWindow> m_windows;
    QString m_unavailable;
    bool m_stale = true, m_enabled = true, m_stacked = false;
};
} // namespace toolbox
