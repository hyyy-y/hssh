#include "ZModemEngine.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QStandardPaths>

namespace hssh {

namespace {
// Calibrated against real lrzsz traffic (see header).
constexpr unsigned char ZDLE = 0x18;
constexpr unsigned char ZPAD = '*';
// Frame types.
constexpr unsigned char ZRQINIT = 0;
constexpr unsigned char ZRINIT = 1;
constexpr unsigned char ZSINIT = 2;
constexpr unsigned char ZACK = 3;
constexpr unsigned char ZFILE = 4;
constexpr unsigned char ZSKIP = 5;
constexpr unsigned char ZNAK = 6;
constexpr unsigned char ZABORT = 7;
constexpr unsigned char ZFIN = 8;
constexpr unsigned char ZRPOS = 9;
constexpr unsigned char ZDATA = 10;
constexpr unsigned char ZEOF = 11;
// Subpacket terminators (after ZDLE).
constexpr unsigned char ZCRCE = 'h'; // end of frame, no response
constexpr unsigned char ZCRCG = 'i'; // more follows
constexpr unsigned char ZCRCW = 'j'; // end, expect response
constexpr unsigned char ZCRCX = 'k'; // end, expect ZACK

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

ZModemEngine::ZModemEngine(QObject *parent)
    : QObject(parent)
{
}

unsigned int ZModemEngine::crc16(const unsigned char *data, int size)
{
    unsigned int crc = 0;
    for (int i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned int>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
    return crc & 0xFFFF;
}

unsigned int ZModemEngine::crc32(const unsigned char *data, int size)
{
    // Standard zip CRC-32 (reflected 0xEDB88320, init/xor 0xFFFFFFFF).
    // Calibrated on real lrzsz traffic (2026-09-20): frame header CRC =
    // crc32(type+f0..f3) little-endian; subpacket CRC = crc32(data +
    // terminator byte, WITHOUT the ZDLE prefix) little-endian.
    unsigned int crc = 0xFFFFFFFF;
    for (int i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void ZModemEngine::reset()
{
    m_state = State::Idle;
    m_expectSubpacket = false;
    m_frameAccum.clear();
    m_hexPos = 0;
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_fileSize = -1;
    m_filePos = 0;
    m_subpacketCrcErrors = 0;
}

void ZModemEngine::sendHexFrame(unsigned char type, const unsigned char flags[4])
{
    // "**\x18B" + hex(type, f0..f3, crc16hi, crc16lo) + CR, LF(0x8A), XON.
    unsigned char body[5] = {type, flags[0], flags[1], flags[2], flags[3]};
    const unsigned int crc = crc16(body, 5);
    unsigned char withCrc[7] = {type, flags[0], flags[1], flags[2], flags[3],
                                static_cast<unsigned char>(crc >> 8),
                                static_cast<unsigned char>(crc & 0xFF)};
    QByteArray frame = "**";
    frame.append(char(ZDLE));
    frame.append('B');
    static const char hexDigits[] = "0123456789abcdef";
    for (int i = 0; i < 7; ++i) {
        frame.append(hexDigits[withCrc[i] >> 4]);
        frame.append(hexDigits[withCrc[i] & 0xF]);
    }
    frame.append('\r');
    frame.append(char(0x8A));
    frame.append(char(0x11));
    emit output(frame);
}

void ZModemEngine::handleFrame(unsigned char type, const unsigned char *flags)
{
    if (type == ZFILE || type == ZDATA) {
        // The frame's data subpacket follows immediately.
        m_expectSubpacket = true;
    }
    if (m_state == State::Done) {
        if (type == ZFIN) {
            emit output("OO");
            reset();
        } else if (type == ZFILE) {
            // Another file follows in the same batch.
            m_state = State::Handshake;
        } else if (type == ZRQINIT) {
            // The sender started over: re-arm the detector flow.
            const unsigned char rinit[4] = {0, 0, 0, 0x23};
            sendHexFrame(ZRINIT, rinit);
            m_state = State::Handshake;
        }
        return;
    }
    switch (m_state) {
    case State::Handshake:
        if (type == ZFILE) {
            // The filename subpacket follows as the next subpacket; handled
            // in handleSubpacket with ZCRCX/W terminator.
            emit status(tr("ZMODEM: sender offers a file..."));
        } else if (type == ZFIN) {
            emit output("OO");
            finishTransfer(false);
        }
        break;
    case State::Data:
        if (type == ZEOF) {
            // flags = file position (P0..P3, little-endian).
            const qint64 pos = static_cast<qint64>(flags[0])
                | (static_cast<qint64>(flags[1]) << 8)
                | (static_cast<qint64>(flags[2]) << 16)
                | (static_cast<qint64>(flags[3]) << 24);
            if (pos == m_filePos) {
                // Complete BEFORE sending ZRINIT: a synchronous ZFIN back
                // from the sender (loopback) must land in the Done state,
                // not re-enter the Data branch and finish a second time.
                finishTransfer(true);
                // Acknowledge with ZRINIT: tells sz this file is complete.
                const unsigned char rinit[4] = {0, 0, 0, 0x23};
                sendHexFrame(ZRINIT, rinit);
            } else {
                const unsigned char rpos[4] = {static_cast<unsigned char>(m_filePos & 0xFF),
                                                static_cast<unsigned char>((m_filePos >> 8) & 0xFF),
                                                static_cast<unsigned char>((m_filePos >> 16) & 0xFF),
                                                static_cast<unsigned char>((m_filePos >> 24) & 0xFF)};
                sendHexFrame(ZRPOS, rpos);
            }
        } else if (type == ZFIN) {
            emit output("OO");
            finishTransfer(true);
        } else if (type == ZDATA) {
            // Position confirmation; subpackets follow.
        } else if (type == ZNAK) {
            const unsigned char rpos[4] = {
                static_cast<unsigned char>(m_filePos & 0xFF),
                static_cast<unsigned char>((m_filePos >> 8) & 0xFF),
                static_cast<unsigned char>((m_filePos >> 16) & 0xFF),
                static_cast<unsigned char>((m_filePos >> 24) & 0xFF)};
            sendHexFrame(ZRPOS, rpos);
        }
        break;
    case State::SendWaitRinit:
        if (type == ZRINIT) {
            // The remote rz is alive: offer the file. NOTE: the state must
            // advance BEFORE emitting — a synchronous reply re-enters
            // handleFrame and must not run this branch a second time.
            if (m_sendTimer) {
                m_sendTimer->stop();
            }
            m_retryCount = 0;
            m_state = State::SendWaitPos;
            {
                const QString info = QStringLiteral("%1 %2 %3")
                                         .arg(m_fileSize)
                                         .arg(QDateTime::currentSecsSinceEpoch())
                                         .arg(0644, 8, 16, QLatin1Char('0'));
                QByteArray meta = m_fileName.toUtf8();
                meta.append('\0');
                meta.append(info.toLatin1());
                sendBin32Frame(ZFILE, nullptr);
                sendSubpacket(meta, ZCRCX); // ZCRCX matches the captured sz stream
            }
        } else if (type == ZFIN) {
            emit output("OO");
            finishSend(false, tr("ZMODEM: remote refused the transfer"));
        }
        break;
    case State::SendWaitPos: {
        const qint64 pos = static_cast<qint64>(flags[0])
            | (static_cast<qint64>(flags[1]) << 8)
            | (static_cast<qint64>(flags[2]) << 16)
            | (static_cast<qint64>(flags[3]) << 24);
        if (type == ZRPOS) {
            m_sendPos = pos; // rz normally requests 0 (no resume)
            if (m_sendPos > m_sendBuffer.size()) {
                m_sendPos = 0;
            }
            const unsigned char zero[4] = {static_cast<unsigned char>(m_sendPos & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 8) & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 16) & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 24) & 0xFF)};
            m_state = State::SendData; // advance before emitting (reentrancy)
            sendBin32Frame(ZDATA, zero);
            pumpSendData();
        } else if (type == ZSKIP) {
            finishSend(false, tr("ZMODEM: remote skipped the file"));
        } else if (type == ZFIN) {
            emit output("OO");
            finishSend(false, tr("ZMODEM: remote refused the transfer"));
        } else if (type == ZNAK) {
            // rz rejected the ZFILE frame: resend it (bounded).
            if (++m_retryCount > 3) {
                finishSend(false, tr("ZMODEM: remote kept rejecting the file offer"));
                return;
            }
            const QString info = QStringLiteral("%1 %2 %3")
                                     .arg(m_sendBuffer.size())
                                     .arg(QDateTime::currentSecsSinceEpoch())
                                     .arg(0644, 8, 16, QLatin1Char('0'));
            QByteArray meta = m_fileName.toUtf8();
            meta.append('\0');
            meta.append(info.toLatin1());
            sendBin32Frame(ZFILE, nullptr);
            sendSubpacket(meta, ZCRCX);
        }
        break;
    }
    case State::SendData:
        if (type == ZRPOS) {
            // rz wants a resend from an offset (windowing): rewind.
            const qint64 pos = static_cast<qint64>(flags[0])
                | (static_cast<qint64>(flags[1]) << 8)
                | (static_cast<qint64>(flags[2]) << 16)
                | (static_cast<qint64>(flags[3]) << 24);
            m_sendPos = qBound<qint64>(0, pos, m_sendBuffer.size());
            const unsigned char zero[4] = {static_cast<unsigned char>(m_sendPos & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 8) & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 16) & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 24) & 0xFF)};
            m_state = State::SendData; // advance before emitting (reentrancy)
            sendBin32Frame(ZDATA, zero);
            pumpSendData();
        } else if (type == ZACK) {
            // Progress confirmation; keep streaming.
        } else if (type == ZFIN) {
            emit output("OO");
            finishSend(true);
        }
        break;
    case State::SendEof:
        if (type == ZFIN) {
            emit output("OO");
            finishSend(true);
        } else if (type == ZRINIT) {
            // rz acknowledged the file: close with ZFIN.
            const unsigned char fin[4] = {0, 0, 0, 0};
            sendHexFrame(ZFIN, fin);
            emit output("OO");
            finishSend(true);
        } else if (type == ZNAK || type == ZRPOS) {
            // rz lost the tail: resend from the requested offset.
            const qint64 pos = (type == ZRPOS)
                ? (static_cast<qint64>(flags[0]) | (static_cast<qint64>(flags[1]) << 8)
                   | (static_cast<qint64>(flags[2]) << 16) | (static_cast<qint64>(flags[3]) << 24))
                : m_sendBuffer.size();
            m_sendPos = qBound<qint64>(0, pos, m_sendBuffer.size());
            const unsigned char zero[4] = {static_cast<unsigned char>(m_sendPos & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 8) & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 16) & 0xFF),
                                           static_cast<unsigned char>((m_sendPos >> 24) & 0xFF)};
            m_state = State::SendData; // advance before emitting (reentrancy)
            sendBin32Frame(ZDATA, zero);
            pumpSendData();
        }
        break;
    default:
        break;
    }
}

void ZModemEngine::sendBin32Frame(unsigned char type, const unsigned char flags[4])
{
    // "*\x18C" + type + f0..f3 + crc32(type..f3), each byte ZDLE-escaped.
    static const unsigned char zero[4] = {0, 0, 0, 0};
    if (!flags) {
        flags = zero;
    }
    unsigned char body[5] = {type, flags[0], flags[1], flags[2], flags[3]};
    const unsigned int crc = crc32(body, 5);
    QByteArray frame;
    frame.append(char(ZPAD));
    frame.append(char(ZDLE));
    frame.append('C');
    const auto escaped = [&frame](unsigned char c) {
        switch (c) {
        case ZDLE:
        case 0x10:
        case 0x11:
        case 0x13:
        case 0x8d:
        case 0x90:
        case 0x91:
        case 0x93:
            frame.append(char(ZDLE));
            frame.append(char(c ^ 0x40));
            break;
        default:
            frame.append(char(c));
            break;
        }
    };
    escaped(type);
    escaped(flags[0]);
    escaped(flags[1]);
    escaped(flags[2]);
    escaped(flags[3]);
    escaped(static_cast<unsigned char>(crc & 0xFF));
    escaped(static_cast<unsigned char>((crc >> 8) & 0xFF));
    escaped(static_cast<unsigned char>((crc >> 16) & 0xFF));
    escaped(static_cast<unsigned char>((crc >> 24) & 0xFF));
    emit output(frame);
}

void ZModemEngine::sendSubpacket(const QByteArray &data, unsigned char terminator)
{
    // data (escaped) + ZDLE + terminator + crc32(data + terminator) LE
    // (escaped). The CRC covers the terminator byte but NOT the ZDLE
    // prefix — calibrated on real lrzsz traffic (2026-09-20).
    QByteArray covered = data;
    covered.append(char(terminator));
    const unsigned int crc = crc32(reinterpret_cast<const unsigned char *>(covered.constData()),
                                   static_cast<int>(covered.size()));
    QByteArray frame;
    const auto escaped = [&frame](unsigned char c) {
        switch (c) {
        case ZDLE:
        case 0x10:
        case 0x11:
        case 0x13:
        case 0x8d:
        case 0x90:
        case 0x91:
        case 0x93:
            frame.append(char(ZDLE));
            frame.append(char(c ^ 0x40));
            break;
        default:
            frame.append(char(c));
            break;
        }
    };
    for (char c : data) {
        escaped(static_cast<unsigned char>(c));
    }
    frame.append(char(ZDLE));
    frame.append(char(terminator));
    escaped(static_cast<unsigned char>(crc & 0xFF));
    escaped(static_cast<unsigned char>((crc >> 8) & 0xFF));
    escaped(static_cast<unsigned char>((crc >> 16) & 0xFF));
    escaped(static_cast<unsigned char>((crc >> 24) & 0xFF));
    if (terminator == ZCRCW || terminator == ZCRCX) {
        frame.append(char(0x11)); // XON: lrzsz expects it after wait-type ends
    }
    emit output(frame);
}

void ZModemEngine::pumpSendData()
{
    // Stream 1024-byte ZCRCG subpackets; finish with ZCRCE, then ZEOF.
    constexpr int chunk = 1024;
    while (m_sendPos < m_sendBuffer.size()) {
        const int len = qMin<qint64>(chunk, m_sendBuffer.size() - m_sendPos);
        const QByteArray piece = m_sendBuffer.mid(static_cast<int>(m_sendPos), len);
        const unsigned char term = (m_sendPos + len >= m_sendBuffer.size()) ? ZCRCE : ZCRCG;
        sendSubpacket(piece, term);
        m_sendPos += len;
        if (m_sendPos % (64 * 1024) < chunk) {
            emit status(tr("ZMODEM: sending %1 ... %2/%3 bytes")
                            .arg(m_fileName).arg(m_sendPos).arg(m_sendBuffer.size()));
        }
    }
    // ZEOF with the final position. Advance the state before emitting —
    // the reply re-enters this engine synchronously.
    const unsigned char fin[4] = {static_cast<unsigned char>(m_sendPos & 0xFF),
                                  static_cast<unsigned char>((m_sendPos >> 8) & 0xFF),
                                  static_cast<unsigned char>((m_sendPos >> 16) & 0xFF),
                                  static_cast<unsigned char>((m_sendPos >> 24) & 0xFF)};
    m_state = State::SendEof;
    sendHexFrame(ZEOF, fin);
    emit status(tr("ZMODEM: sent %1 (%2 bytes), waiting for confirmation")
                    .arg(m_fileName).arg(m_sendBuffer.size()));
}

void ZModemEngine::finishSend(bool ok, const QString &note)
{
    if (m_sendTimer) {
        m_sendTimer->stop();
        m_sendTimer->deleteLater();
        m_sendTimer = nullptr;
    }
    m_sendBuffer.clear();
    m_sendPos = 0;
    m_retryCount = 0;
    emit status(note.isEmpty()
                    ? (ok ? tr("ZMODEM: send complete: %1").arg(m_fileName)
                          : tr("ZMODEM: send cancelled"))
                    : note);
    emit finished(ok, m_filePath);
    m_filePath.clear();
    m_fileName.clear();
    reset();
}

bool ZModemEngine::startSend(const QString &localPath)
{
    if (isActive()) {
        return false;
    }
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    m_sendBuffer = file.readAll();
    file.close();
    if (m_sendBuffer.isEmpty()) {
        return false;
    }
    m_fileName = QFileInfo(localPath).fileName();
    m_filePath = localPath;
    m_fileSize = m_sendBuffer.size();
    m_sendPos = 0;
    m_retryCount = 0;
    m_state = State::SendWaitRinit;

    // Wake the remote receiver; its ZRINIT (hex type 01) is not mistaken
    // for an incoming transfer by the Idle detector.
    emit output("rz\r");
    const unsigned char zero[4] = {0, 0, 0, 0};
    sendHexFrame(ZRQINIT, zero);
    emit status(tr("ZMODEM: sending %1 (%2 bytes), waiting for rz...")
                    .arg(m_fileName).arg(m_sendBuffer.size()));

    // Retry the ZRQINIT until the remote answers (rz may still be
    // starting up).
    m_sendTimer = new QTimer(this);
    m_sendTimer->setInterval(3000);
    connect(m_sendTimer, &QTimer::timeout, this, [this]() {
        if (m_state != State::SendWaitRinit) {
            return;
        }
        if (++m_retryCount > 5) {
            finishSend(false, tr("ZMODEM: no rz answered (is lrzsz installed?)"));
            return;
        }
        const unsigned char zero[4] = {0, 0, 0, 0};
        sendHexFrame(ZRQINIT, zero);
    });
    m_sendTimer->start();
    return true;
}

void ZModemEngine::handleSubpacket(const QByteArray &payload, unsigned char terminator)
{
    if (m_state == State::Handshake) {
        // ZFILE payload: "name\0size mtime mode serial filesleft [remainder]".
        const int nul = payload.indexOf('\0');
        if (nul <= 0) {
            return;
        }
        m_fileName = QString::fromUtf8(payload.left(nul));
        const QString rest = QString::fromLatin1(payload.mid(nul + 1));
        const QStringList fields = rest.simplified().split(' ');
        m_fileSize = fields.isEmpty() ? -1 : fields.first().toLongLong();

        const QString dir = m_downloadDir.isEmpty()
                                ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                                : m_downloadDir;
        QString path = dir + QDir::separator() + m_fileName;
        int dup = 1;
        while (QFile::exists(path)) {
            path = dir + QDir::separator() + m_fileName
                + QStringLiteral(".%1").arg(dup++);
        }
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly)) {
            emit status(tr("ZMODEM: cannot write %1").arg(path));
            const unsigned char fin[4] = {0, 0, 0, 0};
            sendHexFrame(ZFIN, fin);
            reset();
            return;
        }
        m_filePath = path;
        m_filePos = 0;
        m_runningCrc32 = 0xFFFFFFFF;
        m_state = State::Data;
        emit status(tr("ZMODEM: receiving %1 (%2 bytes)").arg(m_fileName).arg(m_fileSize));

        // Tell sz to start from offset 0.
        const unsigned char rpos[4] = {0, 0, 0, 0};
        sendHexFrame(ZRPOS, rpos);
        return;
    }
    if (m_state == State::Data) {
        if (!payload.isEmpty()) {
            m_file.write(payload);
            m_filePos += payload.size();
            for (char c : payload) {
                m_runningCrc32 ^= static_cast<unsigned char>(c);
                for (int b = 0; b < 8; ++b) {
                    m_runningCrc32 = (m_runningCrc32 & 1)
                        ? ((m_runningCrc32 >> 1) ^ 0xEDB88320u)
                        : (m_runningCrc32 >> 1);
                }
            }
        }
        if (m_fileSize > 0 && (m_filePos % (512 * 1024)) < static_cast<qint64>(payload.size())) {
            emit status(tr("ZMODEM: %1 ... %2/%3 bytes")
                            .arg(m_fileName).arg(m_filePos).arg(m_fileSize));
        }
        if (terminator == ZCRCW) {
            // Sender waits for a response: keep it flowing. The position
            // must be the FULL little-endian offset — a low-byte-only ack
            // once made sz rewind/resend past the 64K mark and scramble
            // every large file (2026-09-20).
            const unsigned char ackPos[4] = {
                static_cast<unsigned char>(m_filePos & 0xFF),
                static_cast<unsigned char>((m_filePos >> 8) & 0xFF),
                static_cast<unsigned char>((m_filePos >> 16) & 0xFF),
                static_cast<unsigned char>((m_filePos >> 24) & 0xFF)};
            sendHexFrame(ZACK, ackPos);
        }
    }
}

void ZModemEngine::finishTransfer(bool ok)
{
    if (m_file.isOpen()) {
        m_file.close();
    }
    if (!m_filePath.isEmpty()) {
        emit status(ok ? tr("ZMODEM: saved %1 (%2 bytes)").arg(m_filePath).arg(m_filePos)
                       : tr("ZMODEM: transfer ended (%1 bytes received)").arg(m_filePos));
    }
    emit finished(ok, m_filePath);
    m_filePath.clear();
    m_expectSubpacket = false;
    m_frameAccum.clear();
    if (ok) {
        // Stay in Done: the sender still owes us a ZFIN (its hex frame must
        // NOT be mistaken for a new incoming transfer — that re-triggered
        // the detector once and swallowed the terminal, 2026-09-20). Some
        // senders exit without it: fall back to Idle after 5 s instead of
        // swallowing the terminal forever.
        m_state = State::Done;
        QTimer::singleShot(5000, this, [this]() {
            if (m_state == State::Done) {
                reset();
            }
        });
    } else {
        m_state = State::Idle;
    }
}

void ZModemEngine::abort()
{
    if (m_state == State::Idle) {
        return;
    }
    // CAN x8, backspace-space-backspace: the classic zmodem cancel.
    QByteArray cancel(8, char(0x18));
    cancel += QByteArray("\x08 \x08");
    emit output(cancel);
    finishTransfer(false);
}

QByteArray ZModemEngine::feed(const QByteArray &data)
{
    m_buffer.append(data);
    if (m_state == State::Idle) {
        // Look for the ZRQINIT hex frame start: "**\x18B". NOTE: the C
        // string is split — hex escapes are greedy and "\x18B" would parse
        // as 0x18B, not 0x18 + 'B'.
        static const QByteArray signature = "**\x18" "B";
        int idx = m_buffer.indexOf(signature);
        while (idx >= 0) {
            // Guard against the signature appearing in ordinary output:
            // require 14 following hex chars + CR.
            bool plausible = m_buffer.size() >= idx + 4 + 15;
            if (plausible) {
                for (int i = idx + 4; i < idx + 18 && plausible; ++i) {
                    plausible = hexValue(m_buffer.at(i)) >= 0;
                }
            }
            // Only a ZRQINIT (type hex "00") starts a receive flow; a
            // stray ZRINIT/ZFIN elsewhere must not capture the terminal.
            if (plausible) {
                plausible = m_buffer.at(idx + 4) == '0' && m_buffer.at(idx + 5) == '0';
            }
            if (plausible) {
                m_state = State::Handshake;
                emit status(tr("ZMODEM: incoming transfer detected"));
                const unsigned char rinit[4] = {0, 0, 0, 0x23};
                sendHexFrame(ZRINIT, rinit);
                QByteArray passthrough = m_buffer.left(idx);
                m_buffer.remove(0, idx);
                return passthrough;
            }
            idx = m_buffer.indexOf(signature, idx + 1);
        }
        // Nothing confirmed: display everything except a tail that could
        // still grow into a signature (longest buffer suffix that is a
        // proper signature prefix). A fixed 8-byte holdback used to trap
        // the final bytes of a session (exec end markers!) until more data
        // that never came (2026-09-20).
        int keep = 0;
        const int maxKeep = qMin<int>(m_buffer.size(), signature.size() - 1);
        for (int k = maxKeep; k > 0; --k) {
            if (m_buffer.right(k) == signature.left(k)) {
                keep = k;
                break;
            }
        }
        // A signature occurrence whose type could not be judged yet (not
        // enough bytes arrived) still holds back its region; one that WAS
        // judged and rejected (e.g. the sender's final ZFIN, type 08) must
        // NOT — everything behind it is ordinary output again.
        for (int pending = m_buffer.indexOf(signature); pending >= 0;
             pending = m_buffer.indexOf(signature, pending + 1)) {
            if (m_buffer.size() < pending + 4 + 15) {
                keep = qMax(keep, m_buffer.size() - pending);
            }
        }
        if (keep < m_buffer.size()) {
            const QByteArray head = m_buffer.left(m_buffer.size() - keep);
            m_buffer.remove(0, m_buffer.size() - keep);
            return head;
        }
        return QByteArray();
    }

    // Active protocol: parse frames from m_buffer. A subpacket only ever
    // FOLLOWS a ZFILE or ZDATA frame (m_expectSubpacket); any other noise
    // between frames (shell echoes, "rz waiting" banners) is dropped one
    // byte at a time — treating it as data once wrote 12 bytes of "rz"-echo
    // into the file (2026-09-20 m_filePos=37 bug).
    while (!m_buffer.isEmpty()) {
        // A pending subpacket owns the stream: its DATA bytes may contain
        // 0x2a (ZPAD) which must NOT be mistaken for a frame start — that
        // swallowed one '*' per occurrence and desynced large files (first
        // divergence at byte 562177, 2026-09-20). Subpacket parsing goes
        // FIRST, frame parsing only after the subpacket sequence closed.
        if (m_expectSubpacket) {
            // Data subpacket: unescape until ZDLE + terminator, then 4 CRC
            // bytes (escaped). Collect payload.
            QByteArray payload;
            unsigned char terminator = 0;
            unsigned char crc[4] = {0, 0, 0, 0};
            int pos = 0;
            bool complete = false;
            while (pos < m_buffer.size()) {
                unsigned char c = static_cast<unsigned char>(m_buffer.at(pos));
                ++pos;
                bool escaped = false;
                if (c == ZDLE) {
                    if (pos >= m_buffer.size()) {
                        --pos; // escape pending
                        break;
                    }
                    unsigned char e = static_cast<unsigned char>(m_buffer.at(pos));
                    ++pos;
                    escaped = true;
                    if (e == ZCRCE || e == ZCRCG || e == ZCRCW || e == ZCRCX) {
                        terminator = e;
                        // 4 CRC bytes follow (each possibly escaped).
                        int got = 0;
                        while (got < 4 && pos < m_buffer.size()) {
                            unsigned char cc = static_cast<unsigned char>(m_buffer.at(pos));
                            ++pos;
                            if (cc == ZDLE) {
                                if (pos >= m_buffer.size()) {
                                    --pos;
                                    break;
                                }
                                unsigned char ee = static_cast<unsigned char>(m_buffer.at(pos));
                                ++pos;
                                cc = ((ee & 0x60) == 0x40) ? (ee & ~0x40) : ee;
                            }
                            crc[got++] = cc;
                        }
                        if (got == 4) {
                            complete = true;
                        }
                        break;
                    }
                    c = ((e & 0x60) == 0x40) ? (e & ~0x40) : e;
                }
                // Unescaped XON/XOFF are flow control the sender wedged
                // into the stream — zmodem receivers discard them. Escaped
                // ones (ZDLE 0x51/0x53) are DATA and must survive.
                if (!escaped && (c == 0x11 || c == 0x13)) {
                    continue;
                }
                payload.append(char(c));
            }
            if (!complete) {
                break; // wait for more
            }
            // Verify the subpacket CRC like a real receiver does (rz discards
            // the data and answers ZNAK — a sender-side encoding bug shows up
            // here, not as a silently corrupted file).
            QByteArray covered = payload;
            covered.append(char(terminator));
            const unsigned int computedCrc =
                crc32(reinterpret_cast<const unsigned char *>(covered.constData()),
                      static_cast<int>(covered.size()));
            const unsigned int receivedCrc = static_cast<unsigned int>(crc[0])
                | (static_cast<unsigned int>(crc[1]) << 8)
                | (static_cast<unsigned int>(crc[2]) << 16)
                | (static_cast<unsigned int>(crc[3]) << 24);
            m_buffer.remove(0, pos);
            if (computedCrc != receivedCrc) {
                emit status(tr("ZMODEM: subpacket CRC error (got %1 of %2 bytes), requesting resend")
                                .arg(payload.size()).arg(m_fileName));
                const unsigned char nak[4] = {0, 0, 0, 0};
                sendHexFrame(ZNAK, nak);
                if (++m_subpacketCrcErrors > 8) {
                    emit status(tr("ZMODEM: too many CRC errors, aborting"));
                    finishTransfer(false);
                }
                continue;
            }
            m_subpacketCrcErrors = 0;
            // 'i' (ZCRCG) subpackets stream back-to-back; h/j/k close the
            // subpacket sequence.
            m_expectSubpacket = (terminator == ZCRCG);
            handleSubpacket(payload, terminator);
            continue;
        }

        const unsigned char first = static_cast<unsigned char>(m_buffer.at(0));
        if (first == ZPAD) {
            // Frame: ZPAD* ZDLE then 'B' (hex) or 'C' (bin32). The ZDLE must
            // sit within the first three bytes — otherwise this is noise
            // that merely looks like a frame start.
            int zdle = -1;
            for (int i = 0; i < qMin(3, m_buffer.size()); ++i) {
                if (static_cast<unsigned char>(m_buffer.at(i)) == ZDLE) {
                    zdle = i;
                    break;
                }
            }
            if (zdle < 0) {
                m_buffer.remove(0, 1); // lone ZPAD noise
                continue;
            }
            if (m_buffer.size() < zdle + 2) {
                break; // need the format byte
            }
            const char fmt = m_buffer.at(zdle + 1);
            if (fmt == 'B') {
                // Hex frame: 14 hex chars + CR (+0x8A +XON).
                if (m_buffer.size() < zdle + 2 + 14 + 1) {
                    break;
                }
                unsigned char decoded[7];
                bool ok = true;
                for (int i = 0; i < 7; ++i) {
                    const int hi = hexValue(m_buffer.at(zdle + 2 + i * 2));
                    const int lo = hexValue(m_buffer.at(zdle + 3 + i * 2));
                    if (hi < 0 || lo < 0) {
                        ok = false;
                        break;
                    }
                    decoded[i] = static_cast<unsigned char>((hi << 4) | lo);
                }
                if (!ok) {
                    m_buffer.remove(0, zdle + 1);
                    continue;
                }
                unsigned char body[5] = {decoded[0], decoded[1], decoded[2],
                                         decoded[3], decoded[4]};
                const unsigned int crc = (decoded[5] << 8) | decoded[6];
                if (crc16(body, 5) != crc) {
                    m_buffer.remove(0, zdle + 2);
                    continue;
                }
                // Consume the frame BEFORE dispatching: a synchronous reply
                // (loopback/test setups) re-enters feed() and would parse
                // this frame a second time from the buffer head.
                int end = zdle + 2 + 14;
                while (end < m_buffer.size()
                       && (m_buffer.at(end) == '\r' || m_buffer.at(end) == char(0x8A)
                           || m_buffer.at(end) == char(0x11)
                           || m_buffer.at(end) == '\n')) {
                    ++end;
                }
                m_buffer.remove(0, end);
                handleFrame(decoded[0], decoded + 1);
                continue;
            }
            if (fmt == 'C') {
                // Bin32 frame: type + 4 flags + 4 crc bytes (ZDLE-escaped).
                // Decode escaped stream until we have 9 values.
                unsigned char decoded[9];
                int got = 0;
                int pos = zdle + 2;
                int consumedTo = pos;
                bool ok = true;
                while (got < 9 && pos < m_buffer.size()) {
                    unsigned char c = static_cast<unsigned char>(m_buffer.at(pos));
                    ++pos;
                    bool escaped = false;
                    if (c == ZDLE) {
                        if (pos >= m_buffer.size()) {
                            ok = false; // escape split; wait for more
                            break;
                        }
                        unsigned char e = static_cast<unsigned char>(m_buffer.at(pos));
                        ++pos;
                        escaped = true;
                        if ((e & 0x60) == 0x40) {
                            c = e & ~0x40;
                        } else {
                            c = e; // raw (e.g. ZDLE ZDLE -> ZDLE handled below)
                        }
                    }
                    // Unescaped XON/XOFF wedged into the frame are flow
                    // control (zmodem receivers discard them); escaped ones
                    // are DATA.
                    if (!escaped && (c == 0x11 || c == 0x13)) {
                        continue;
                    }
                    decoded[got++] = c;
                    consumedTo = pos;
                }
                if (!ok || got < 9) {
                    break; // wait for more bytes
                }
                // Verify the frame CRC like a real receiver (rz answers ZNAK
                // on a bad header and never acts on the frame).
                const unsigned char body[5] = {decoded[0], decoded[1], decoded[2],
                                               decoded[3], decoded[4]};
                const unsigned int frameCrc = crc32(body, 5);
                const unsigned int recvCrc = static_cast<unsigned int>(decoded[5])
                    | (static_cast<unsigned int>(decoded[6]) << 8)
                    | (static_cast<unsigned int>(decoded[7]) << 16)
                    | (static_cast<unsigned int>(decoded[8]) << 24);
                if (frameCrc != recvCrc) {
                    emit status(tr("ZMODEM: frame CRC error, requesting resend"));
                    m_buffer.remove(0, consumedTo);
                    const unsigned char nak[4] = {0, 0, 0, 0};
                    sendHexFrame(ZNAK, nak);
                    continue;
                }
                // Consume before dispatch (see the hex-frame branch).
                m_buffer.remove(0, consumedTo);
                handleFrame(decoded[0], decoded + 1);
                continue;
            }
            // Unknown format byte: skip this ZDLE.
            m_buffer.remove(0, zdle + 1);
            continue;
        }
        // Noise between frames: drop one byte.
        m_buffer.remove(0, 1);
    }
    return QByteArray();
}

} // namespace hssh