#include "KeyManagerDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hssh {

namespace {

// Maximum import retries with a passphrase prompt (empty passphrase first).
constexpr int kMaxPassphraseAttempts = 3;

QString typeLabel(const KeyStore::KeyInfo &info)
{
    if (!info.encrypted) {
        return info.type.isEmpty() ? QStringLiteral("?") : info.type;
    }
    return KeyManagerDialog::tr("encrypted");
}

} // namespace

KeyManagerDialog::KeyManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Key Manager"));
    resize(760, 380);

    auto *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("SHA256 Fingerprint")});
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &KeyManagerDialog::exportSelected);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &KeyManagerDialog::updateButtonStates);
    layout->addWidget(m_table);

    auto *buttons = new QHBoxLayout();
    auto *generateButton = new QPushButton(tr("&Generate..."), this);
    auto *importButton = new QPushButton(tr("&Import..."), this);
    m_exportButton = new QPushButton(tr("&Export Public Key"), this);
    m_deleteButton = new QPushButton(tr("&Delete"), this);
    buttons->addWidget(generateButton);
    buttons->addWidget(importButton);
    buttons->addWidget(m_exportButton);
    buttons->addWidget(m_deleteButton);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    // PH2-12: known hosts (first-seen servers; remove = re-confirm next time).
    auto *knownHostsLabel = new QLabel(tr("Known Hosts"), this);
    layout->addWidget(knownHostsLabel);
    m_knownHostsTable = new QTableWidget(this);
    m_knownHostsTable->setColumnCount(3);
    m_knownHostsTable->setHorizontalHeaderLabels(
        {tr("Host"), tr("Key Type"), tr("SHA256 Fingerprint")});
    m_knownHostsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_knownHostsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_knownHostsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_knownHostsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_knownHostsTable->verticalHeader()->hide();
    m_knownHostsTable->setMaximumHeight(160);
    layout->addWidget(m_knownHostsTable);
    m_removeKnownHostButton = new QPushButton(tr("Remove &Host"), this);
    auto *knownHostsButtons = new QHBoxLayout();
    knownHostsButtons->addWidget(m_removeKnownHostButton);
    knownHostsButtons->addStretch(1);
    layout->addLayout(knownHostsButtons);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    connect(generateButton, &QPushButton::clicked, this, &KeyManagerDialog::generate);
    connect(importButton, &QPushButton::clicked, this, &KeyManagerDialog::importKey);
    connect(m_exportButton, &QPushButton::clicked, this, &KeyManagerDialog::exportSelected);
    connect(m_deleteButton, &QPushButton::clicked, this, &KeyManagerDialog::deleteSelected);
    connect(m_removeKnownHostButton, &QPushButton::clicked, this,
            &KeyManagerDialog::removeSelectedKnownHost);

    refresh();
    refreshKnownHosts();
}

void KeyManagerDialog::refresh()
{
    m_keys = KeyStore::listKeys();
    m_table->setRowCount(m_keys.size());
    for (int i = 0; i < m_keys.size(); ++i) {
        const KeyStore::KeyInfo &info = m_keys.at(i);
        m_table->setItem(i, 0, new QTableWidgetItem(info.fileName));
        m_table->setItem(i, 1, new QTableWidgetItem(typeLabel(info)));
        m_table->setItem(i, 2, new QTableWidgetItem(
                                 info.encrypted ? tr("unlock to show") : info.fingerprintSha256));
        m_table->item(i, 0)->setToolTip(info.filePath);
    }
    m_table->sortByColumn(0, Qt::AscendingOrder);
    updateButtonStates();
}

int KeyManagerDialog::currentRow() const
{
    const QModelIndex index = m_table->currentIndex();
    return index.isValid() ? index.row() : -1;
}

void KeyManagerDialog::updateButtonStates()
{
    const bool selected = currentRow() >= 0;
    m_exportButton->setEnabled(selected);
    m_deleteButton->setEnabled(selected);
}

void KeyManagerDialog::generate()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Generate Key"));
    auto *layout = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setText(QStringLiteral("id_ed25519"));
    layout->addRow(tr("Name:"), nameEdit);

    auto *typeBox = new QComboBox(&dialog);
    typeBox->addItem(tr("Ed25519 (recommended)"), static_cast<int>(KeyStore::KeyType::Ed25519));
    layout->addRow(tr("Type:"), typeBox);

    auto *passEdit = new QLineEdit(&dialog);
    passEdit->setEchoMode(QLineEdit::Password);
    layout->addRow(tr("Passphrase (optional):"), passEdit);
    auto *passConfirmEdit = new QLineEdit(&dialog);
    passConfirmEdit->setEchoMode(QLineEdit::Password);
    layout->addRow(tr("Confirm passphrase:"), passConfirmEdit);

    auto *note = new QLabel(
        tr("Keys are stored in the hssh key store. Use \"Export Public Key\" to copy the "
           "authorized_keys line for the remote host.\n\n"
           "Only Ed25519 can be generated here (crypto backend limitation); RSA/ECDSA keys "
           "can be imported via \"Import...\"."), &dialog);
    note->setWordWrap(true);
    layout->addRow(note);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(box);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (passEdit->text() != passConfirmEdit->text()) {
        QMessageBox::warning(this, tr("Generate Key"), tr("Passphrases do not match."));
        return;
    }

    const auto type = static_cast<KeyStore::KeyType>(typeBox->currentData().toInt());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString path;
    QString error;
    const bool ok = KeyStore::generateKey(type, nameEdit->text(), passEdit->text(), &path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Generate Key"),
                             error.isEmpty() ? tr("Key generation failed.") : error);
        return;
    }
    refresh();
    QMessageBox::information(this, tr("Generate Key"),
                             tr("Key generated:\n%1").arg(QDir::toNativeSeparators(path)));
}

void KeyManagerDialog::importKey()
{
    const QString source = QFileDialog::getOpenFileName(
        this, tr("Import Private Key"), QString(),
        tr("Private key files (id_* *.pem *.key);;All files (*)"));
    if (source.isEmpty()) {
        return;
    }
    QString passphrase;
    for (int attempt = 0; attempt < kMaxPassphraseAttempts; ++attempt) {
        QString path;
        QString error;
        if (KeyStore::importKeyFile(source, passphrase, &path, &error)) {
            refresh();
            QMessageBox::information(this, tr("Import Key"),
                                     tr("Key imported:\n%1").arg(QDir::toNativeSeparators(path)));
            return;
        }
        if (attempt + 1 == kMaxPassphraseAttempts || !error.contains(QLatin1String("passphrase"), Qt::CaseInsensitive)) {
            QMessageBox::warning(this, tr("Import Key"),
                                 error.isEmpty() ? tr("Key import failed.") : error);
            return;
        }
        bool ok = false;
        passphrase = QInputDialog::getText(this, tr("Import Key"),
                                           tr("The key is passphrase-protected. Passphrase:"),
                                           QLineEdit::Password, QString(), &ok);
        if (!ok) {
            return;
        }
    }
}

void KeyManagerDialog::exportSelected()
{
    const int row = currentRow();
    if (row < 0 || row >= m_keys.size()) {
        return;
    }
    const KeyStore::KeyInfo info = m_keys.at(row);
    QString passphrase;
    if (info.encrypted) {
        bool ok = false;
        passphrase = QInputDialog::getText(this, tr("Export Public Key"),
                                           tr("Passphrase for %1:").arg(info.fileName),
                                           QLineEdit::Password, QString(), &ok);
        if (!ok) {
            return;
        }
    }
    QString line;
    QString error;
    if (!KeyStore::exportPublicKey(info.filePath, passphrase, &line, &error)) {
        QMessageBox::warning(this, tr("Export Public Key"),
                             error.isEmpty() ? tr("Export failed.") : error);
        return;
    }
    QGuiApplication::clipboard()->setText(line);
    const QString fingerprint =
        info.encrypted ? tr("(unlock to show)") : info.fingerprintSha256;
    QMessageBox::information(this, tr("Export Public Key"),
                             tr("Copied to the clipboard:\n\n%1\n\nFingerprint: %2")
                                 .arg(line, fingerprint));
}

void KeyManagerDialog::deleteSelected()
{
    const int row = currentRow();
    if (row < 0 || row >= m_keys.size()) {
        return;
    }
    const KeyStore::KeyInfo info = m_keys.at(row);
    const auto answer = QMessageBox::question(
        this, tr("Delete Key"),
        tr("Delete %1?\n\nSessions using it will need to be updated.").arg(info.fileName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!KeyStore::deleteKey(info.filePath, &error)) {
        QMessageBox::warning(this, tr("Delete Key"),
                             error.isEmpty() ? tr("Delete failed.") : error);
        return;
    }
    refresh();
}

void KeyManagerDialog::refreshKnownHosts()
{
    m_knownHostsTable->setRowCount(0);
    const QVector<KeyStore::KnownHostEntry> entries = KeyStore::listKnownHosts();
    m_knownHostsTable->setRowCount(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        const KeyStore::KnownHostEntry &entry = entries.at(i);
        m_knownHostsTable->setItem(i, 0, new QTableWidgetItem(entry.hosts));
        m_knownHostsTable->setItem(i, 1, new QTableWidgetItem(entry.keyType));
        m_knownHostsTable->setItem(i, 2, new QTableWidgetItem(entry.fingerprintSha256));
    }
    m_removeKnownHostButton->setEnabled(!entries.isEmpty());
}

void KeyManagerDialog::removeSelectedKnownHost()
{
    const int row = m_knownHostsTable->currentRow();
    if (row < 0) {
        return;
    }
    const QString hosts = m_knownHostsTable->item(row, 0)->text();
    const auto answer = QMessageBox::question(
        this, tr("Remove Known Host"),
        tr("Remove the stored host key for %1?\nThe next connection will ask for "
           "confirmation again.").arg(hosts),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    // Entries may list "host,ip": strip after the comma for the removal
    // needle (removeKnownHost does a prefix match on the raw line).
    QString needle = hosts;
    const int comma = needle.indexOf(QLatin1Char(','));
    if (comma > 0) {
        needle = needle.left(comma);
    }
    QString error;
    if (!KeyStore::removeKnownHost(needle, &error)) {
        QMessageBox::warning(this, tr("Remove Known Host"),
                             error.isEmpty() ? tr("Remove failed.") : error);
        return;
    }
    refreshKnownHosts();
}

QString KeyManagerDialog::pickKeyPath(QWidget *parent)
{
    const QVector<KeyStore::KeyInfo> keys = KeyStore::listKeys();
    if (keys.isEmpty()) {
        QMessageBox::information(parent, QObject::tr("Key Store"),
                                 QObject::tr("The key store is empty. Generate or import a key "
                                             "via Tools > Key Manager first."));
        return {};
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Select Key"));
    auto *layout = new QVBoxLayout(&dialog);

    auto *table = new QTableWidget(keys.size(), 3, &dialog);
    table->setHorizontalHeaderLabels({QObject::tr("Name"), QObject::tr("Type"),
                                      QObject::tr("SHA256 Fingerprint")});
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->hide();
    for (int i = 0; i < keys.size(); ++i) {
        const KeyStore::KeyInfo &info = keys.at(i);
        table->setItem(i, 0, new QTableWidgetItem(info.fileName));
        table->setItem(i, 1, new QTableWidgetItem(typeLabel(info)));
        table->setItem(i, 2, new QTableWidgetItem(
                                  info.encrypted ? QObject::tr("encrypted") : info.fingerprintSha256));
    }
    table->selectRow(0);
    layout->addWidget(table);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(box);

    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    const int row = table->currentRow();
    if (row < 0 || row >= keys.size()) {
        return {};
    }
    return keys.at(row).filePath;
}

} // namespace hssh
