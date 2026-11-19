// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QImage>
#include <QKeySequence>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QWidget>
#include <functional>
#include <memory>

namespace toolbox {
struct PasteFocus {
    virtual ~PasteFocus() = default;
};
struct PasteTarget {
    quintptr window = 0, focus = 0;
    quint32 process = 0;
    std::shared_ptr<PasteFocus> editor;
};
struct NativeShortcut {
    unsigned modifiers = 0, key = 0;
};
bool parseQuickShortcut(const QKeySequence& sequence, NativeShortcut* result);
class QuickPastePlatform {
  public:
    virtual ~QuickPastePlatform() = default;
    std::function<void(bool)> activated;
    virtual PasteTarget rememberTarget() = 0;
    virtual bool valid(const PasteTarget& target) const = 0;
    virtual bool activate(const PasteTarget& target) = 0;
    virtual bool focused(const PasteTarget& target) const = 0;
    virtual void restoreInputFocus(const PasteTarget&) {}
    virtual bool inputReady(const PasteTarget& target) const {
        return focused(target);
    }
    virtual bool keysReleased() const = 0;
    virtual bool sendPaste() = 0;
    virtual bool registerKey(int index, const NativeShortcut& shortcut) = 0;
    virtual void unregisterKeys() = 0;
};
std::unique_ptr<QuickPastePlatform> makeQuickPastePlatform();
class QuickPaste final : public QObject {
    Q_OBJECT
  public:
    explicit QuickPaste(QObject* parent = nullptr,
                        std::unique_ptr<QuickPastePlatform> platform = makeQuickPastePlatform());
    ~QuickPaste() override;
    QString configure(const QKeySequence& clipboard, const QKeySequence& notes);
    QString setSuspended(bool suspended);
    void rememberTarget();
    void cancel(QWidget* panel, bool restoreFocus = true);
    void paste(const QString& text, const QImage& image, QWidget* panel, const QString& html = {});
    bool busy() const {
        return m_busy;
    }
  signals:
    void requested(bool notes);
    void finished(bool sent, const QString& message);

  private:
    std::unique_ptr<QuickPastePlatform> m_platform;
    PasteTarget m_target;
    QKeySequence m_clipboardKey, m_notesKey;
    QPointer<QWidget> m_source;
    QTimer m_wait;
    QElapsedTimer m_elapsed;
    QElapsedTimer m_focusElapsed;
    bool m_busy = false, m_suspended = false, m_activatedTarget = false;
    bool m_focusRestoreAttempted = false;
    bool registerKeys(const QKeySequence& clipboard, const QKeySequence& notes);
    void advance();
    void finish(bool sent, const QString& message);
};
} // namespace toolbox
