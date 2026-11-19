// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QStringList>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <algorithm>

namespace toolbox {
inline QString windowsPopupDiagnostic(const QString& xml, const QString& executable) {
    QXmlStreamReader reader(xml);
    QString caption, message, record, timestamp;
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (reader.name() == QStringLiteral("EventRecordID"))
            record = reader.readElementText();
        else if (reader.name() == QStringLiteral("TimeCreated"))
            timestamp = reader.attributes().value(QStringLiteral("SystemTime")).toString();
        else if (reader.name() == QStringLiteral("Data")) {
            const auto field = reader.attributes().value(QStringLiteral("Name")).toString();
            if (field == QStringLiteral("Caption"))
                caption = reader.readElementText();
            else if (field == QStringLiteral("Message"))
                message = reader.readElementText();
        }
    }
    static const QRegularExpression toolboxName(QStringLiteral(
        R"((?:^|:\s*)SnowToolbox(?:-[A-Za-z0-9_-]+)?\.exe\s+-)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto appName = QRegularExpression(
        QStringLiteral(R"((?:^|:\s*)%1\s+-)").arg(QRegularExpression::escape(executable)),
        QRegularExpression::CaseInsensitiveOption);
    if (reader.hasError() || record.toULongLong() == 0 ||
        (!toolboxName.match(caption).hasMatch() && !appName.match(caption).hasMatch()))
        return {};
    static const QRegularExpression dllName(QStringLiteral(R"([\\/]([^\\/\r\n:<>|"?*]{1,128}\.dll)\b)"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression errorCode(QStringLiteral(R"(\b0x[0-9a-f]{8}\b)"),
                                              QRegularExpression::CaseInsensitiveOption);
    const auto dll = dllName.match(message);
    const auto code = errorCode.match(message);
    const auto time = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    if (!dll.hasMatch() || !code.hasMatch() || !time.isValid())
        return {};
    return QStringLiteral("[Windows.ApplicationPopup] record=%1 at=%2 module=%3 code=%4")
        .arg(record, time.toLocalTime().toString(Qt::ISODateWithMs), dll.captured(1),
             code.captured().toLower());
}

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
