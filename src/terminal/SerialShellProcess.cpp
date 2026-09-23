#include "SerialShellProcess.h"

#include <QTimer>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <string.h>
#  include <termios.h>
#  include <unistd.h>
#endif

namespace hssh {

namespace {

constexpr int pollIntervalMs = 20;

#ifdef Q_OS_WIN

bool baudToDcb(DWORD baud, DCB &dcb)
{
    dcb.BaudRate = baud;
    return baud == 110 || baud == 300 || baud == 600 || baud == 1200
        || baud == 2400 || baud == 4800 || baud == 9600 || baud == 14400
        || baud == 19200 || baud == 38400 || baud == 57600 || baud == 115200
        || baud == 230400 || baud == 460800 || baud == 921600;
}

#endif

} // namespace

SerialShellProcess::SerialShellProcess(const QString &portName, int baudRate,
                                       QObject *parent)
    : ShellProcess(parent)
    , m_portName(portName)
    , m_baudRate(baudRate > 0 ? baudRate : 115200)
{
}

SerialShellProcess::~SerialShellProcess()
{
    close();
}

QStringList SerialShellProcess::availablePorts()
{
    QStringList names;
#ifdef Q_OS_WIN
    // Probe COM1..COM255; opening with GENERIC_READ|WRITE fails fast for
    // nonexistent devices (no blocking).
    for (int i = 1; i <= 255; ++i) {
        const QString name = QStringLiteral("COM%1").arg(i);
        const std::wstring path = (QStringLiteral("\\\\.\\") + name).toStdWString();
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            names.append(name);
            CloseHandle(h);
        }
    }
#else
    DIR *dir = opendir("/dev");
    if (dir) {
        while (dirent *e = readdir(dir)) {
            const QString name = QString::fromLatin1(e->d_name);
            if (name.startsWith(QLatin1String("ttyUSB"))
                || name.startsWith(QLatin1String("ttyACM"))
                || name.startsWith(QLatin1String("ttyS"))
                || name.startsWith(QLatin1String("ttyAMA"))) {
                names.append(QStringLiteral("/dev/") + name);
            }
        }
        closedir(dir);
    }
    names.sort();
#endif
    return names;
}

bool SerialShellProcess::start()
{
    if (m_portName.isEmpty()) {
        emit errorOccurred(tr("No serial port configured"));
        return false;
    }
    if (isRunning()) {
        return true;
    }

#ifdef Q_OS_WIN
    const std::wstring path = m_portName.startsWith(QLatin1String("\\\\.\\"))
        ? m_portName.toStdWString()
        : (QStringLiteral("\\\\.\\") + m_portName).toStdWString();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        emit errorOccurred(tr("Cannot open serial port %1 (error %2)")
                               .arg(m_portName).arg(static_cast<int>(GetLastError())));
        return false;
    }
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    if (!baudToDcb(static_cast<DWORD>(m_baudRate), dcb)) {
        emit errorOccurred(tr("Unsupported baud rate %1").arg(m_baudRate));
        CloseHandle(h);
        return false;
    }
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(h, &dcb)) {
        emit errorOccurred(tr("Cannot configure serial port %1").arg(m_portName));
        CloseHandle(h);
        return false;
    }
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;      // nonblocking reads
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(h, &timeouts);
    m_handle = h;
#else
    const int fd = ::open(m_portName.toUtf8().constData(), O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        emit errorOccurred(tr("Cannot open serial port %1: %2")
                               .arg(m_portName, QString::fromLatin1(strerror(errno))));
        return false;
    }
    termios tio{};
    if (tcgetattr(fd, &tio) != 0
        || cfsetispeed(&tio, static_cast<speed_t>(m_baudRate)) != 0
        || cfsetospeed(&tio, static_cast<speed_t>(m_baudRate)) != 0) {
        emit errorOccurred(tr("Cannot configure serial port %1").arg(m_portName));
        ::close(fd);
        return false;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0; // fully nonblocking reads
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        emit errorOccurred(tr("Cannot configure serial port %1").arg(m_portName));
        ::close(fd);
        return false;
    }
    m_fd = fd;
#endif

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(pollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &SerialShellProcess::pollRead);
    m_pollTimer->start();
    return true;
}

void SerialShellProcess::pollRead()
{
    char buffer[4096];
#ifdef Q_OS_WIN
    DWORD n = 0;
    if (!ReadFile(static_cast<HANDLE>(m_handle), buffer, sizeof(buffer), &n, nullptr)) {
        emit errorOccurred(tr("Serial port %1 read failed (error %2)")
                               .arg(m_portName).arg(static_cast<int>(GetLastError())));
        close();
        return;
    }
    if (n > 0) {
        emit dataReceived(QByteArray(buffer, static_cast<int>(n)));
    }
#else
    const ssize_t n = ::read(m_fd, buffer, sizeof(buffer));
    if (n > 0) {
        emit dataReceived(QByteArray(buffer, static_cast<int>(n)));
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        emit errorOccurred(tr("Serial port %1 read failed: %2")
                               .arg(m_portName, QString::fromLatin1(strerror(errno))));
        close();
    }
#endif
}

void SerialShellProcess::write(const QByteArray &data)
{
    if (!isRunning()) {
        return;
    }
#ifdef Q_OS_WIN
    DWORD written = 0;
    WriteFile(static_cast<HANDLE>(m_handle), data.constData(),
              static_cast<DWORD>(data.size()), &written, nullptr);
#else
    (void)::write(m_fd, data.constData(), static_cast<size_t>(data.size()));
#endif
}

void SerialShellProcess::resize(int, int)
{
    // A wire has no PTY size.
}

void SerialShellProcess::close()
{
    if (m_pollTimer) {
        m_pollTimer->stop();
        m_pollTimer->deleteLater();
        m_pollTimer = nullptr;
    }
#ifdef Q_OS_WIN
    if (m_handle) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
        emit finished(0);
    }
#else
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
        emit finished(0);
    }
#endif
}

bool SerialShellProcess::isRunning() const
{
#ifdef Q_OS_WIN
    return m_handle != nullptr;
#else
    return m_fd >= 0;
#endif
}

} // namespace hssh
