// SPDX-License-Identifier: GPL-3.0-or-later
#include "offlinetranslator.h"
#include "toolboxpaths.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <optional>

namespace toolbox {
OfflineTranslator& OfflineTranslator::instance() {
    static QPointer<OfflineTranslator> service;
    if (!service)
        service = new OfflineTranslator(QCoreApplication::instance());
    return *service;
}
OfflineTranslator::OfflineTranslator(QObject* parent) : QObject(parent) {
    m_idle.setSingleShot(true);
    m_idle.setInterval(60000);
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(120000);
    connect(&m_idle, &QTimer::timeout, this, &OfflineTranslator::releaseIdle);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        m_stopping = true;
        m_process.kill();
        m_process.waitForFinished(1000);
        m_stopping = false;
        finish({}, tr("Offline translation timed out. Retry the operation"));
    });
    connect(&m_process, &QProcess::started, this, &OfflineTranslator::send);
    connect(&m_process, &QProcess::readyReadStandardError, this,
            [this] { m_process.readAllStandardError(); });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_stopping && error == QProcess::FailedToStart)
            finish({}, tr("Unable to start the offline translation engine"));
    });
    connect(&m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (!m_stopping && m_active)
            finish({}, tr("The offline translation engine stopped unexpectedly"));
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_buffer += m_process.readAllStandardOutput();
        if (m_buffer.size() > 4 * 1024 * 1024) {
            stop();
            return;
        }
        while (m_buffer.contains('\n')) {
            const auto end = m_buffer.indexOf('\n');
            const auto message = QJsonDocument::fromJson(m_buffer.left(end)).object();
            m_buffer.remove(0, end + 1);
            if (!m_active ||
                message.value(QStringLiteral("id")).toInteger() != qint64(m_active->id))
                continue;
            finish(message.value(QStringLiteral("text")).toString(),
                   message.value(QStringLiteral("error")).toString());
        }
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
            &OfflineTranslator::stop);
}
OfflineTranslator::~OfflineTranslator() {
    stop();
}
QString OfflineTranslator::direction(const QString& text, const QString& target) {
    if (target == QStringLiteral("en"))
        return QStringLiteral("zh_en");
    if (target.startsWith(QStringLiteral("zh")))
        return QStringLiteral("en_zh");
    int chinese = 0, latin = 0;
    for (QChar character : text) {
        const auto code = character.unicode();
        if (code >= 0x4e00 && code <= 0x9fff)
            ++chinese;
        else if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z'))
            ++latin;
    }
    return chinese > 0 && chinese * 2 >= latin ? QStringLiteral("zh_en") : QStringLiteral("en_zh");
}
quint64 OfflineTranslator::translate(QString text, QString target, QObject* owner,
                                     Completion done) {
    const auto id = ++m_nextId;
    m_queue.enqueue({id, std::move(text), std::move(target), owner, std::move(done)});
    connect(owner, &QObject::destroyed, this, [this, id] { cancel(id); });
    QTimer::singleShot(0, this, [this] { pump(); });
    return id;
}
void OfflineTranslator::pump() {
    if (m_stopping || m_active)
        return;
    while (!m_queue.isEmpty() && !m_queue.head().owner)
        m_queue.dequeue();
    if (m_queue.isEmpty()) {
        m_idle.start();
        return;
    }
    m_idle.stop();
    m_active = m_queue.dequeue();
    if (!enabled(QStringLiteral("translation"))) {
        finish({}, tr("Offline translation is disabled in Settings"));
        return;
    }
    if (m_active->text.size() > 20000) {
        finish({}, tr("Translate at most 20000 characters at a time"));
        return;
    }
    const QString root =
        qEnvironmentVariable("SNOW_TOOLBOX_RUNTIME",
                             QCoreApplication::applicationDirPath() + QStringLiteral("/runtime"));
    const QString python = root + QStringLiteral("/python/python.exe");
    if (!QFileInfo::exists(python)) {
        finish({}, tr("Offline models are missing. Extract the complete package"));
        return;
    }
    m_deadline.start();
    if (m_process.state() == QProcess::Running)
        send();
    else {
        m_buffer.clear();
        m_process.setWorkingDirectory(root);
        m_process.start(python,
                        {QStringLiteral("-u"), root + QStringLiteral("/translate_worker.py"),
                         root + QStringLiteral("/models")});
    }
}
void OfflineTranslator::send() {
    if (!m_active)
        return;
    m_process.write(QJsonDocument(QJsonObject{{QStringLiteral("id"), qint64(m_active->id)},
                                              {QStringLiteral("text"), m_active->text},
                                              {QStringLiteral("direction"),
                                               direction(m_active->text, m_active->target)}})
                        .toJson(QJsonDocument::Compact) +
                    '\n');
}
void OfflineTranslator::finish(QString text, QString error) {
    m_deadline.stop();
    if (m_active) {
        auto request = std::move(*m_active);
        m_active.reset();
        if (request.owner)
            request.done(std::move(text), std::move(error));
    }
    QTimer::singleShot(0, this, [this] { pump(); });
}
void OfflineTranslator::cancel(quint64 id) {
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (it->id == id)
            it = m_queue.erase(it);
        else
            ++it;
    }
    if (m_active && m_active->id == id) {
        m_active.reset();
        m_deadline.stop();
        m_stopping = true;
        m_process.kill();
        m_process.waitForFinished(1000);
        m_stopping = false;
        m_buffer.clear();
        QTimer::singleShot(0, this, [this] { pump(); });
    }
}
void OfflineTranslator::releaseIdle() {
    if (m_active || !m_queue.isEmpty())
        return;
    m_stopping = true;
    m_process.kill();
    m_process.waitForFinished(1000);
    m_stopping = false;
    m_buffer.clear();
}
void OfflineTranslator::stop() {
    m_idle.stop();
    m_deadline.stop();
    m_stopping = true;
    auto queue = std::move(m_queue);
    m_queue.clear();
    auto active = std::move(m_active);
    m_active.reset();
    m_process.kill();
    m_process.waitForFinished(1000);
    m_buffer.clear();
    const auto error = tr("Translation cancelled; the model has been released");
    if (active && active->owner)
        active->done({}, error);
    for (auto& request : queue)
        if (request.owner)
            request.done({}, error);
    m_stopping = false;
}
} // namespace toolbox
