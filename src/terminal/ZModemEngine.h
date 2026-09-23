#ifndef HSSH_TERMINAL_ZMODEMENGINE_H
#define HSSH_TERMINAL_ZMODEMENGINE_H

#include <QObject>
#include <QTimer>
#include <QFile>

namespace hssh {

// PH2-10: ZMODEM receiver. Watches the process->terminal byte stream; when
// the remote starts `sz` (ZRQINIT "**\x18B..." hex frame), it takes over
// the stream, runs the protocol and writes the file to the download
// directory, then hands the terminal back. Byte-level formats were
// calibrated against real lrzsz traffic (2026-09-20):
//   hex frame  : "**\x18" 'B' + 14 hex chars (type,f0..f3,crc16) + CR 0x8A XON
//   bin32 frame: "*\x18" 'C' + type + f0..f3 + crc32(4, ZDLE-escaped)
//   subpacket  : data + ZDLE + terminator('h'.. 'k') + crc32(4, escaped)
class ZModemEngine : public QObject {
    Q_OBJECT

public:
    explicit ZModemEngine(QObject *parent = nullptr);

    // Feed raw process output. Returns the bytes that should still be
    // DISPLAYED in the terminal (consumed protocol bytes are kept).
    QByteArray feed(const QByteArray &data);

    [[nodiscard]] bool isActive() const { return m_state != State::Idle; }

    // Abort an active transfer (user Ctrl+C in the terminal).
    void abort();

    // Directory where received files are written (default: the user's
    // download location). Used by tests to keep transfers contained.
    void setDownloadDirectory(const QString &dir) { m_downloadDir = dir; }


    // PH2-10 send direction: push a local file to the remote. The engine
    // types "rz" into the terminal, waits for the remote rz to answer
    // ZRINIT, then runs the sz role (ZRQINIT -> ZFILE -> ZDATA -> ZEOF ->
    // ZFIN). Returns false if a transfer is already active or the file
    // cannot be read.
    bool startSend(const QString &localPath);

signals:
    // Bytes to write TO the remote (protocol responses).
    void output(const QByteArray &data);
    // Human-readable status lines for the terminal.
    void status(const QString &line);
    void finished(bool ok, const QString &filePath);

private:
    enum class State {
        Idle,       // scanning for ZRQINIT
        Handshake,  // sent ZRINIT, waiting ZFILE
        Data,       // receiving ZDATA subpackets
        Done,       // ZEOF seen, finishing
        // Send direction (we are the sz side).
        SendWaitRinit, // sent ZRQINIT, waiting for the remote ZRINIT
        SendWaitPos,   // sent ZFILE, waiting for ZRPOS
        SendData,      // streaming ZDATA subpackets
        SendEof,       // sent ZEOF, waiting for ZFIN
    };

    void handleFrame(unsigned char type, const unsigned char *flags);
    void handleSubpacket(const QByteArray &payload, unsigned char terminator);
    void finishTransfer(bool ok);
    void sendHexFrame(unsigned char type, const unsigned char flags[4]);
    // PH2-10 send direction helpers.
    void sendBin32Frame(unsigned char type, const unsigned char flags[4]);
    void sendSubpacket(const QByteArray &data, unsigned char terminator);
    void pumpSendData();
    void finishSend(bool ok, const QString &note = QString());
    void reset();
    static unsigned int crc16(const unsigned char *data, int size);
    static unsigned int crc32(const unsigned char *data, int size);

    State m_state = State::Idle;
    bool m_expectSubpacket = false; // a ZFILE/ZDATA frame just went by
    QByteArray m_buffer;      // unparsed stream bytes
    QByteArray m_frameAccum;  // hex-frame accumulator
    int m_hexPos = 0;

    // Receiver transfer context.
    QString m_fileName;
    QString m_filePath;
    QString m_downloadDir;
    qint64 m_fileSize = -1;
    qint64 m_filePos = 0;
    QFile m_file;
    unsigned int m_runningCrc32 = 0xFFFFFFFF;
    int m_subpacketCrcErrors = 0;

    // Send direction context.
    QByteArray m_sendBuffer;   // whole file in memory (v1: no streaming)
    qint64 m_sendPos = 0;
    int m_retryCount = 0;
    QTimer *m_sendTimer = nullptr;
};

} // namespace hssh

#endif // HSSH_TERMINAL_ZMODEMENGINE_H
