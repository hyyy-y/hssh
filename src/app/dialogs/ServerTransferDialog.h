#ifndef HSSH_APP_DIALOGS_SERVERTRANSFERDIALOG_H
#define HSSH_APP_DIALOGS_SERVERTRANSFERDIALOG_H

#include "core/SessionConfig.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace hssh {

class SftpSession;

// PH3-13: copy one file between two remote hosts. Two independent SftpSession
// workers (own thread + connection each) run the two hops in sequence:
//   source host --(verified download)--> local temp --> (verified upload)--> target host
// Both hops carry the full SftpSession safety net (.part atomicity, md5
// verify, fresh-connection retry), so the end-to-end content equality the
// old "in-memory pipe" sketch wanted falls out of the verified hops — a
// streaming pipe would have had to rebuild all of that from scratch.
class ServerTransferDialog : public QDialog {
    Q_OBJECT

public:
    // `sessions` should be the saved SSH sessions (the dialog filters to
    // usable entries itself).
    explicit ServerTransferDialog(const QList<SessionConfig> &sessions, QWidget *parent = nullptr);
    // `openTabEndpoints` carries the REAL endpoints of already-open SSH
    // tabs: (session name as saved — empty for ad-hoc —, "user@host:port").
    // The transfer connects with the CURRENT saved config; when a same-name
    // tab is open on a different endpoint (the stale-session scenario), the
    // dialog warns before connecting.
    ServerTransferDialog(const QList<SessionConfig> &sessions,
                         const QList<QPair<QString, QString>> &openTabEndpoints,
                         QWidget *parent = nullptr);
    ~ServerTransferDialog() override;

    // Empty when the selected session agrees with every open same-name tab
    // (or no such tab is open); otherwise a human-readable warning. Pure.
    [[nodiscard]] static QString endpointWarning(
        const SessionConfig &selected,
        const QList<QPair<QString, QString>> &openTabEndpoints);

protected:
    void reject() override; // Cancel/Close during a run cancels the transfer

private:
    enum class Phase { Idle, Downloading, Uploading, Done };

    void onStart();
    void onCancel();
    void cleanupRun(bool removeTemp);
    void startUploadPhase();
    void finish(const QString &message, bool ok);

    void onSourceProgress(const QString &path, qint64 done, qint64 total);
    void onSourceFinished(const QString &path, bool ok, const QString &message);
    void onTargetProgress(const QString &path, qint64 done, qint64 total);
    void onTargetFinished(const QString &path, bool ok, const QString &message);
    void onWorkerError(const QString &message);

    QComboBox *m_sourceBox = nullptr;
    QComboBox *m_targetBox = nullptr;
    QLineEdit *m_sourcePath = nullptr;
    QLineEdit *m_targetPath = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_phaseLabel = nullptr;

    QList<SessionConfig> m_sessions; // index-aligned with the combo boxes
    SftpSession *m_sourceWorker = nullptr; // parentless: they moveToThread
    SftpSession *m_targetWorker = nullptr;
    int m_sourceTransferId = 0; // TransferRegistry ids (0 = not open)
    int m_targetTransferId = 0;
    Phase m_phase = Phase::Idle;
    bool m_cancelled = false;
    QString m_tempFile;
    QString m_sourceRemote;
    QString m_targetRemote;
    QList<QPair<QString, QString>> m_openTabEndpoints;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_SERVERTRANSFERDIALOG_H
