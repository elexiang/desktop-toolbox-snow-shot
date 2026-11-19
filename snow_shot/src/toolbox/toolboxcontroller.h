// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "quotaservice.h"
#include "memoryservice.h"
#include "quotacard.h"
#include <QWidget>
#include <QSystemTrayIcon>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QCheckBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QToolButton>
#include <functional>

namespace toolbox {
class TaskbarEvents;
class LocalLibrary;
class ClipboardTracker;
class LibraryWindow;
class QuickPaste;
struct ToolboxActions {
    std::function<void()> capture, pastePin, restorePins, cancelCapture, openCaptureSettings;
    std::function<void(bool)> setPinsEnabled = [](bool) {};
    std::function<bool()> autoStartEnabled = [] { return false; };
    std::function<QString(bool)> setAutoStart = [](bool) { return QString(); };
    std::function<QString()> applyCaptureShortcut = [] { return QString(); };
    std::function<void(bool)> setShortcutEditing = [](bool) {};
};
class ToolboxController final : public QObject {
    Q_OBJECT
  public:
    explicit ToolboxController(ToolboxActions actions, QObject* parent = nullptr);
    ~ToolboxController() override;
    void showHome();
    void presentText(const QString& text);
    void showCaptureSettings();
    void captureActivity(bool active);
    void reportClipboardCopy(quint64 generation, const QString& stage, int attempts, quint32 code);
    void refreshUi();
    void reportShortcutFailure();
    void showQuickPicker(bool notes = false);
    static bool configureCapture();

  protected:
    bool eventFilter(QObject* object, QEvent* event) override;

  private:
    ToolboxActions m_actions;
    QuotaService m_quota;
    MemoryService m_memory;
    QWidget *m_home = nullptr, *m_detail = nullptr;
    QuotaCard* m_card = nullptr;
    QLabel *m_quotaLabel = nullptr, *m_detailLabel = nullptr, *m_memoryLabel = nullptr,
           *m_status = nullptr, *m_quotaStatus = nullptr, *m_quotaUpdated = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QString m_lastQuotaLog;
    QSystemTrayIcon m_tray;
    QMenu m_menu;
    QTimer m_hover, m_leave;
    QTimer m_taskbarTracking;
    TaskbarEvents* m_taskbarEvents = nullptr;
    bool m_taskbarDocked = false;
    void positionCard();
    QPointer<QWidget> m_textWindow;
    QPointer<QWidget> m_qrWindow;
    QPointer<LibraryWindow> m_libraryWindow;
    LocalLibrary* m_library = nullptr;
    ClipboardTracker* m_clipboardTracker = nullptr;
    QuickPaste* m_quickPaste = nullptr;
    void showClipboard();
    void showNotes();
    void showLibrary(bool notes);
    QCheckBox* m_contextToggle = nullptr;
    QToolButton* m_contextHelp = nullptr;
    QPushButton *m_contextRefresh = nullptr, *m_contextDefault = nullptr;
    QProcess m_contextProcess;
    QTimer m_contextTimeout;
    QTimer m_logCleanup;
    QTimer m_popupPoll;
    qint64 m_popupPollAt = 0;
    quint32 m_popupReadError = 0;
    QString m_contextRevision;
    bool m_contextWriting = false;
    QPoint m_dragOrigin, m_dragPosition;
    bool m_dragging = false, m_moved = false, m_capture = false;
    bool m_quitting = false;
    bool m_pinsEnabled = true;
    bool m_shortcutEditing = false;
    void setShortcutEditing(bool editing);
    void buildHome();
    void applyTheme();
    QString applySettings(bool updateShortcut = true);
    void showDetail();
    void capture();
    void clean(bool deep);
    void runContext(const QString& action);
    void finishContext();
    void setContextStatus(const QString& text);
    void cleanupLogs();
    void pollWindowsPopups();
    QString quotaHtml() const;
    QString memoryHtml(bool compact = false) const;
    void appendLog(const QString& message);
};
} // namespace toolbox
