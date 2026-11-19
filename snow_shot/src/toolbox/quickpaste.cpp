// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickpaste.h"
#include "libraryclipboard.h"
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QClipboard>
#include <QHash>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#endif

namespace toolbox {
bool parseQuickShortcut(const QKeySequence& sequence, NativeShortcut* result) {
    if (!result || sequence.count() != 1)
        return false;
    const auto combination = sequence[0];
    const auto modifiers = combination.keyboardModifiers();
    if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) ||
        (modifiers &
         ~(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier)))
        return false;
    const int key = combination.key();
    unsigned nativeKey = 0;
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
        nativeKey = static_cast<unsigned>(key);
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        nativeKey = 0x70U + static_cast<unsigned>(key - Qt::Key_F1);
    else {
        switch (key) {
        case Qt::Key_Space:
            nativeKey = 0x20;
            break;
        case Qt::Key_Insert:
            nativeKey = 0x2d;
            break;
        case Qt::Key_Home:
            nativeKey = 0x24;
            break;
        case Qt::Key_End:
            nativeKey = 0x23;
            break;
        case Qt::Key_PageUp:
            nativeKey = 0x21;
            break;
        case Qt::Key_PageDown:
            nativeKey = 0x22;
            break;
        default:
            return false;
        }
    }
    *result = {(modifiers.testFlag(Qt::AltModifier) ? 1U : 0U) |
                   (modifiers.testFlag(Qt::ControlModifier) ? 2U : 0U) |
                   (modifiers.testFlag(Qt::ShiftModifier) ? 4U : 0U) |
                   (modifiers.testFlag(Qt::MetaModifier) ? 8U : 0U),
               nativeKey};
    return true;
}
namespace {
#ifdef Q_OS_WIN
struct NativeEditorFocus final : PasteFocus {
    Microsoft::WRL::ComPtr<IUIAutomationElement> element;
};
#endif
class NativePlatform final : public QuickPastePlatform, public QAbstractNativeEventFilter {
  public:
    NativePlatform() {
#ifdef Q_OS_WIN
        m_native = QGuiApplication::platformName() == QStringLiteral("windows");
        if (m_native) {
            const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            m_comInitialized = SUCCEEDED(initialized);
            CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&m_automation));
            if (m_automation) {
                Microsoft::WRL::ComPtr<IUIAutomation2> automation2;
                if (SUCCEEDED(m_automation.As(&automation2))) {
                    automation2->put_ConnectionTimeout(300);
                    automation2->put_TransactionTimeout(300);
                }
            }
            qApp->installNativeEventFilter(this);
            m_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                     &foregroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
            if (m_hook)
                instances().insert(m_hook, this);
            m_focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr,
                                          &foregroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
            if (m_focusHook)
                instances().insert(m_focusHook, this);
            observeTarget();
        }
#endif
    }
    ~NativePlatform() override {
        unregisterKeys();
#ifdef Q_OS_WIN
        if (m_native && qApp)
            qApp->removeNativeEventFilter(this);
        if (m_hook) {
            instances().remove(m_hook);
            UnhookWinEvent(m_hook);
        }
        if (m_focusHook) {
            instances().remove(m_focusHook);
            UnhookWinEvent(m_focusHook);
        }
        m_last.editor.reset();
        m_automation.Reset();
        if (m_comInitialized)
            CoUninitialize();
#endif
    }
    PasteTarget rememberTarget() override {
#ifdef Q_OS_WIN
        if (m_native) {
            observeTarget();
            captureEditor();
            // Keep the last external input target while a toolbox window owns focus.
            // Inactive threads can temporarily report a null/parent focus window.
        }
#endif
        return m_last;
    }
    bool valid(const PasteTarget& target) const override {
#ifdef Q_OS_WIN
        const auto window = reinterpret_cast<HWND>(target.window);
        DWORD process = 0;
        const auto focus = reinterpret_cast<HWND>(target.focus);
        return m_native && window && IsWindow(window) && IsWindowVisible(window) &&
               (target.editor ||
                (focus && IsWindow(focus) && (focus == window || IsChild(window, focus)))) &&
               GetWindowThreadProcessId(window, &process) && process == target.process &&
               process != GetCurrentProcessId();
#else
        Q_UNUSED(target)
        return false;
#endif
    }
    bool activate(const PasteTarget& target) override {
#ifdef Q_OS_WIN
        if (!valid(target))
            return false;
        const auto window = reinterpret_cast<HWND>(target.window);
        if (IsIconic(window))
            ShowWindow(window, SW_RESTORE);
        if (GetForegroundWindow() != window && !SetForegroundWindow(window))
            return false;
        // An Alt-only hotkey can leave a menu loop active in the original app.
        cancelMenu(window);
        return true;
#else
        Q_UNUSED(target)
        return false;
#endif
    }
    bool focused(const PasteTarget& target) const override {
#ifdef Q_OS_WIN
        if (!valid(target) || reinterpret_cast<quintptr>(GetForegroundWindow()) != target.window)
            return false;
        const auto current = capture(reinterpret_cast<HWND>(target.window));
        return current.focus && (target.editor ? (current.focus == target.window ||
                                                  IsChild(reinterpret_cast<HWND>(target.window),
                                                          reinterpret_cast<HWND>(current.focus)))
                                               : current.focus == target.focus);
#else
        Q_UNUSED(target)
        return false;
#endif
    }
    void restoreInputFocus(const PasteTarget& target) override {
#ifdef Q_OS_WIN
        if (reinterpret_cast<quintptr>(GetForegroundWindow()) != target.window)
            return;
        const auto editor = std::dynamic_pointer_cast<NativeEditorFocus>(target.editor);
        if (editor) {
            editor->element->SetFocus();
            return;
        }
        if (!valid(target))
            return;
        const auto window = reinterpret_cast<HWND>(target.window);
        const auto targetThread = GetWindowThreadProcessId(window, nullptr);
        const auto ownThread = GetCurrentThreadId();
        const bool attached =
            targetThread != ownThread && AttachThreadInput(ownThread, targetThread, TRUE);
        if (!attached && targetThread != ownThread)
            return;
        SetFocus(reinterpret_cast<HWND>(target.focus));
        if (attached)
            AttachThreadInput(ownThread, targetThread, FALSE);
#else
        Q_UNUSED(target)
#endif
    }
    bool inputReady(const PasteTarget& target) const override {
        if (!focused(target))
            return false;
#ifdef Q_OS_WIN
        const auto editor = std::dynamic_pointer_cast<NativeEditorFocus>(target.editor);
        if (editor && m_automation) {
            Microsoft::WRL::ComPtr<IUIAutomationElement> current;
            BOOL same = FALSE;
            return SUCCEEDED(m_automation->GetFocusedElement(&current)) && current &&
                   SUCCEEDED(m_automation->CompareElements(current.Get(), editor->element.Get(),
                                                           &same)) &&
                   same;
        }
#endif
        return true;
    }
    bool keysReleased() const override {
#ifdef Q_OS_WIN
        for (const int key :
             {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN, VK_RETURN, VK_LBUTTON, VK_RBUTTON})
            if (GetAsyncKeyState(key) & 0x8000)
                return false;
#endif
        return true;
    }
    bool sendPaste() override {
#ifdef Q_OS_WIN
        INPUT keys[4]{};
        for (auto& key : keys)
            key.type = INPUT_KEYBOARD;
        keys[0].ki.wVk = VK_CONTROL;
        keys[1].ki.wVk = 'V';
        keys[2].ki.wVk = 'V';
        keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
        keys[3].ki.wVk = VK_CONTROL;
        keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
        const auto sent = SendInput(4, keys, sizeof(INPUT));
        if (sent != 4 && sent > 0) {
            // Release only keys that this failed batch may have pressed.
            INPUT release[2]{};
            release[0].type = release[1].type = INPUT_KEYBOARD;
            release[0].ki.wVk = 'V';
            release[1].ki.wVk = VK_CONTROL;
            release[0].ki.dwFlags = release[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, release, sizeof(INPUT));
        }
        return sent == 4;
#else
        return false;
#endif
    }
    bool registerKey(int index, const NativeShortcut& shortcut) override {
#ifdef Q_OS_WIN
        return m_native && RegisterHotKey(nullptr, 0x5b01 + index,
                                          shortcut.modifiers | MOD_NOREPEAT, shortcut.key);
#else
        Q_UNUSED(index)
        Q_UNUSED(shortcut)
        return false;
#endif
    }
    void unregisterKeys() override {
#ifdef Q_OS_WIN
        if (m_native) {
            UnregisterHotKey(nullptr, 0x5b01);
            UnregisterHotKey(nullptr, 0x5b02);
        }
#endif
    }
    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
#ifdef Q_OS_WIN
        const auto* native = static_cast<MSG*>(message);
        if (native && native->message == WM_HOTKEY &&
            (native->wParam == 0x5b01 || native->wParam == 0x5b02)) {
            if ((LOWORD(native->lParam) & (MOD_ALT | MOD_CONTROL | MOD_WIN)) == MOD_ALT)
                cancelMenu(GetForegroundWindow());
            if (activated)
                activated(native->wParam == 0x5b02);
            return true;
        }
#else
        Q_UNUSED(message)
#endif
        return false;
    }

  private:
    PasteTarget m_last;
#ifdef Q_OS_WIN
    bool m_native = false;
    bool m_comInitialized = false, m_capturing = false;
    Microsoft::WRL::ComPtr<IUIAutomation> m_automation;
    HWINEVENTHOOK m_hook = nullptr;
    HWINEVENTHOOK m_focusHook = nullptr;
    static void cancelMenu(HWND window) {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (window && GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info) &&
            (info.flags & (GUI_INMENUMODE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE)))
            PostMessageW(window, WM_CANCELMODE, 0, 0);
    }
    void observeTarget() {
        const auto window = GetForegroundWindow();
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (window && process != GetCurrentProcessId()) {
            auto current = capture(window);
            if (current.window == m_last.window && current.focus == m_last.focus)
                current.editor = m_last.editor;
            m_last = std::move(current);
        }
    }
    void captureEditor() {
        if (!m_automation || m_capturing || !m_last.window ||
            reinterpret_cast<quintptr>(GetForegroundWindow()) != m_last.window)
            return;
        QScopedValueRollback<bool> guard(m_capturing, true);
        const auto target = m_last;
        auto editor = std::make_shared<NativeEditorFocus>();
        int process = 0;
        CONTROLTYPEID type = 0;
        BOOL keyboard = FALSE;
        if (FAILED(m_automation->GetFocusedElement(&editor->element)) || !editor->element ||
            FAILED(editor->element->get_CurrentProcessId(&process)) ||
            static_cast<quint32>(process) != target.process ||
            FAILED(editor->element->get_CurrentControlType(&type)) ||
            type != UIA_EditControlTypeId ||
            FAILED(editor->element->get_CurrentIsKeyboardFocusable(&keyboard)) || !keyboard)
            return;
        if (m_last.window == target.window &&
            reinterpret_cast<quintptr>(GetForegroundWindow()) == target.window)
            m_last.editor = std::move(editor);
    }
    static QHash<HWINEVENTHOOK, NativePlatform*>& instances() {
        static QHash<HWINEVENTHOOK, NativePlatform*> map;
        return map;
    }
    static PasteTarget capture(HWND window) {
        DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(window, &process);
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        GetGUIThreadInfo(thread, &info);
        return {reinterpret_cast<quintptr>(window),
                reinterpret_cast<quintptr>(info.hwndFocus),
                process,
                {}};
    }
    static void CALLBACK foregroundChanged(HWINEVENTHOOK hook, DWORD, HWND, LONG, LONG, DWORD,
                                           DWORD) {
        if (auto* platform = instances().value(hook))
            platform->observeTarget();
    }
#endif
};
} // namespace
std::unique_ptr<QuickPastePlatform> makeQuickPastePlatform() {
    return std::make_unique<NativePlatform>();
}
QuickPaste::QuickPaste(QObject* parent, std::unique_ptr<QuickPastePlatform> platform)
    : QObject(parent), m_platform(std::move(platform)) {
    m_platform->activated = [this](bool notes) {
        if (!m_busy && !m_suspended) {
            rememberTarget();
            emit requested(notes);
        }
    };
    m_wait.setInterval(20);
    connect(&m_wait, &QTimer::timeout, this, &QuickPaste::advance);
}
QuickPaste::~QuickPaste() {
    m_platform->activated = {};
    m_target.editor.reset();
}
bool QuickPaste::registerKeys(const QKeySequence& clipboard, const QKeySequence& notes) {
    const QKeySequence keys[]{clipboard, notes};
    for (int i = 0; i < 2; ++i) {
        if (keys[i].isEmpty())
            continue;
        NativeShortcut shortcut;
        if (!parseQuickShortcut(keys[i], &shortcut) || !m_platform->registerKey(i, shortcut)) {
            m_platform->unregisterKeys();
            return false;
        }
    }
    return true;
}
QString QuickPaste::configure(const QKeySequence& clipboard, const QKeySequence& notes) {
    NativeShortcut shortcut;
    if ((!clipboard.isEmpty() && !parseQuickShortcut(clipboard, &shortcut)) ||
        (!notes.isEmpty() && !parseQuickShortcut(notes, &shortcut)))
        return QStringLiteral(
            "快速呼出快捷键需要 Ctrl、Alt 或 Win，并搭配字母、数字、功能键或导航键");
    if (!clipboard.isEmpty() && clipboard == notes)
        return QStringLiteral("剪贴板和笔记不能使用相同的快速呼出快捷键");
    m_platform->unregisterKeys();
    if (!m_suspended && !registerKeys(clipboard, notes)) {
        registerKeys(m_clipboardKey, m_notesKey);
        return QStringLiteral("快速呼出快捷键注册失败，可能已被占用；已保留原快捷键");
    }
    m_clipboardKey = clipboard;
    m_notesKey = notes;
    return {};
}
QString QuickPaste::setSuspended(bool suspended) {
    if (suspended == m_suspended)
        return {};
    m_suspended = suspended;
    m_platform->unregisterKeys();
    if (!suspended && !registerKeys(m_clipboardKey, m_notesKey))
        return QStringLiteral("快速呼出快捷键恢复失败，请检查是否被其他程序占用");
    return {};
}
void QuickPaste::rememberTarget() {
    if (!m_busy)
        m_target = m_platform->rememberTarget();
}
void QuickPaste::cancel(QWidget* panel, bool restoreFocus) {
    m_wait.stop();
    m_busy = false;
    m_source.clear();
    if (panel)
        panel->hide();
    if (restoreFocus && m_platform->valid(m_target))
        m_platform->activate(m_target);
}
void QuickPaste::paste(const QString& text, const QImage& image, QWidget* panel,
                       const QString& html) {
    if (m_busy)
        return;
    if (text.isEmpty() && image.isNull()) {
        emit finished(false, QStringLiteral("没有可粘贴的内容"));
        return;
    }
    setLibraryClipboard(text, image, html);
    if (!m_platform->valid(m_target)) {
        emit finished(false, QStringLiteral("未找到原输入窗口，内容已复制，可切回输入框手动粘贴"));
        return;
    }
    m_source = panel;
    m_busy = true;
    m_activatedTarget = false;
    m_focusRestoreAttempted = false;
    m_elapsed.start();
    m_wait.start();
    advance();
}
void QuickPaste::advance() {
    if (m_elapsed.elapsed() > 1800) {
        finish(false, QStringLiteral("等待按键释放或切回输入框超时，内容已复制，请手动粘贴"));
        return;
    }
    if (!m_platform->valid(m_target)) {
        finish(false, QStringLiteral("原输入窗口已关闭，内容已复制"));
        return;
    }
    if (!m_platform->keysReleased())
        return;
    if (!m_activatedTarget) {
        if (m_source) {
            // On Windows, hide() may synchronously deliver WindowDeactivate while
            // the picker still appears visible. This is our focus handoff, not a dismissal.
            const QSignalBlocker blockDismissal(m_source.data());
            m_source->hide();
        }
        if (!m_platform->activate(m_target)) {
            finish(false, QStringLiteral("无法切回原输入窗口，内容已复制，请手动粘贴"));
            return;
        }
        m_activatedTarget = true;
        m_focusElapsed.start();
        return; // Let the target process receive its activation/focus messages.
    }
    if (!m_platform->inputReady(m_target)) {
        const auto focusWait = m_focusElapsed.elapsed();
        if (focusWait >= 700) {
            finish(false, QStringLiteral("原输入焦点尚未恢复，内容已复制，请手动粘贴"));
        } else if (!m_focusRestoreAttempted && focusWait >= 60) {
            // Let Windows return focus naturally before forcing it across threads.
            m_focusRestoreAttempted = true;
            m_platform->restoreInputFocus(m_target);
        }
        return;
    }
    const bool sent = m_platform->sendPaste();
    finish(sent,
           sent ? QStringLiteral("已向原输入窗口发送粘贴")
                : QStringLiteral("粘贴发送失败，目标可能需要更高权限；内容已复制，请手动粘贴"));
}
void QuickPaste::finish(bool sent, const QString& message) {
    m_wait.stop();
    m_busy = false;
    if (!sent && m_source) {
        m_source->show();
        m_source->raise();
        m_source->activateWindow();
    }
    m_source.clear();
    emit finished(sent, message);
}
} // namespace toolbox
