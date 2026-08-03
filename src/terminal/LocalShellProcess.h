#ifndef HSSH_TERMINAL_LOCALSHELLPROCESS_H
#define HSSH_TERMINAL_LOCALSHELLPROCESS_H

#include "ShellProcess.h"

#include <QString>

#include <atomic>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// HPCON is defined in consoleapi.h via windows.h on recent SDKs.
using HPCON = void *;
#else
class QSocketNotifier;
#endif

namespace hssh {

class LocalShellProcess : public ShellProcess {
    Q_OBJECT

public:
    explicit LocalShellProcess(const QString &shellType, QObject *parent = nullptr);
    ~LocalShellProcess() override;

    bool start() override;
    void write(const QByteArray &data) override;
    void resize(int columns, int rows) override;
    void close() override;
    [[nodiscard]] bool isRunning() const override;

    [[nodiscard]] static QString defaultShell();
    [[nodiscard]] static QStringList availableShells();
    [[nodiscard]] static QString shellExecutable(const QString &shellType);
    [[nodiscard]] static QStringList shellArguments(const QString &shellType);

private:
    QString m_shellType;

    struct ShellCommand {
        QString program;
        QStringList args;
        bool valid = false;
    };
    [[nodiscard]] ShellCommand resolveShellCommand() const;
    [[nodiscard]] static QStringList defaultShellList();

#ifdef Q_OS_WIN
    bool initializeConPTY();
    void shutdownConPTY();
    static DWORD WINAPI readerThreadProc(LPVOID param);
    void readerThreadFunc();
    // Waits on the shell process handle; reports natural exits (e.g. the
    // user typed "exit") via the finished signal.
    static DWORD WINAPI watcherThreadProc(LPVOID param);
    Q_INVOKABLE void onProcessExitedNaturally();

    HPCON m_hPC = nullptr;
    HANDLE m_hPipeInRead = INVALID_HANDLE_VALUE;
    HANDLE m_hPipeInWrite = INVALID_HANDLE_VALUE;
    HANDLE m_hPipeOutRead = INVALID_HANDLE_VALUE;
    HANDLE m_hPipeOutWrite = INVALID_HANDLE_VALUE;
    HANDLE m_hProcess = nullptr;
    HANDLE m_hReaderThread = nullptr;
    HANDLE m_hWatcherThread = nullptr;
    bool m_readerStop = false;
    std::atomic<bool> m_shuttingDown{false};
    std::atomic<bool> m_finishedEmitted{false};
    std::atomic<DWORD> m_naturalExitCode{0};
#else
    bool initializeUnixPty();
    void shutdownUnixPty();
    void onMasterFdReady();
    void onSigChldReady();
    bool setupSigChldHandler();
    void emitFinished(int exitCode);

    int m_masterFd = -1;
    pid_t m_childPid = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_sigChldNotifier = nullptr;
    int m_sigChldFds[2] = {-1, -1};
    bool m_exited = false;
#endif
};

} // namespace hssh

#endif // HSSH_TERMINAL_LOCALSHELLPROCESS_H
