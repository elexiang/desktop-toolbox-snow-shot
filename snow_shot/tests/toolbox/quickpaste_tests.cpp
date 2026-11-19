// SPDX-License-Identifier: GPL-3.0-or-later
#include "quickpaste.h"
#include "librarywindows.h"
#include <QtWidgets>
#include <QTemporaryDir>
#include <QProcess>
#include <QSaveFile>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
bool waitFor(const std::function<bool()>& predicate, int timeout = 2500) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}
class FakePlatform final : public toolbox::QuickPastePlatform {
  public:
    toolbox::PasteTarget target{1, 2, 3, {}};
    bool alive = true, activateOk = true, focusOk = true, released = true, sendOk = true;
    bool ready = true;
    int sends = 0, activations = 0;
    unsigned blocked = 0;
    QList<unsigned> keys;
    toolbox::PasteTarget rememberTarget() override {
        return target;
    }
    bool valid(const toolbox::PasteTarget& value) const override {
        return alive && value.window != 0;
    }
    bool activate(const toolbox::PasteTarget&) override {
        ++activations;
        return activateOk;
    }
    bool focused(const toolbox::PasteTarget&) const override {
        return focusOk;
    }
    bool inputReady(const toolbox::PasteTarget&) const override {
        return focusOk && ready;
    }
    bool keysReleased() const override {
        return released;
    }
    bool sendPaste() override {
        ++sends;
        return sendOk;
    }
    bool registerKey(int, const toolbox::NativeShortcut& shortcut) override {
        if (shortcut.key == blocked)
            return false;
        keys.append(shortcut.key);
        return true;
    }
    void unregisterKeys() override {
        keys.clear();
    }
};
void press(QWidget* target, Qt::Key key) {
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(target, &down);
}
void mouseEvent(QListWidget* list, QEvent::Type type, Qt::MouseButton button) {
    const auto point = list->visualItemRect(list->currentItem()).center();
    auto* viewport = list->viewport();
    QMouseEvent event(type, point, viewport->mapToGlobal(point), button,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : button, Qt::NoModifier);
    QApplication::sendEvent(viewport, &event);
}
void clickResult(QListWidget* list, Qt::MouseButton button = Qt::LeftButton) {
    mouseEvent(list, QEvent::MouseButtonPress, button);
    mouseEvent(list, QEvent::MouseButtonRelease, button);
}
#ifdef Q_OS_WIN
LRESULT CALLBACK editorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SETFOCUS) {
        if (const auto edit = GetWindow(window, GW_CHILD))
            SetFocus(edit);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
int targetEditor(QApplication& app) {
    const auto output = app.arguments().at(2);
    const auto title = app.arguments().at(3).toStdWString();
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = editorWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"SnowToolboxPasteFixture";
    RegisterClassW(&windowClass);
    HWND root =
        CreateWindowExW(0, windowClass.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, 100, 100,
                        520, 220, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!root)
        return 1;
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"前后",
                                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN, 12, 12, 470,
                                130, root, nullptr, GetModuleHandleW(nullptr), nullptr);
    HMENU menu = CreateMenu();
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, 101, L"测试菜单");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图(&V)");
    SetMenu(root, menu);
    ShowWindow(root, SW_SHOWNORMAL);
    ShowWindow(root, SW_SHOW);
    SetForegroundWindow(root);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 1, 1);
    // Exercise a standard Win32 control with a Win32 message pump.
    QElapsedTimer elapsed;
    elapsed.start();
    while (IsWindow(root) && elapsed.elapsed() < 15000) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!IsWindow(root))
            break;
        wchar_t text[4096]{};
        GetWindowTextW(edit, text, 4096);
        QSaveFile file(output);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QString::fromWCharArray(text).toUtf8());
            file.commit();
        }
        QThread::msleep(10);
    }
    if (IsWindow(root))
        DestroyWindow(root);
    DestroyMenu(menu);
    return 0;
}
int nativeTest(QApplication& app) {
    const auto require = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    struct DesktopGuard {
        HWND previous = GetForegroundWindow();
        POINT cursor{};
        QMimeData* saved = new QMimeData;
        DesktopGuard() {
            GetCursorPos(&cursor);
            const auto* current = QApplication::clipboard()->mimeData();
            if (current) {
                for (const auto& format : current->formats())
                    saved->setData(format, current->data(format));
                if (current->hasImage())
                    saved->setImageData(current->imageData());
            }
        }
        ~DesktopGuard() {
            SetCursorPos(cursor.x, cursor.y);
            QApplication::clipboard()->setMimeData(saved);
            if (IsWindow(previous))
                SetForegroundWindow(previous);
        }
    } desktop;
    QTemporaryDir directory;
    const auto output = directory.filePath(QStringLiteral("target.txt"));
    const auto title =
        QStringLiteral("Snow Toolbox paste fixture %1").arg(QCoreApplication::applicationPid());
    QProcess fixture;
    fixture.start(QCoreApplication::applicationFilePath(),
                  {QStringLiteral("--target-editor"), output, title});
    require(fixture.waitForStarted(), "Native editor fixture must start");
    HWND target = nullptr;
    require(waitFor([&] {
                target = FindWindowW(L"SnowToolboxPasteFixture", title.toStdWString().c_str());
                return target != nullptr;
            }),
            "Native editor fixture must create a window");
    SetForegroundWindow(target);
    require(waitFor([&] { return GetForegroundWindow() == target; }),
            "Native fixture must acquire foreground");
    require(waitFor([&] {
                const auto edit = FindWindowExW(target, nullptr, L"EDIT", nullptr);
                GUITHREADINFO info{};
                info.cbSize = sizeof(info);
                return QFile::exists(output) && edit &&
                       GetGUIThreadInfo(GetWindowThreadProcessId(target, nullptr), &info) &&
                       info.hwndFocus == edit;
            }),
            "Native fixture must finish initialization with the input control focused");
    toolbox::QuickPaste service;
    const auto payload = QStringLiteral("中文 😀\nSQL");
    toolbox::LocalLibrary library(directory.filePath(QStringLiteral("library")));
    require(library.isOpen() && library.recordText(payload), "Picker must have a saved entry");
    QWidget home;
    toolbox::ClipboardTracker tracker(&library);
    toolbox::LibraryWindow panel(&library, &tracker, &home);
    int choices = 0;
    int dismissalsDuringPaste = 0;
    QObject::connect(&panel, &toolbox::LibraryWindow::pasteRequested, &app,
                     [&](const QString& text, const QImage& image) {
                         ++choices;
                         service.paste(text, image, &panel);
                     });
    QObject::connect(&panel, &toolbox::LibraryWindow::canceled, &app, [&] {
        if (service.busy())
            ++dismissalsDuringPaste;
        service.cancel(nullptr, false);
    });
    const bool altOnly = app.arguments().contains(QStringLiteral("--alt-v"));
    require(service
                .configure(QKeySequence(altOnly ? QStringLiteral("Alt+V")
                                                : QStringLiteral("Ctrl+Alt+F11")),
                           QKeySequence(QStringLiteral("Ctrl+Alt+F12")))
                .isEmpty(),
            "Native hotkeys must register");
    int opens = 0;
    QObject::connect(&service, &toolbox::QuickPaste::requested, &app, [&](bool notes) {
        require(!notes, "Clipboard hotkey must choose clipboard mode");
        service.rememberTarget();
        panel.selectPage(notes);
        panel.show();
        panel.raise();
        panel.activateWindow();
        ++opens;
    });
    INPUT hotkey[6]{};
    const QList<WORD> codes =
        altOnly ? QList<WORD>{VK_MENU, 'V', 'V', VK_MENU}
                : QList<WORD>{VK_CONTROL, VK_MENU, VK_F11, VK_F11, VK_MENU, VK_CONTROL};
    const int count = static_cast<int>(codes.size());
    for (int i = 0; i < count; ++i) {
        hotkey[i].type = INPUT_KEYBOARD;
        hotkey[i].ki.wVk = codes[i];
        if (i >= count / 2)
            hotkey[i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    require(SendInput(static_cast<UINT>(count), hotkey, sizeof(INPUT)) ==
                    static_cast<UINT>(count) &&
                waitFor([&] { return opens == 1; }),
            "Native RegisterHotKey must deliver the shortcut");
    require(waitFor([&] { return GetForegroundWindow() == reinterpret_cast<HWND>(panel.winId()); }),
            "Picker fixture must acquire focus");
    bool finished = false, sent = false;
    QObject::connect(&service, &toolbox::QuickPaste::finished, &app,
                     [&](bool ok, const QString& message) {
                         std::cerr << message.toStdString() << '\n';
                         finished = true;
                         sent = ok;
                     });
    INPUT enter[2]{};
    for (auto& event : enter) {
        event.type = INPUT_KEYBOARD;
        event.ki.wVk = VK_RETURN;
    }
    enter[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, enter, sizeof(INPUT)) == 2, "Enter must reach the real picker");
    waitFor([] { return false; }, 60);
    require(choices == 0 && panel.isVisible(), "Enter must leave the real picker open");
    QElapsedTimer clickElapsed;
    {
        auto* results = panel.findChild<QListWidget*>(QStringLiteral("ClipboardList"));
        require(results && results->count(), "Library must have a clipboard result");
        const auto point = results->viewport()->mapToGlobal(
            results->visualItemRect(results->item(0)).center());
        const auto ratio = panel.devicePixelRatioF();
        const auto local = panel.mapFromGlobal(point);
        POINT origin{};
        require(ClientToScreen(reinterpret_cast<HWND>(panel.winId()), &origin),
                "Picker needs a native client origin");
        require(SetCursorPos(origin.x + qRound(local.x() * ratio),
                             origin.y + qRound(local.y() * ratio)),
                "Mouse must reach the selected result");
        INPUT click[2]{};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        clickElapsed.start();
        require(SendInput(2, click, sizeof(INPUT)) == 2, "One click must reach the picker");
    }
    const bool delivered = waitFor([&] { return finished; }) && sent;
    std::cerr << "Click to paste dispatch: " << clickElapsed.elapsed() << " ms\n";
    require(choices == 1, "Real picker must emit one paste request");
    require(dismissalsDuringPaste == 0, "Hiding the picker must not cancel an in-flight paste");
    require(delivered, "Native paste must send successfully");
    QString actual;
    const bool inserted = waitFor([&] {
        QFile file(output);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        auto text = QString::fromUtf8(file.readAll());
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        actual = text;
        return text == QStringLiteral("前") + payload + QStringLiteral("后");
    });
    if (!inserted)
        std::cerr << "Fixture text: " << actual.toUtf8().toStdString() << '\n'
                  << fixture.readAllStandardError().toStdString();
    require(
        inserted,
        "Native paste must insert Unicode and newlines at the original caret in another process");
    PostMessageW(target, WM_CLOSE, 0, 0);
    require(fixture.waitForFinished(3000), "Native fixture must exit cleanly");
    if (app.arguments().size() > 2) {
        QSaveFile report(app.arguments().last());
        require(report.open(QIODevice::WriteOnly), "Native result report must open");
        const auto bytes = QStringLiteral("PASS: global hotkey, foreground/focus restoration and "
                                          "cross-process caret insertion\n%1\n")
                               .arg(actual)
                               .toUtf8();
        require(report.write(bytes) == bytes.size() && report.commit(),
                "Native result report must save");
    }
    std::cerr << "Native hotkey, foreground restoration and cross-process caret insertion passed\n";
    return 0;
}
#endif
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--target-editor")))
        return targetEditor(app);
    if (app.arguments().contains(QStringLiteral("--native"))) {
        try {
            return nativeTest(app);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
#endif
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    toolbox::NativeShortcut parsed;
    require(toolbox::parseQuickShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+V")), &parsed) &&
                parsed.modifiers == 3 && parsed.key == 'V',
            "Shortcut parsing must preserve modifiers and virtual key");
    require(
        !toolbox::parseQuickShortcut(QKeySequence(QStringLiteral("V")), &parsed) &&
            !toolbox::parseQuickShortcut(QKeySequence(QStringLiteral("Ctrl+V, Ctrl+N")), &parsed),
        "Bare keys and multi-chord shortcuts must be rejected");
    auto platform = std::make_unique<FakePlatform>();
    auto* fake = platform.get();
    toolbox::QuickPaste service(nullptr, std::move(platform));
    const auto clipKey = QKeySequence(QStringLiteral("Ctrl+Alt+V")),
               noteKey = QKeySequence(QStringLiteral("Ctrl+Alt+N"));
    require(service.configure(clipKey, noteKey).isEmpty() && fake->keys.size() == 2,
            "Both shortcuts must register");
    fake->blocked = 'B';
    require(!service.configure(QKeySequence(QStringLiteral("Ctrl+Alt+B")), noteKey).isEmpty() &&
                fake->keys.contains('V') && fake->keys.contains('N'),
            "Conflicting shortcut changes must restore the previous bindings");
    require(!service.configure(clipKey, clipKey).isEmpty(), "Duplicate hotkeys must be rejected");
    service.setSuspended(true);
    require(fake->keys.isEmpty(), "Shortcut editing must release hotkeys");
    require(service.setSuspended(false).isEmpty() && fake->keys.size() == 2,
            "Leaving shortcut editing must restore bindings");
    int requests = 0, finishes = 0;
    bool sent = false;
    QObject::connect(&service, &toolbox::QuickPaste::requested, &app, [&](bool notes) {
        require(notes, "Note hotkey mode must propagate");
        ++requests;
    });
    QObject::connect(&service, &toolbox::QuickPaste::finished, &app, [&](bool ok, const QString&) {
        ++finishes;
        sent = ok;
    });
    fake->activated(true);
    require(requests == 1, "Hotkey activation must request the picker");
    QWidget panel;
    panel.show();
    fake->released = false;
    service.paste(QStringLiteral("  原文\n😀  "), {}, &panel);
    QTimer::singleShot(80, &app, [&] {
        require(fake->sends == 0, "Held modifiers must postpone paste");
        fake->released = true;
    });
    require(waitFor([&] { return finishes == 1; }) && sent && fake->sends == 1 &&
                !panel.isVisible(),
            "Paste must wait, restore focus, inject once and hide the picker");
    require(QApplication::clipboard()->text() == QStringLiteral("  原文\n😀  "),
            "Paste must preserve exact Unicode text");
    fake->focusOk = false;
    panel.show();
    service.paste(QStringLiteral("changed focus"), {}, &panel);
    require(waitFor([&] { return finishes == 2; }) && !sent && fake->sends == 1 &&
                panel.isVisible(),
            "Changed focus must prevent input injection and retain the picker");
    fake->focusOk = true;
    fake->alive = false;
    service.paste(QStringLiteral("closed target"), {}, &panel);
    require(finishes == 3 && !sent && fake->sends == 1,
            "Closed target must fall back to clipboard only");
    fake->alive = true;
    fake->released = false;
    service.paste(QStringLiteral("cancel"), {}, &panel);
    service.cancel(&panel);
    require(!service.busy() && fake->sends == 1, "Escape must cancel pending injection");
    fake->released = true;
    fake->sendOk = false;
    service.paste(QStringLiteral("failed send"), {}, &panel);
    require(waitFor([&] { return finishes == 4; }) && !sent && fake->sends == 2,
            "Failed input injection must not report success");
    fake->sendOk = true;
    QImage pasteImage(16, 12, QImage::Format_RGB32);
    pasteImage.fill(Qt::green);
    service.paste({}, pasteImage, &panel);
    require(waitFor([&] { return finishes == 5; }) && sent &&
                QApplication::clipboard()->image() == pasteImage,
            "Image paste must preserve image clipboard data");
    fake->released = false;
    service.paste(QStringLiteral("held keys"), {}, &panel);
    require(waitFor([&] { return finishes == 6; }) && !sent && fake->sends == 3,
            "Held modifiers must time out without injecting input");
    fake->released = true;
    fake->ready = false;
    service.paste(QStringLiteral("延迟恢复焦点"), {}, &panel);
    QTimer::singleShot(420, &app, [&] {
        require(fake->sends == 3, "Paste must wait until the original editor is ready");
        fake->ready = true;
    });
    require(waitFor([&] { return finishes == 7; }) && sent && fake->sends == 4,
            "Delayed editor focus must permit exactly one paste");
    const auto text = QStringLiteral("带图片的正文");
    const auto html = QStringLiteral("<p>带图片的正文</p>");
    service.paste(text, pasteImage, &panel, html);
    const auto* data = QApplication::clipboard()->mimeData();
    require(waitFor([&] { return finishes == 8; }) && sent &&
                data->text() == text && data->html() == html &&
                QApplication::clipboard()->image() == pasteImage,
            "Mixed note paste must publish text, image and HTML together");
    std::cout << "Quick shortcut, paste state and failure-path checks passed\n";
    return 0;
}
