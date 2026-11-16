// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QTimer>
#include <QFutureWatcher>
#include <QJsonObject>

namespace toolbox {
struct MemorySnapshot {
    quint64 total = 0, available = 0, privateWorking = 0, committed = 0;
    int ownedProcesses = 0;
};
class MemoryService final : public QObject {
    Q_OBJECT
  public:
    explicit MemoryService(QObject* parent = nullptr);
    ~MemoryService() override;
    void setEnabled(bool enabled);
    void refresh();
    void clean(bool deep);
    bool busy() const {
        return m_busy;
    }
    MemorySnapshot snapshot() const {
        return m_snapshot;
    }
    static MemorySnapshot collect();
    static int runDeepHelper(const QString& id);
  signals:
    void changed();
    void cleaned(QString message);

  private:
    QTimer m_timer;
    QFutureWatcher<QString> m_cleanup;
    MemorySnapshot m_snapshot;
    bool m_busy = false;
};
} // namespace toolbox
