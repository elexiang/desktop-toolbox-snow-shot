// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QPointer>
#include <QQueue>
#include <functional>
#include <optional>

namespace toolbox {
class OfflineTranslator final : public QObject {
    Q_OBJECT
  public:
    using Completion = std::function<void(QString, QString)>;
    static OfflineTranslator& instance();
    quint64 translate(QString text, QString target, QObject* owner, Completion completion);
    void cancel(quint64 id);
    void releaseIdle();
    void stop();
    qint64 processId() const {
        return m_process.processId();
    }
    static QString direction(const QString& text, const QString& target);

  private:
    explicit OfflineTranslator(QObject* parent);
    ~OfflineTranslator() override;
    struct Request {
        quint64 id;
        QString text, target;
        QPointer<QObject> owner;
        Completion done;
    };
    QQueue<Request> m_queue;
    std::optional<Request> m_active;
    quint64 m_nextId = 0;
    QProcess m_process;
    QTimer m_idle, m_deadline;
    QByteArray m_buffer;
    bool m_stopping = false;
    void pump();
    void send();
    void finish(QString text, QString error);
};
} // namespace toolbox
