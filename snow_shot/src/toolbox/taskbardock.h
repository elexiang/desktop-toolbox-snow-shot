// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QGuiApplication>
#include <QScreen>
#include <QRect>
#include <optional>
#include <QTimer>
#include <QHash>
#include <functional>
#include <utility>
#ifdef Q_OS_WIN
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#include <dwmapi.h>
#endif

namespace toolbox {
// All geometry exposed to the controller is in Qt logical coordinates.
struct TaskbarDock {
    QRect bar, tray;
    bool visible = false;
};

inline bool taskbarShellClass(const QString& name) {
    return name == QStringLiteral("Shell_TrayWnd") ||
           name == QStringLiteral("Shell_SecondaryTrayWnd") ||
           name == QStringLiteral("TrayNotifyWnd") ||
           name == QStringLiteral("TaskListThumbnailWnd") ||
           name == QStringLiteral("MultitaskingViewFrame") || name == QStringLiteral("WorkerW") ||
           name == QStringLiteral("Progman");
}

// Out-of-context hooks run on this object's UI thread. Coalesce bursts without polling faster.
class TaskbarEvents final : public QObject {
  public:
    explicit TaskbarEvents(QObject* parent, std::function<void()> changed) : QObject(parent) {
        m_update.setSingleShot(true);
        m_update.setInterval(0);
        connect(&m_update, &QTimer::timeout, this, std::move(changed));
    }
    ~TaskbarEvents() override {
        setEnabled(false);
    }
    void setEnabled(bool enabled) {
        if (m_enabled == enabled)
            return;
        m_enabled = enabled;
#ifdef Q_OS_WIN
        if (enabled && QGuiApplication::platformName() == QStringLiteral("windows")) {
            for (const auto range :
                 {std::pair<DWORD, DWORD>{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                  {EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE}}) {
                const auto hook =
                    SetWinEventHook(range.first, range.second, nullptr, callback, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
                if (hook) {
                    m_hooks.append(hook);
                    s_owners.insert(hook, this);
                }
            }
        } else {
            for (const auto hook : m_hooks) {
                s_owners.remove(hook);
                UnhookWinEvent(hook);
            }
            m_hooks.clear();
        }
#endif
        if (!enabled)
            m_update.stop();
    }

  private:
    bool m_enabled = false;
    QTimer m_update;
#ifdef Q_OS_WIN
    QList<HWINEVENTHOOK> m_hooks;
    inline static QHash<HWINEVENTHOOK, TaskbarEvents*> s_owners;
    static void CALLBACK callback(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object,
                                  LONG child, DWORD, DWORD) {
        auto* owner = s_owners.value(hook, nullptr);
        if (!owner || !owner->m_enabled || owner->m_update.isActive())
            return;
        if (event != EVENT_SYSTEM_FOREGROUND) {
            if (child != CHILDID_SELF || (object != OBJID_WINDOW && object != OBJID_CLIENT) ||
                (event != EVENT_OBJECT_REORDER && event != EVENT_OBJECT_SHOW &&
                 event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_LOCATIONCHANGE))
                return;
            if (window && window != GetDesktopWindow()) {
                wchar_t name[128]{};
                GetClassNameW(GetAncestor(window, GA_ROOT), name, 128);
                if (!taskbarShellClass(QString::fromWCharArray(name)))
                    return;
            } else if (event != EVENT_OBJECT_REORDER) {
                return;
            }
        }
        owner->m_update.start();
    }
#endif
};

#ifdef Q_OS_WIN
inline bool taskbarCoversCard(HWND card) {
    const auto bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!bar)
        return false;
    for (auto previous = GetWindow(card, GW_HWNDPREV); previous;
         previous = GetWindow(previous, GW_HWNDPREV))
        if (previous == bar)
            return true;
    return false;
}
#endif

inline QRect taskbarLogicalRect(QRect nativeRect, QPoint nativeOrigin, QPoint logicalOrigin,
                                qreal scale) {
    return {logicalOrigin + QPoint(qRound((nativeRect.x() - nativeOrigin.x()) / scale),
                                   qRound((nativeRect.y() - nativeOrigin.y()) / scale)),
            QSize(qRound(nativeRect.width() / scale), qRound(nativeRect.height() / scale))};
}

inline std::optional<QPoint> taskbarCardPosition(const TaskbarDock& dock, QSize size) {
    if (!dock.visible || dock.bar.width() <= dock.bar.height() ||
        size.height() > dock.bar.height() || dock.tray.left() - 6 - size.width() < dock.bar.left())
        return std::nullopt;
    return QPoint(dock.tray.left() - 6 - size.width(),
                  dock.bar.top() + (dock.bar.height() - size.height()) / 2);
}

inline TaskbarDock currentTaskbarDock() {
    TaskbarDock result;
#ifdef Q_OS_WIN
    // Query afresh so Explorer restarts, tray resizing and monitor changes recover automatically.
    const HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT barRect{}, trayRect{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const auto monitorHandle = MonitorFromWindow(bar, MONITOR_DEFAULTTOPRIMARY);
    if (!bar || !tray || !GetWindowRect(bar, &barRect) || !GetWindowRect(tray, &trayRect) ||
        !GetMonitorInfoW(monitorHandle, &monitor))
        return result;
    QScreen* screen = nullptr;
    for (auto* candidate : QGuiApplication::screens()) {
        const auto* native = candidate->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitorHandle) {
            screen = candidate;
            break;
        }
    }
    if (!screen)
        return result;
    const qreal scale = screen->devicePixelRatio();
    const auto logicalRect = [&](const RECT& rect) {
        return taskbarLogicalRect(
            QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top),
            QPoint(monitor.rcMonitor.left, monitor.rcMonitor.top), screen->geometry().topLeft(),
            scale);
    };
    result.bar = logicalRect(barRect);
    result.tray = logicalRect(trayRect);
    result.visible = IsWindowVisible(bar) && IsWindowVisible(tray) &&
                     screen->geometry().contains(result.bar) && result.bar.contains(result.tray);
    const HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    RECT foregroundRect{};
    wchar_t windowClass[128]{};
    GetClassNameW(foreground, windowClass, 128);
    if (foreground && foreground != bar && process != GetCurrentProcessId() &&
        !taskbarShellClass(QString::fromWCharArray(windowClass)) &&
        GetWindowRect(foreground, &foregroundRect) &&
        foregroundRect.left <= monitor.rcMonitor.left &&
        foregroundRect.top <= monitor.rcMonitor.top &&
        foregroundRect.right >= monitor.rcMonitor.right &&
        foregroundRect.bottom >= monitor.rcMonitor.bottom)
        result.visible = false;
#endif
    return result;
}
} // namespace toolbox
