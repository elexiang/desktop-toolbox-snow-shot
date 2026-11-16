// SPDX-License-Identifier: GPL-3.0-or-later
#include "quotaservice.h"
#include "toolboxpaths.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QDirIterator>
#include <algorithm>

namespace toolbox {
QuotaService::QuotaService(QObject* parent) : QObject(parent) {
    QSettings cached(settingsPath(), QSettings::IniFormat);
    m_buckets =
        QJsonDocument::fromJson(cached.value(QStringLiteral("quotaCache")).toByteArray()).object();
    m_updatedAt = cached.value(QStringLiteral("quotaUpdatedAt")).toDateTime();
    m_refresh.setInterval(60000);
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(20000);
    m_retire.setSingleShot(true);
    m_retire.setInterval(1500);
    connect(&m_refresh, &QTimer::timeout, this, &QuotaService::refresh);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail(tr("Quota query timed out; previous data is stale"));
        retire();
    });
    connect(&m_retire, &QTimer::timeout, this, &QuotaService::retire);
    connect(&m_process, &QProcess::started, this,
            [this] { send(QStringLiteral("initialize"), 1); });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &QuotaService::read);
    connect(&m_process, &QProcess::readyReadStandardError, this,
            [this] { m_process.readAllStandardError(); });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_closing && error == QProcess::FailedToStart)
            fail(tr("Cannot start Codex CLI. Select codex.exe in Settings"));
    });
    connect(&m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (!m_closing && m_pending)
            fail(tr("The quota query stopped. Check your CLI login"));
    });
}
QuotaService::~QuotaService() {
    shutdown();
}
void QuotaService::setEnabled(bool enabledValue) {
    if (m_enabled == enabledValue)
        return;
    m_enabled = enabledValue;
    if (!m_enabled) {
        shutdown();
        m_status = tr("Quota queries are disabled");
        emit changed();
    } else {
        m_refresh.start();
        QTimer::singleShot(0, this, &QuotaService::refresh);
    }
}
void QuotaService::retire() {
    m_retire.stop();
    m_closing = true;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1500);
    }
    m_closing = false;
    m_buffer.clear();
}
void QuotaService::shutdown() {
    m_refresh.stop();
    m_deadline.stop();
    m_pending = false;
    retire();
}
QString QuotaService::discover() {
    auto direct = QStandardPaths::findExecutable(QStringLiteral("codex.exe"));
    if (!direct.isEmpty())
        return direct;
    QStringList roots{qEnvironmentVariable("APPDATA") + QStringLiteral("/npm"),
                      QStringLiteral("E:/DevCaches/npm-global")};
    const auto wrapper = QStandardPaths::findExecutable(QStringLiteral("codex.cmd"));
    if (!wrapper.isEmpty())
        roots.prepend(QFileInfo(wrapper).absolutePath());
    for (const auto& root : roots) {
        QDirIterator it(root + QStringLiteral("/node_modules/@openai"),
                        {QStringLiteral("codex.exe")}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext())
            return it.next();
    }
    return {};
}
void QuotaService::refresh() {
    if (!m_enabled || m_pending)
        return;
    retire();
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const QString executable =
        settings.value(QStringLiteral("codexExecutable"), discover()).toString();
    if (!QFileInfo::exists(executable)) {
        fail(tr("Codex CLI was not found. Select codex.exe in Settings"));
        return;
    }
    m_pending = true;
    m_status = tr("Refreshing quota…");
    emit changed();
    m_deadline.start();
    m_process.setWorkingDirectory(dataPath());
    m_process.start(executable, {QStringLiteral("app-server"), QStringLiteral("--stdio")});
}
void QuotaService::send(QString method, int id) {
    QJsonObject message{{QStringLiteral("method"), method}};
    if (id >= 0)
        message.insert(QStringLiteral("id"), id);
    message.insert(
        QStringLiteral("params"),
        id == 1 ? QJsonObject{{QStringLiteral("clientInfo"),
                               QJsonObject{{QStringLiteral("name"), QStringLiteral("snow_toolbox")},
                                           {QStringLiteral("version"), QStringLiteral("0.2.0")}}}}
                : QJsonObject{});
    m_process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}
void QuotaService::read() {
    m_buffer += m_process.readAllStandardOutput();
    if (m_buffer.size() > 4 * 1024 * 1024) {
        fail(tr("Invalid quota response"));
        retire();
        return;
    }
    while (m_buffer.contains('\n')) {
        auto end = m_buffer.indexOf('\n');
        auto message = QJsonDocument::fromJson(m_buffer.left(end)).object();
        m_buffer.remove(0, end + 1);
        const int id = message.value(QStringLiteral("id")).toInt(-1);
        if (id == 1) {
            if (message.contains(QStringLiteral("error"))) {
                fail(tr("Codex initialization failed"));
                retire();
                return;
            }
            send(QStringLiteral("initialized"), -1);
            send(QStringLiteral("account/rateLimits/read"), 2);
        } else if (id == 2 && m_pending) {
            auto result = message.value(QStringLiteral("result")).toObject();
            auto buckets = result.value(QStringLiteral("rateLimitsByLimitId")).toObject();
            if (buckets.isEmpty() && result.value(QStringLiteral("rateLimits")).isObject())
                buckets.insert(QStringLiteral("codex"), result.value(QStringLiteral("rateLimits")));
            if (message.contains(QStringLiteral("error")) || parse(buckets).isEmpty()) {
                fail(tr("No quota returned. Log in to Codex CLI with your ChatGPT account"));
            } else {
                m_buckets = buckets;
                m_updatedAt = QDateTime::currentDateTime();
                m_stale = false;
                QSettings cached(settingsPath(), QSettings::IniFormat);
                cached.setValue(QStringLiteral("quotaCache"),
                                QJsonDocument(m_buckets).toJson(QJsonDocument::Compact));
                cached.setValue(QStringLiteral("quotaUpdatedAt"), m_updatedAt);
                m_pending = false;
                m_deadline.stop();
                m_failures = 0;
                m_refresh.setInterval(60000);
                m_status = tr("Up to date · refresh every 60 seconds");
                emit changed();
            }
            m_process.closeWriteChannel();
            m_retire.start();
        }
    }
}
void QuotaService::fail(QString message) {
    m_pending = false;
    m_deadline.stop();
    m_stale = true;
    m_status = std::move(message);
    m_failures = std::min(m_failures + 1, 4);
    m_refresh.setInterval(std::min(900000, 60000 * (1 << m_failures)));
    emit changed();
}
bool QuotaService::stale() const {
    return m_stale || !m_enabled || !m_updatedAt.isValid() ||
           m_updatedAt.secsTo(QDateTime::currentDateTime()) > 120;
}
QList<QuotaWindow> QuotaService::windows() const {
    return parse(m_buckets);
}
QList<QuotaWindow> QuotaService::parse(const QJsonObject& buckets) {
    QList<QuotaWindow> result;
    QStringList ids = buckets.keys();
    if (ids.removeOne(QStringLiteral("codex")))
        ids.prepend(QStringLiteral("codex"));
    for (const auto& id : ids)
        for (const auto& key : {QStringLiteral("primary"), QStringLiteral("secondary")}) {
            const auto window = buckets.value(id).toObject().value(key).toObject();
            if (!window.value(QStringLiteral("usedPercent")).isDouble())
                continue;
            const int minutes = window.value(QStringLiteral("windowDurationMins")).toInt();
            QString title = minutes == 300     ? tr("5 hours")
                            : minutes == 10080 ? tr("Weekly")
                            : minutes > 0      ? tr("%1 minutes").arg(minutes)
                                               : tr("Window %1").arg(key);
            if (id != QStringLiteral("codex"))
                title = id + QStringLiteral(" · ") + title;
            const auto reset = window.value(QStringLiteral("resetsAt"));
            result.append(
                {title,
                 std::clamp(100.0 - window.value(QStringLiteral("usedPercent")).toDouble(), 0.0,
                            100.0),
                 reset.isDouble() ? QDateTime::fromSecsSinceEpoch(reset.toInteger())
                                  : QDateTime{}});
        }
    return result;
}
} // namespace toolbox
