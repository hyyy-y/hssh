#ifndef HSSH_APP_DIALOGS_SESSIONLOGVIEWER_H
#define HSSH_APP_DIALOGS_SESSIONLOGVIEWER_H

#include <QDialog>
#include <QTextCursor>

#include <vector>

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QLineEdit;

namespace hssh {

// PH2-15: browse the automatic session logs written by TerminalSession
// (<AppData>/logs/session_<yyyyMMdd_HHmmss_zzz>.log). The files contain the
// RAW terminal byte stream (escape sequences included); the viewer strips
// those for display, filters files by their start timestamp, highlights
// keyword matches and can export the cleaned text.
class SessionLogViewer : public QDialog {
    Q_OBJECT

public:
    explicit SessionLogViewer(QWidget *parent = nullptr);

    // Preselect one log file (e.g. the current tab's own log). No-op when
    // the file is not in the current filter window.
    void selectFile(const QString &filePath);

    // --- pure helpers (unit-tested) ---
    // "session_20260923_141530_123.log" -> QDateTime (invalid on mismatch).
    [[nodiscard]] static QDateTime timestampFromFileName(const QString &fileName);
    // Raw terminal bytes -> readable text: drop escape sequences and stray
    // control characters, normalize CR/CRLF to LF.
    [[nodiscard]] static QString stripAnsi(const QByteArray &raw);

private:
    void refreshFileList();
    void loadSelectedFile();
    void applySearch();
    void gotoNextMatch();
    void exportContent();

    QComboBox *m_rangeCombo = nullptr;
    QListWidget *m_fileList = nullptr;
    QPlainTextEdit *m_content = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_matchLabel = nullptr;

    // std::vector: QTextCursor is not nothrow-destructible, which QList's
    // static_assert rejects.
    std::vector<QTextCursor> m_matches;
    int m_currentMatch = -1;
    QString m_currentPath;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_SESSIONLOGVIEWER_H
