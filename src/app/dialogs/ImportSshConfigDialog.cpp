#include "ImportSshConfigDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hssh {

namespace {

bool isWildcardPattern(const QString &pattern)
{
    return pattern.contains(QLatin1Char('*'))
        || pattern.contains(QLatin1Char('?'))
        || pattern.startsWith(QLatin1Char('!'));
}

} // namespace

QList<ImportSshConfigDialog::Entry> ImportSshConfigDialog::parseConfig(const QString &text)
{
    // One Host block at a time; a block may carry several Host patterns,
    // and its options (HostName/Port/...) apply to every pattern in it.
    struct Block {
        QStringList aliases;
        QString hostName;
        int port = 22;
        QString user;
        QString identityFile;
        QString proxyJump;
    };

    QList<Entry> result;
    Block block;
    bool inBlock = false;

    const auto flushBlock = [&result, &block, &inBlock]() {
        if (inBlock) {
            for (const QString &alias : std::as_const(block.aliases)) {
                Entry e;
                e.alias = alias;
                e.hostName = block.hostName.isEmpty() ? alias : block.hostName;
                e.port = block.port > 0 ? block.port : 22;
                e.user = block.user;
                e.identityFile = block.identityFile;
                e.proxyJump = block.proxyJump;
                result.append(e);
            }
        }
        block = Block();
        inBlock = false;
    };

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString rawLine : lines) {
        const int hash = rawLine.indexOf(QLatin1Char('#'));
        if (hash >= 0) {
            rawLine.truncate(hash);
        }
        rawLine = rawLine.trimmed();
        if (rawLine.isEmpty()) {
            continue;
        }
        // Both "Key Value" and "Key=Value" forms.
        int sep = -1;
        for (int i = 0; i < rawLine.size(); ++i) {
            const QChar c = rawLine.at(i);
            if (c == QLatin1Char('=') || c.isSpace()) {
                sep = i;
                break;
            }
        }
        if (sep <= 0) {
            continue;
        }
        const QString key = rawLine.left(sep).trimmed().toLower();
        const QString value = rawLine.mid(sep + 1).trimmed();
        if (key == QLatin1String("host")) {
            flushBlock();
            const QStringList patterns = value.split(QRegularExpression(QStringLiteral("\\s+")),
                                                     Qt::SkipEmptyParts);
            for (const QString &pattern : patterns) {
                if (!isWildcardPattern(pattern)) {
                    block.aliases.append(pattern);
                }
            }
            inBlock = !block.aliases.isEmpty();
        } else if (key == QLatin1String("hostname")) {
            if (inBlock && block.hostName.isEmpty()) {
                block.hostName = value;
            }
        } else if (key == QLatin1String("port")) {
            if (inBlock) {
                bool ok = false;
                const int port = value.toInt(&ok);
                if (ok && port > 0 && port < 65536) {
                    block.port = port;
                }
            }
        } else if (key == QLatin1String("user")) {
            if (inBlock && block.user.isEmpty()) {
                block.user = value;
            }
        } else if (key == QLatin1String("identityfile")) {
            if (inBlock && block.identityFile.isEmpty()) {
                // ~ expansion to the local home.
                QString path = value;
                if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/"))) {
                    path.replace(0, 1, QDir::homePath());
                }
                block.identityFile = path;
            }
        } else if (key == QLatin1String("proxyjump")) {
            if (inBlock && block.proxyJump.isEmpty()) {
                block.proxyJump = value;
            }
        }
        // Unknown keys (ForwardAgent, ServerAliveInterval, ...) are ignored.
    }
    flushBlock();
    return result;
}

ImportSshConfigDialog::ImportSshConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Import OpenSSH config"));
    setMinimumSize(700, 420);

    auto *layout = new QVBoxLayout(this);

    // File picker: default ~/.ssh/config.
    QString path = QDir::homePath() + QStringLiteral("/.ssh/config");
    auto *pathRow = new QHBoxLayout();
    auto *pathEdit = new QLineEdit(path, this);
    auto *browse = new QPushButton(tr("Browse..."), this);
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browse);
    layout->addLayout(pathRow);

    auto *hint = new QLabel(
        tr("Hosts parsed from the config. Wildcard patterns are skipped; "
           "ProxyJump is stored but not used until jump-host support lands."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
        {tr("Import"), tr("Name"), tr("Host"), tr("Port"), tr("User")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_table, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    auto reload = [this, pathEdit]() {
        QFile file(pathEdit->text().trimmed());
        if (file.open(QIODevice::ReadOnly)) {
            fillTable(parseConfig(QString::fromUtf8(file.readAll())));
        } else {
            fillTable({});
        }
    };
    connect(browse, &QPushButton::clicked, this, [this, pathEdit]() {
        const QString picked = QFileDialog::getOpenFileName(this, tr("OpenSSH config"),
                                                             pathEdit->text());
        if (!picked.isEmpty()) {
            pathEdit->setText(picked);
            pathEdit->textChanged(QString()); // no-op; reload happens below
        }
    });
    connect(pathEdit, &QLineEdit::textChanged, this, [reload](const QString &) { reload(); });
    connect(browse, &QPushButton::clicked, this, [reload]() { reload(); });
    reload();
}

void ImportSshConfigDialog::fillTable(const QList<Entry> &entries)
{
    m_entries = entries;
    m_table->setRowCount(entries.size());
    for (int row = 0; row < entries.size(); ++row) {
        const Entry &e = entries.at(row);
        auto *check = new QCheckBox(this);
        check->setChecked(true);
        m_table->setCellWidget(row, 0, check);
        auto setItem = [this, row](int col, const QString &text) {
            auto *item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_table->setItem(row, col, item);
        };
        setItem(1, e.alias);
        setItem(2, e.hostName);
        setItem(3, QString::number(e.port));
        setItem(4, e.user);
    }
}

QList<SessionConfig> ImportSshConfigDialog::selectedConfigs() const
{
    QList<SessionConfig> result;
    for (int row = 0; row < m_entries.size(); ++row) {
        auto *check = qobject_cast<QCheckBox *>(m_table->cellWidget(row, 0));
        if (!check || !check->isChecked()) {
            continue;
        }
        const Entry &e = m_entries.at(row);
        SessionConfig config;
        config.setName(e.alias);
        config.setHost(e.hostName);
        config.setPort(e.port);
        config.setUsername(e.user);
        if (!e.identityFile.isEmpty()) {
            config.setAuthMethod(AuthMethod::PublicKey);
            config.setPrivateKeyPath(e.identityFile);
        } else {
            config.setAuthMethod(AuthMethod::Password);
        }
        result.append(config);
    }
    return result;
}

} // namespace hssh
