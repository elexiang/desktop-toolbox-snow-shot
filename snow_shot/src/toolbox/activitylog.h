// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QStringList>
#include <algorithm>

namespace toolbox {
inline QStringList activityLines(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}
inline QString recentActivity(const QString& path) {
    auto lines = activityLines(path);
    std::reverse(lines.begin(), lines.end());
    std::stable_sort(lines.begin(), lines.end(),
                     [](const QString& a, const QString& b) { return a.left(19) > b.left(19); });
    return lines.mid(0, 500).join(QLatin1Char('\n'));
}
inline bool pruneActivity(const QString& path, int days, const QDateTime& now) {
    if (days <= 0)
        return true;
    const auto lines = activityLines(path);
    QStringList retained;
    const auto cutoff = now.addDays(-days);
    for (const auto& line : lines) {
        const auto date =
            QDateTime::fromString(line.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (!date.isValid() || date >= cutoff)
            retained.append(line);
    }
    if (lines == retained)
        return true;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const auto bytes = retained.isEmpty()
                           ? QByteArray()
                           : (retained.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    return file.write(bytes) == bytes.size() && file.commit();
}
} // namespace toolbox
