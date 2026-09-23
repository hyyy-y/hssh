#ifndef HSSH_APP_DIALOGS_REMOTESEARCHDIALOG_H
#define HSSH_APP_DIALOGS_REMOTESEARCHDIALOG_H

#include "core/SftpSession.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;

namespace hssh {

class SftpWidget;

// PH3-12: search files on the remote side of an SFTP tab. The tree walk
// runs on the SFTP worker thread (listDirTree, optionally depth-capped);
// the name/size/mtime filters below run locally on the collected entries,
// so ten-thousand-entry trees never block the GUI.
class RemoteSearchDialog : public QDialog {
    Q_OBJECT

public:
    // Filter bundle (unit-tested through filterEntries).
    struct Criteria {
        QString namePattern; // wildcard (*?); empty = match everything.
                             // A pattern containing '/' matches the relative
                             // path, otherwise just the file name.
        int sizeMode = 0;    // 0 = any, 1 = at least, 2 = at most
        qint64 sizeBytes = 0;
        bool timeEnabled = false; // mtime within `days` days
        int days = 7;
        int maxDepth = 0;    // 0 = unlimited
    };

    explicit RemoteSearchDialog(SftpWidget *remote, QWidget *parent = nullptr);

    // Pure local filter over walk results; `nowSecs` anchors the mtime
    // window so tests are deterministic.
    [[nodiscard]] static QList<RemoteFileEntry> filterEntries(
        const QList<RemoteFileEntry> &entries, const Criteria &criteria, qint64 nowSecs);

private:
    void startSearch();
    void cancelSearch();
    void onWalkListed(const QString &path, const QList<RemoteFileEntry> &entries);
    void onWalkProgress(const QString &path, int scanned);
    void onResultActivated(int row, int column);
    Criteria currentCriteria() const;

    SftpWidget *m_remote = nullptr;
    QLineEdit *m_rootEdit = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_sizeModeBox = nullptr;
    QSpinBox *m_sizeSpin = nullptr;
    QComboBox *m_sizeUnitBox = nullptr;
    QCheckBox *m_timeCheck = nullptr;
    QSpinBox *m_daysSpin = nullptr;
    QSpinBox *m_depthSpin = nullptr;
    QPushButton *m_searchButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTableWidget *m_results = nullptr;

    bool m_active = false;     // a walk started from THIS dialog is running
    QString m_activeRoot;      // root of that walk (ignores foreign walks)
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_REMOTESEARCHDIALOG_H
