// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDir>
#include <QStandardPaths>
#include <QSettings>

namespace toolbox {
inline QString dataPath() {
    const QString root = qEnvironmentVariable(
        "SNOW_TOOLBOX_DATA", QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                                 QStringLiteral("/SnowToolbox"));
    QDir().mkpath(root);
    return root;
}
inline QString settingsPath() {
    return dataPath() + QStringLiteral("/toolbox.ini");
}
inline bool enabled(const QString& name) {
    return QSettings(settingsPath(), QSettings::IniFormat).value(name, true).toBool();
}
} // namespace toolbox
