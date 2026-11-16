// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QDateTime>
#include <QJsonObject>

namespace toolbox {
struct QuotaWindow {
    QString title;
    double remaining = -1;
    QDateTime resets;
};
class QuotaService final : public QObject {
    Q_OBJECT
  public:
    explicit QuotaService(QObject* parent = nullptr);
    ~QuotaService() override;
    void setEnabled(bool enabled);
    void refresh();
    void shutdown();
    QString status() const {
        return m_status;
    }
    QDateTime updatedAt() const {
        return m_updatedAt;
    }
    bool stale() const;
    QList<QuotaWindow> windows() const;
    QJsonObject buckets() const {
        return m_buckets;
    }
    static QString discover();
    static QList<QuotaWindow> parse(const QJsonObject& buckets);
  signals:
    void changed();

  private:
    QProcess m_process;
    QTimer m_refresh, m_deadline, m_retire;
    QByteArray m_buffer;
    QJsonObject m_buckets;
    QDateTime m_updatedAt;
    QString m_status;
    bool m_enabled = false, m_pending = false, m_stale = true, m_closing = false;
    int m_failures = 0;
    void send(QString method, int id);
    void read();
    void fail(QString message);
    void retire();
};
} // namespace toolbox
