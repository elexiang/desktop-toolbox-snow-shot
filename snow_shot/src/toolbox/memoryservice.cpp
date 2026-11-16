// SPDX-License-Identifier: GPL-3.0-or-later
#include "memoryservice.h"
#include "toolboxpaths.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSaveFile>
#include <QUuid>
#include <QRegularExpression>
#include <QtConcurrent>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <vector>

namespace toolbox {
namespace {
struct Process {
    DWORD pid, parent;
    QString name;
};
QList<Process> processes() {
    QList<Process> result;
    HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (handle == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(handle, &entry))
        do {
            result.append({entry.th32ProcessID, entry.th32ParentProcessID,
                           QString::fromWCharArray(entry.szExeFile).toLower()});
        } while (Process32NextW(handle, &entry));
    CloseHandle(handle);
    return result;
}
quint64 privateWorking(HANDLE process) {
    std::vector<unsigned char> storage(65536);
    while (!QueryWorkingSet(process, storage.data(), static_cast<DWORD>(storage.size()))) {
        if (GetLastError() != ERROR_BAD_LENGTH || storage.size() >= 16 * 1024 * 1024)
            return 0;
        storage.resize(storage.size() * 2);
    }
    const auto* working = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(storage.data());
    quint64 pages = 0;
    for (ULONG_PTR i = 0; i < working->NumberOfEntries; ++i)
        if (!working->WorkingSetInfo[i].Shared)
            ++pages;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    return pages * system.dwPageSize;
}
bool privilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES value{};
    value.PrivilegeCount = 1;
    LookupPrivilegeValueW(nullptr, name, &value.Privileges[0].Luid);
    value.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(0);
    const bool result =
        AdjustTokenPrivileges(token, FALSE, &value, 0, nullptr, nullptr) && GetLastError() == 0;
    CloseHandle(token);
    return result;
}
bool sameUser(HANDLE process) {
    HANDLE other = nullptr, own = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &other))
        return false;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own)) {
        CloseHandle(other);
        return false;
    }
    DWORD sizeA = 0, sizeB = 0;
    GetTokenInformation(other, TokenUser, nullptr, 0, &sizeA);
    GetTokenInformation(own, TokenUser, nullptr, 0, &sizeB);
    std::vector<unsigned char> a(sizeA), b(sizeB);
    const bool result = GetTokenInformation(other, TokenUser, a.data(), sizeA, &sizeA) &&
                        GetTokenInformation(own, TokenUser, b.data(), sizeB, &sizeB) &&
                        EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid,
                                 reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid);
    CloseHandle(other);
    CloseHandle(own);
    return result;
}
} // namespace
MemoryService::MemoryService(QObject* parent) : QObject(parent) {
    m_timer.setInterval(15000);
    connect(&m_timer, &QTimer::timeout, this, &MemoryService::refresh);
    connect(&m_cleanup, &QFutureWatcher<QString>::finished, this, [this] {
        m_busy = false;
        emit cleaned(m_cleanup.result());
        refresh();
    });
}
MemoryService::~MemoryService() {
    m_cleanup.waitForFinished();
}
void MemoryService::setEnabled(bool value) {
    if (value) {
        m_timer.start();
        refresh();
    } else {
        m_timer.stop();
        m_snapshot = {};
        emit changed();
    }
}
void MemoryService::refresh() {
    m_snapshot = collect();
    emit changed();
}
MemorySnapshot MemoryService::collect() {
    MemorySnapshot result;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        result.total = memory.ullTotalPhys;
        result.available = memory.ullAvailPhys;
    }
    const auto list = processes();
    QSet<DWORD> owned{GetCurrentProcessId()};
    for (int pass = 0; pass < list.size(); ++pass) {
        const auto before = owned.size();
        for (const auto& p : list)
            if (owned.contains(p.parent))
                owned.insert(p.pid);
        if (before == owned.size())
            break;
    }
    for (DWORD pid : owned) {
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!process)
            continue;
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters))) {
            result.privateWorking += privateWorking(process);
            result.committed += counters.PrivateUsage;
            ++result.ownedProcesses;
        }
        CloseHandle(process);
    }
    return result;
}
void MemoryService::clean(bool deep) {
    if (m_busy || !enabled(QStringLiteral("cleanup")))
        return;
    if (!deep) {
        m_busy = true;
        m_cleanup.setFuture(QtConcurrent::run([] {
            const auto list = processes();
            QSet<DWORD> protect{GetCurrentProcessId()};
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            protect.insert(foreground);
            for (const auto& p : list)
                if (p.name.contains(QStringLiteral("idea")) ||
                    p.name.contains(QStringLiteral("codex")) ||
                    p.name.contains(QStringLiteral("chatgpt")) ||
                    p.name == QStringLiteral("java.exe") || p.name == QStringLiteral("javaw.exe") ||
                    p.name == QStringLiteral("node.exe") || p.name == QStringLiteral("devenv.exe"))
                    protect.insert(p.pid);
            for (int pass = 0; pass < list.size(); ++pass) {
                const auto before = protect.size();
                for (const auto& p : list)
                    if (protect.contains(p.parent))
                        protect.insert(p.pid);
                if (before == protect.size())
                    break;
            }
            int ok = 0, skipped = 0;
            for (const auto& p : list) {
                if (p.pid < 100 || protect.contains(p.pid)) {
                    ++skipped;
                    continue;
                }
                HANDLE process =
                    OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA, FALSE, p.pid);
                if (!process) {
                    ++skipped;
                    continue;
                }
                wchar_t path[32768];
                DWORD length = 32768;
                const bool safe =
                    QueryFullProcessImageNameW(process, 0, path, &length) && sameUser(process) &&
                    !QString::fromWCharArray(path, static_cast<int>(length))
                         .startsWith(qEnvironmentVariable("WINDIR"), Qt::CaseInsensitive);
                if (safe && EmptyWorkingSet(process))
                    ++ok;
                else
                    ++skipped;
                CloseHandle(process);
            }
            return tr("Regular cleanup: %1 background processes trimmed; %2 protected or skipped. "
                      "Pages may return when used")
                .arg(ok)
                .arg(skipped);
        }));
        return;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString args = QStringLiteral("--toolbox-memory-helper ") + id;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = reinterpret_cast<LPCWSTR>(exe.utf16());
    info.lpParameters = reinterpret_cast<LPCWSTR>(args.utf16());
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        emit cleaned(
            tr("Deep cleanup was not performed. Authorization cancelled or launch failed (%1)")
                .arg(GetLastError()));
        return;
    }
    m_busy = true;
    auto* poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this, poll, process = info.hProcess, id] {
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
            return;
        CloseHandle(process);
        poll->stop();
        poll->deleteLater();
        m_busy = false;
        QFile file(dataPath() + QStringLiteral("/cleanup-") + id + QStringLiteral(".json"));
        if (!file.open(QIODevice::ReadOnly)) {
            emit cleaned(tr("The cleanup helper exited without a result"));
            return;
        }
        const auto result = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        file.remove();
        emit cleaned(tr("Deep cleanup: %1 of %2 system operations succeeded. Available RAM: %3 → "
                        "%4 MiB (instant observation)")
                         .arg(result.value(QStringLiteral("succeeded")).toInt())
                         .arg(result.value(QStringLiteral("operations")).toInt())
                         .arg(result.value(QStringLiteral("before")).toInteger() / 1048576)
                         .arg(result.value(QStringLiteral("after")).toInteger() / 1048576));
        refresh();
    });
    poll->start(200);
}
int MemoryService::runDeepHelper(const QString& id) {
    if (!QRegularExpression(QStringLiteral("^[a-f0-9-]{36}$")).match(id).hasMatch())
        return 64;
    MEMORYSTATUSEX before{};
    before.dwLength = sizeof(before);
    GlobalMemoryStatusEx(&before);
    const bool profile = privilege(SE_PROF_SINGLE_PROCESS_NAME),
               quota = privilege(SE_INCREASE_QUOTA_NAME);
    using NtSet = LONG(WINAPI*)(int, void*, ULONG);
    const auto ntSet = reinterpret_cast<NtSet>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetSystemInformation"));
    int succeeded = 0;
    QJsonArray statuses;
    for (ULONG command : {2UL, 3UL, 4UL}) {
        const LONG status =
            profile && ntSet ? ntSet(80, &command, sizeof(command)) : LONG(0xC0000061);
        if (status >= 0)
            ++succeeded;
        statuses.append(QStringLiteral("0x%1").arg(quint32(status), 8, 16, QLatin1Char('0')));
    }
    if (quota && SetSystemFileCacheSize(SIZE_T(-1), SIZE_T(-1), 0))
        ++succeeded;
    MEMORYSTATUSEX after{};
    after.dwLength = sizeof(after);
    GlobalMemoryStatusEx(&after);
    QSaveFile file(dataPath() + QStringLiteral("/cleanup-") + id + QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly))
        return 2;
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("succeeded"), succeeded},
                                         {QStringLiteral("operations"), 4},
                                         {QStringLiteral("before"), qint64(before.ullAvailPhys)},
                                         {QStringLiteral("after"), qint64(after.ullAvailPhys)},
                                         {QStringLiteral("status"), statuses}})
                   .toJson());
    return file.commit() && succeeded == 4 ? 0 : 1;
}
} // namespace toolbox
