#ifndef HSSH_TERMINAL_SERIALSHELLPROCESS_H
#define HSSH_TERMINAL_SERIALSHELLPROCESS_H

#include "ShellProcess.h"

#include <QString>

class QTimer;

namespace hssh {

// PH3-02: serial console (COM/tty) behind the ShellProcess interface, so a
// serial device renders in a normal terminal tab. 8N1, no flow control.
// Implemented on the native APIs (Win32/termios): the Qt SerialPort module
// is not part of this Qt installation.
class SerialShellProcess : public ShellProcess {
    Q_OBJECT

public:
    explicit SerialShellProcess(const QString &portName, int baudRate,
                                QObject *parent = nullptr);
    ~SerialShellProcess() override;

    bool start() override;
    void write(const QByteArray &data) override;
    void resize(int columns, int rows) override; // no PTY on a wire
    void close() override;
    [[nodiscard]] bool isRunning() const override;

    // Windows: "COM3"; POSIX: "/dev/ttyUSB0" (already a full device path).
    [[nodiscard]] static QStringList availablePorts();

private:
    void pollRead();

    QString m_portName;
    int m_baudRate = 115200;
    // Opaque native handle: HANDLE on Windows, int fd on POSIX.
    void *m_handle = nullptr;
    int m_fd = -1;
    QTimer *m_pollTimer = nullptr;
};

} // namespace hssh

#endif // HSSH_TERMINAL_SERIALSHELLPROCESS_H
