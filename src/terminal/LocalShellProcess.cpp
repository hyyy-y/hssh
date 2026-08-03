#include "LocalShellProcess.h"

#include <QDebug>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace hssh {

LocalShellProcess::LocalShellProcess(const QString &shellType, QObject *parent)
    : ShellProcess(parent)
    , m_shellType(shellType)
{
}

LocalShellProcess::~LocalShellProcess()
{
    close();
}

LocalShellProcess::ShellCommand LocalShellProcess::resolveShellCommand() const
{
    const QString shell = m_shellType.isEmpty() ? defaultShell() : m_shellType;
    const QString program = shellExecutable(shell);
    if (program.isEmpty()) {
        return {};
    }
    return {program, shellArguments(shell), true};
}

QStringList LocalShellProcess::defaultShellList()
{
#ifdef Q_OS_WIN
    return {QStringLiteral("PowerShell"), QStringLiteral("CMD"), QStringLiteral("WSL")};
#else
    return {QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish")};
#endif
}

QString LocalShellProcess::defaultShell()
{
#ifdef Q_OS_WIN
    for (const QString &name : defaultShellList()) {
        if (!shellExecutable(name).isEmpty()) {
            return name;
        }
    }
    return QStringLiteral("PowerShell");
#else
    const QString shellEnv = QProcessEnvironment::systemEnvironment().value(QStringLiteral("SHELL"));
    if (!shellEnv.isEmpty()) {
        const QFileInfo info(shellEnv);
        const QString baseName = info.fileName();
        if (!shellExecutable(baseName).isEmpty()) {
            return baseName;
        }
    }
    for (const QString &name : defaultShellList()) {
        if (!shellExecutable(name).isEmpty()) {
            return name;
        }
    }
    return QStringLiteral("bash");
#endif
}

QStringList LocalShellProcess::availableShells()
{
    QStringList result;
    for (const QString &name : defaultShellList()) {
        if (!shellExecutable(name).isEmpty()) {
            result.append(name);
        }
    }
#ifndef Q_OS_WIN
    const QString shellEnv = QProcessEnvironment::systemEnvironment().value(QStringLiteral("SHELL"));
    if (!shellEnv.isEmpty()) {
        const QString baseName = QFileInfo(shellEnv).fileName();
        if (!result.contains(baseName) && !shellExecutable(baseName).isEmpty()) {
            result.append(baseName);
        }
    }
#endif
    return result;
}

QString LocalShellProcess::shellExecutable(const QString &shellType)
{
    if (shellType.isEmpty()) {
        return {};
    }

#ifdef Q_OS_WIN
    if (shellType == QStringLiteral("PowerShell")) {
        // Prefer PowerShell 7 (pwsh.exe), fall back to Windows PowerShell.
        const QString pwsh = QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
        if (!pwsh.isEmpty()) {
            return pwsh;
        }
        return QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    }
    if (shellType == QStringLiteral("CMD")) {
        return QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    }
    if (shellType == QStringLiteral("WSL")) {
        return QStandardPaths::findExecutable(QStringLiteral("wsl.exe"));
    }
    return QStandardPaths::findExecutable(shellType);
#else
    if (shellType == QStringLiteral("bash")) {
        return QFileInfo::exists(QStringLiteral("/bin/bash")) ? QStringLiteral("/bin/bash") : QStandardPaths::findExecutable(QStringLiteral("bash"));
    }
    if (shellType == QStringLiteral("zsh")) {
        return QFileInfo::exists(QStringLiteral("/bin/zsh")) ? QStringLiteral("/bin/zsh") : QStandardPaths::findExecutable(QStringLiteral("zsh"));
    }
    if (shellType == QStringLiteral("fish")) {
        return QFileInfo::exists(QStringLiteral("/usr/bin/fish")) ? QStringLiteral("/usr/bin/fish") : QStandardPaths::findExecutable(QStringLiteral("fish"));
    }
    return QStandardPaths::findExecutable(shellType);
#endif
}

QStringList LocalShellProcess::shellArguments(const QString &shellType)
{
#ifdef Q_OS_WIN
    if (shellType == QStringLiteral("PowerShell")) {
        return {QStringLiteral("-NoLogo")};
    }
    return {};
#else
    Q_UNUSED(shellType)
    return {QStringLiteral("-l")};
#endif
}

} // namespace hssh
