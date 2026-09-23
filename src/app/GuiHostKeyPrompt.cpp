#include "GuiHostKeyPrompt.h"

#include "utils/Config.h"

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <memory>

namespace hssh {

namespace {

QString policyFromConfig()
{
    return Config::instance().stringValue(QStringLiteral("security/hostKeyPolicy"),
                                          QStringLiteral("accept-new"));
}

// Runs ON the GUI thread (or inline when already there): fingerprint dialog
// with a 60 s auto-reject so a worker thread can never wedge forever.
KeyStore::HostKeyDecision showHostKeyDialog(const KeyStore::HostKeyInfo &info, bool changed)
{
    QMessageBox box;
    box.setWindowTitle(changed ? QStringLiteral("Host Key CHANGED")
                               : QStringLiteral("Unknown Host Key"));
    box.setIcon(changed ? QMessageBox::Warning : QMessageBox::Question);
    box.setText(QStringLiteral("The host key of %1 %2.")
                    .arg(info.host,
                         changed ? QStringLiteral("has CHANGED — possible man-in-the-middle")
                                 : QStringLiteral("is not known yet")));
    box.setInformativeText(QStringLiteral("%1 key\nSHA256: %2\nMD5: %3\n\n"
                                          "Accept and continue?")
                                .arg(info.keyType,
                                     info.fingerprintSha256,
                                     info.fingerprintMd5));
    QPushButton *acceptButton = box.addButton(QStringLiteral("Accept"), QMessageBox::YesRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);

    bool timedOut = false;
    QTimer autoReject;
    autoReject.setSingleShot(true);
    QObject::connect(&autoReject, &QTimer::timeout, &box, [&]() {
        timedOut = true;
        box.reject();
    });
    autoReject.start(60000);
    box.exec();

    if (timedOut || box.clickedButton() != acceptButton) {
        return KeyStore::HostKeyDecision::Reject;
    }
    return KeyStore::HostKeyDecision::Accept;
}

} // namespace

KeyStore::HostKeyDecision decideHostKey(const KeyStore::HostKeyInfo &info, bool changed)
{
    const QString policy = policyFromConfig();
    if (policy == QLatin1String("accept-all")) {
        return KeyStore::HostKeyDecision::Accept;
    }
    if (policy == QLatin1String("ask")) {
        if (QThread::currentThread() == qApp->thread()) {
            return showHostKeyDialog(info, changed);
        }
        // Worker thread: hop to the GUI thread and block until answered.
        auto result = std::make_shared<int>(static_cast<int>(KeyStore::HostKeyDecision::Reject));
        QMetaObject::invokeMethod(
            qApp,
            [info, changed, result]() {
                *result = static_cast<int>(showHostKeyDialog(info, changed));
            },
            Qt::BlockingQueuedConnection);
        return static_cast<KeyStore::HostKeyDecision>(*result);
    }
    // accept-new (default): TOFU for new keys, hard reject on change.
    return changed ? KeyStore::HostKeyDecision::Reject : KeyStore::HostKeyDecision::Accept;
}

} // namespace hssh
