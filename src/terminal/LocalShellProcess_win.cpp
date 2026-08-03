#include "LocalShellProcess.h"

#include <QDebug>
#include <QDir>
#include <QMetaObject>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace hssh {

namespace {

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

using CreatePseudoConsoleFn = HRESULT(WINAPI *)(COORD size, HANDLE hInput, HANDLE hOutput, DWORD dwFlags, HPCON *phPC);
using ResizePseudoConsoleFn = HRESULT(WINAPI *)(HPCON hPc, COORD size);
// ConptyClosePseudoConsole (bundled conpty.dll) returns HRESULT while the
// inbox ClosePseudoConsole returns void; the call sites ignore the result.
using ClosePseudoConsoleFn = HRESULT(WINAPI *)(HPCON hPc);
using ShowHidePseudoConsoleFn = HRESULT(WINAPI *)(HPCON hPc, BOOL show);
using InitializeProcThreadAttributeListFn = BOOL(WINAPI *)(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, DWORD dwAttributeCount, DWORD dwFlags, PSIZE_T lpSize);
using UpdateProcThreadAttributeFn = BOOL(WINAPI *)(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, DWORD dwFlags, DWORD_PTR Attribute, PVOID lpValue, SIZE_T cbSize, PVOID lpPreviousValue, PSIZE_T lpReturnSize);
using DeleteProcThreadAttributeListFn = void(WINAPI *)(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList);

struct ConPTYApi {
    CreatePseudoConsoleFn createPseudoConsole = nullptr;
    ResizePseudoConsoleFn resizePseudoConsole = nullptr;
    ClosePseudoConsoleFn closePseudoConsole = nullptr;
    ShowHidePseudoConsoleFn showHidePseudoConsole = nullptr; // optional, bundled DLL only
    InitializeProcThreadAttributeListFn initializeProcThreadAttributeList = nullptr;
    UpdateProcThreadAttributeFn updateProcThreadAttribute = nullptr;
    DeleteProcThreadAttributeListFn deleteProcThreadAttributeList = nullptr;
    bool valid = false;
};

ConPTYApi loadConPTYApi()
{
    ConPTYApi api;

    // Prefer the bundled Microsoft Terminal implementation (conpty.dll +
    // OpenConsole.exe next to the executable). The inbox conhost shipped
    // with Windows 11 26100+ kills clients that attach to a pseudoconsole,
    // so the system CreatePseudoConsole is only a fallback for older Windows.
    HMODULE lib = nullptr;
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        if (wchar_t *slash = wcsrchr(exePath, L'\\')) {
            wcscpy(slash + 1, L"conpty.dll");
            lib = LoadLibraryW(exePath);
        }
    }
    if (lib) {
        api.createPseudoConsole = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(lib, "ConptyCreatePseudoConsole"));
        api.resizePseudoConsole = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(lib, "ConptyResizePseudoConsole"));
        api.closePseudoConsole = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(lib, "ConptyClosePseudoConsole"));
        api.showHidePseudoConsole = reinterpret_cast<ShowHidePseudoConsoleFn>(GetProcAddress(lib, "ConptyShowHidePseudoConsole"));
    }
    if (!api.createPseudoConsole || !api.resizePseudoConsole || !api.closePseudoConsole) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (kernel32) {
            api.createPseudoConsole = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel32, "CreatePseudoConsole"));
            api.resizePseudoConsole = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(kernel32, "ResizePseudoConsole"));
            api.closePseudoConsole = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel32, "ClosePseudoConsole"));
            api.showHidePseudoConsole = nullptr;
        }
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32) {
        api.initializeProcThreadAttributeList = reinterpret_cast<InitializeProcThreadAttributeListFn>(GetProcAddress(kernel32, "InitializeProcThreadAttributeList"));
        api.updateProcThreadAttribute = reinterpret_cast<UpdateProcThreadAttributeFn>(GetProcAddress(kernel32, "UpdateProcThreadAttribute"));
        api.deleteProcThreadAttributeList = reinterpret_cast<DeleteProcThreadAttributeListFn>(GetProcAddress(kernel32, "DeleteProcThreadAttributeList"));
    }
    api.valid = api.createPseudoConsole && api.resizePseudoConsole && api.closePseudoConsole
                && api.initializeProcThreadAttributeList && api.updateProcThreadAttribute && api.deleteProcThreadAttributeList;
    return api;
}

// A pseudoconsole client only attaches successfully when the parent's
// standard handles refer to a console at CreateProcess time (this is what
// pywinpty / Windows Terminal do). GUI applications have no console, so one
// is allocated (hidden) once per process and kept alive; start() then swaps
// the std handles onto it just around CreateProcessW so the rest of the
// process (tests, logging) keeps its original stdout/stderr.
struct ConsoleHandles {
    HANDLE out = INVALID_HANDLE_VALUE;
    HANDLE in = INVALID_HANDLE_VALUE;
    bool ok = false;
};

ConsoleHandles acquireConsoleHandles()
{
    static const ConsoleHandles handles = [] {
        ConsoleHandles result;
        result.out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (result.out == INVALID_HANDLE_VALUE && AllocConsole()) {
            if (HWND consoleWindow = GetConsoleWindow()) {
                ShowWindow(consoleWindow, SW_HIDE);
            }
            result.out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (result.out == INVALID_HANDLE_VALUE) {
            qWarning() << "LocalShellProcess: unable to acquire a console for ConPTY";
            return result;
        }
        // The pseudoconsole child inherits its initial codepage from this
        // console. Without UTF-8 here OpenConsole emits non-ASCII text (e.g.
        // Chinese cmd output) in the system OEM codepage (GBK) while the
        // terminal decodes UTF-8, producing garbled characters.
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        DWORD mode = 0;
        if (GetConsoleMode(result.out, &mode)) {
            SetConsoleMode(result.out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
        result.in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        result.ok = true;
        return result;
    }();
    return handles;
}

} // namespace

bool LocalShellProcess::start()
{
    if (m_hProcess != nullptr) {
        emit errorOccurred(tr("Local shell process is already running"));
        return false;
    }

    const ConPTYApi api = loadConPTYApi();
    if (!api.valid) {
        emit errorOccurred(tr("ConPTY is not available on this system (requires Windows 10 1809+)"));
        return false;
    }

    const ConsoleHandles console = acquireConsoleHandles();
    if (!console.ok) {
        emit errorOccurred(tr("Failed to allocate a console required for the pseudo terminal"));
        return false;
    }

    const ShellCommand cmd = resolveShellCommand();
    if (!cmd.valid || cmd.program.isEmpty()) {
        emit errorOccurred(tr("Shell type '%1' is not available on this system").arg(m_shellType));
        return false;
    }

    initializeConPTY();

    SECURITY_ATTRIBUTES sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hPipeInRead = INVALID_HANDLE_VALUE;
    HANDLE hPipeInWrite = INVALID_HANDLE_VALUE;
    HANDLE hPipeOutRead = INVALID_HANDLE_VALUE;
    HANDLE hPipeOutWrite = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&hPipeInRead, &hPipeInWrite, &sa, 0)
        || !CreatePipe(&hPipeOutRead, &hPipeOutWrite, &sa, 0)) {
        emit errorOccurred(tr("Failed to create pipes for ConPTY"));
        shutdownConPTY();
        return false;
    }

    SetHandleInformation(hPipeInWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hPipeOutRead, HANDLE_FLAG_INHERIT, 0);

    const COORD consoleSize = {static_cast<SHORT>(m_size.width()), static_cast<SHORT>(m_size.height())};
    const HRESULT hr = api.createPseudoConsole(consoleSize, hPipeInRead, hPipeOutWrite, 0, &m_hPC);

    if (FAILED(hr)) {
        emit errorOccurred(tr("CreatePseudoConsole failed: 0x%1").arg(QString::number(static_cast<quint32>(hr), 16)));
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        CloseHandle(hPipeOutWrite);
        initializeConPTY();
        return false;
    }

    m_hPipeInRead = hPipeInRead;
    m_hPipeInWrite = hPipeInWrite;
    m_hPipeOutRead = hPipeOutRead;
    m_hPipeOutWrite = hPipeOutWrite;

    if (api.showHidePseudoConsole) {
        api.showHidePseudoConsole(m_hPC, FALSE);
    }

    // Build command line. Arguments are simple in Phase 1; no complex quoting needed.
    // Use native separators: cmd.exe's own command-line parser misbehaves
    // with forward slashes in the executable path.
    QString commandLine = QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(cmd.program));
    for (const QString &arg : cmd.args) {
        commandLine += QStringLiteral(" %1").arg(arg);
    }

    SIZE_T attrListSize = 0;
    api.initializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attrListSize));
    if (!attrList
        || !api.initializeProcThreadAttributeList(attrList, 1, 0, &attrListSize)
        || !api.updateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, m_hPC, sizeof(m_hPC), nullptr, nullptr)) {
        emit errorOccurred(tr("Failed to initialize process thread attribute list"));
        if (attrList) {
            HeapFree(GetProcessHeap(), 0, attrList);
        }
        shutdownConPTY();
        return false;
    }

    STARTUPINFOEXW siEx;
    std::memset(&siEx, 0, sizeof(siEx));
    siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    siEx.lpAttributeList = attrList;

    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof(pi));

    const std::wstring cmdLineW = commandLine.toStdWString();
    std::vector<wchar_t> cmdLineBuffer(cmdLineW.begin(), cmdLineW.end());
    cmdLineBuffer.push_back(L'\0');

    SetEnvironmentVariableW(L"TERM", L"xterm-256color");

    // The child only attaches to the pseudoconsole if our std handles refer
    // to a console at creation time. Swap them in just for CreateProcessW.
    const HANDLE savedOut = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE savedErr = GetStdHandle(STD_ERROR_HANDLE);
    const HANDLE savedIn = GetStdHandle(STD_INPUT_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, console.out);
    SetStdHandle(STD_ERROR_HANDLE, console.out);
    if (console.in != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_INPUT_HANDLE, console.in);
    }

    BOOL created = CreateProcessW(nullptr,
                                  cmdLineBuffer.data(),
                                  nullptr,
                                  nullptr,
                                  FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT,
                                  nullptr,
                                  nullptr,
                                  &siEx.StartupInfo,
                                  &pi);

    SetStdHandle(STD_OUTPUT_HANDLE, savedOut);
    SetStdHandle(STD_ERROR_HANDLE, savedErr);
    SetStdHandle(STD_INPUT_HANDLE, savedIn);

    SetEnvironmentVariableW(L"TERM", nullptr);

    api.deleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);

    if (!created) {
        emit errorOccurred(tr("Failed to start shell process: %1").arg(QString::number(GetLastError())));
        shutdownConPTY();
        return false;
    }

    m_hProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    m_readerStop = false;
    m_hReaderThread = CreateThread(nullptr, 0, &LocalShellProcess::readerThreadProc, this, 0, nullptr);
    if (!m_hReaderThread) {
        emit errorOccurred(tr("Failed to start ConPTY reader thread"));
        shutdownConPTY();
        return false;
    }

    // Watch for the shell exiting on its own ("exit", Ctrl+D, ...) so the
    // finished signal fires without waiting for close().
    m_shuttingDown = false;
    m_finishedEmitted = false;
    m_hWatcherThread = CreateThread(nullptr, 0, &LocalShellProcess::watcherThreadProc, this, 0, nullptr);
    if (!m_hWatcherThread) {
        // Non-fatal: natural exits just won't be reported.
        qWarning() << "Failed to start ConPTY watcher thread:" << GetLastError();
    }

    return true;
}

void LocalShellProcess::write(const QByteArray &data)
{
    if (m_hPipeInWrite == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(m_hPipeInWrite, data.constData(), static_cast<DWORD>(data.size()), &written, nullptr);
}

void LocalShellProcess::resize(int columns, int rows)
{
    m_size = QSize(columns, rows);
    if (!m_hPC) {
        return;
    }
    const ConPTYApi api = loadConPTYApi();
    if (!api.valid) {
        return;
    }
    const COORD consoleSize = {static_cast<SHORT>(columns), static_cast<SHORT>(rows)};
    api.resizePseudoConsole(m_hPC, consoleSize);
}

void LocalShellProcess::close()
{
    if (!m_hProcess && !m_hPC) {
        return;
    }

    shutdownConPTY();
}

bool LocalShellProcess::isRunning() const
{
    if (!m_hProcess) {
        return false;
    }
    return WaitForSingleObject(m_hProcess, 0) == WAIT_TIMEOUT;
}

bool LocalShellProcess::initializeConPTY()
{
    m_hPC = nullptr;
    m_hPipeInRead = INVALID_HANDLE_VALUE;
    m_hPipeInWrite = INVALID_HANDLE_VALUE;
    m_hPipeOutRead = INVALID_HANDLE_VALUE;
    m_hPipeOutWrite = INVALID_HANDLE_VALUE;
    m_hProcess = nullptr;
    m_hReaderThread = nullptr;
    m_hWatcherThread = nullptr;
    m_readerStop = false;
    m_shuttingDown = false;
    m_finishedEmitted = false;
    return true;
}

void LocalShellProcess::shutdownConPTY()
{
    m_shuttingDown = true;
    m_readerStop = true;

    // Close the pseudoconsole first. This makes the ConPTY server
    // (OpenConsole.exe) tear down and close its ends of the pipes, which is
    // what unblocks the reader thread waiting in ReadFile.
    if (m_hPC) {
        const ConPTYApi api = loadConPTYApi();
        if (api.closePseudoConsole) {
            api.closePseudoConsole(m_hPC);
        }
        m_hPC = nullptr;
    }

    // Closing the pseudoconsole does not reliably terminate the client with
    // the bundled OpenConsole implementation, so force-kill the shell after
    // a short grace period. Without this the shell (cmd/pwsh) and
    // OpenConsole.exe leak every time a tab is closed.
    DWORD exitCode = 0;
    if (m_hProcess) {
        if (WaitForSingleObject(m_hProcess, 1000) == WAIT_TIMEOUT) {
            TerminateProcess(m_hProcess, 1);
            WaitForSingleObject(m_hProcess, 1000);
        }
        GetExitCodeProcess(m_hProcess, &exitCode);
    }

    // Close our ends of the pipes. Closing the output write end unblocks the
    // reader thread that is waiting on the output read end.
    if (m_hPipeInWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipeInWrite);
        m_hPipeInWrite = INVALID_HANDLE_VALUE;
    }
    if (m_hPipeOutWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipeOutWrite);
        m_hPipeOutWrite = INVALID_HANDLE_VALUE;
    }

    if (m_hReaderThread) {
        WaitForSingleObject(m_hReaderThread, 2000);
        CloseHandle(m_hReaderThread);
        m_hReaderThread = nullptr;
    }

    // The watcher waits on m_hProcess; collect it before closing that handle.
    // By now the process is signaled, so the watcher has already woken (and
    // skipped its finished emission because m_shuttingDown is set).
    if (m_hWatcherThread) {
        WaitForSingleObject(m_hWatcherThread, 2000);
        CloseHandle(m_hWatcherThread);
        m_hWatcherThread = nullptr;
    }

    if (m_hPipeOutRead != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipeOutRead);
        m_hPipeOutRead = INVALID_HANDLE_VALUE;
    }

    if (m_hPipeInRead != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipeInRead);
        m_hPipeInRead = INVALID_HANDLE_VALUE;
    }

    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
        if (!m_finishedEmitted.exchange(true)) {
            emit finished(static_cast<int>(exitCode));
        }
    }
}

DWORD WINAPI LocalShellProcess::watcherThreadProc(LPVOID param)
{
    auto *self = static_cast<LocalShellProcess *>(param);
    WaitForSingleObject(self->m_hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(self->m_hProcess, &exitCode);
    self->m_naturalExitCode = exitCode;

    if (!self->m_shuttingDown) {
        // Back on the GUI thread; dropped automatically if the object is
        // already gone.
        QMetaObject::invokeMethod(self, &LocalShellProcess::onProcessExitedNaturally,
                                  Qt::QueuedConnection);
    }
    return 0;
}

void LocalShellProcess::onProcessExitedNaturally()
{
    if (m_finishedEmitted.exchange(true)) {
        return;
    }
    // Keep m_hProcess open (isRunning() still reads it); close()/the
    // destructor do the real cleanup through shutdownConPTY().
    emit finished(static_cast<int>(m_naturalExitCode.load()));
}

DWORD WINAPI LocalShellProcess::readerThreadProc(LPVOID param)
{
    auto *self = static_cast<LocalShellProcess *>(param);
    self->readerThreadFunc();
    return 0;
}

void LocalShellProcess::readerThreadFunc()
{
    std::array<char, 4096> buffer;
    while (!m_readerStop) {
        DWORD bytesRead = 0;
        const BOOL ok = ReadFile(m_hPipeOutRead, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr);
        if (!ok || bytesRead == 0) {
            qDebug() << "Reader thread exiting, ok=" << ok << " error=" << GetLastError() << " bytesRead=" << bytesRead;
            break;
        }
        qDebug() << "Reader thread read" << bytesRead << "bytes";
        emit dataReceived(QByteArray(buffer.data(), static_cast<int>(bytesRead)));
    }
}

} // namespace hssh
