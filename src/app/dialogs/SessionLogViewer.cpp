#include "SessionLogViewer.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

namespace hssh {

namespace {

constexpr qint64 kMaxDisplayBytes = 20 * 1024 * 1024; // cap huge logs
constexpr int kMaxHighlightedMatches = 10000;

QString logsDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QDir::separator() + QStringLiteral("logs");
}

QString humanSize(qint64 bytes)
{
    if (bytes >= 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
    }
    if (bytes >= 1024) {
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    }
    return QString::number(bytes) + QStringLiteral(" B");
}

} // namespace

SessionLogViewer::SessionLogViewer(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Session Log Viewer"));
    setObjectName(QStringLiteral("sessionLogViewer"));
    resize(1000, 640);

    auto *rootLayout = new QVBoxLayout(this);

    // Filter bar: time range + refresh + export.
    auto *filterBar = new QHBoxLayout;
    filterBar->addWidget(new QLabel(tr("Show:"), this));
    m_rangeCombo = new QComboBox(this);
    m_rangeCombo->addItem(tr("All time"), 0);
    m_rangeCombo->addItem(tr("Today"), 1);
    m_rangeCombo->addItem(tr("Last 3 days"), 3);
    m_rangeCombo->addItem(tr("Last 7 days"), 7);
    m_rangeCombo->addItem(tr("Last 30 days"), 30);
    filterBar->addWidget(m_rangeCombo);
    filterBar->addStretch();

    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    filterBar->addWidget(refreshButton);

    auto *exportButton = new QPushButton(tr("Export..."), this);
    filterBar->addWidget(exportButton);
    rootLayout->addLayout(filterBar);

    // Splitter: file list (left) / content (right).
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    rootLayout->addWidget(splitter, 1);

    m_fileList = new QListWidget(splitter);
    m_fileList->setMinimumWidth(240);
    splitter->addWidget(m_fileList);

    m_content = new QPlainTextEdit(splitter);
    m_content->setReadOnly(true);
    m_content->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_content->setFont(mono);
    splitter->addWidget(m_content);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    // Search bar under the content.
    auto *searchBar = new QHBoxLayout;
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Find in log (Enter = next match)"));
    m_searchEdit->setClearButtonEnabled(true);
    searchBar->addWidget(m_searchEdit, 1);
    m_matchLabel = new QLabel(this);
    searchBar->addWidget(m_matchLabel);
    rootLayout->addLayout(searchBar);

    connect(m_rangeCombo, &QComboBox::currentIndexChanged, this, &SessionLogViewer::refreshFileList);
    connect(refreshButton, &QPushButton::clicked, this, [this]() {
        refreshFileList();
        // An open tab keeps appending; re-read the selected file too.
        loadSelectedFile();
    });
    connect(exportButton, &QPushButton::clicked, this, &SessionLogViewer::exportContent);
    connect(m_fileList, &QListWidget::currentRowChanged, this, [this]() {
        m_searchEdit->clear();
        loadSelectedFile();
    });
    connect(m_searchEdit, &QLineEdit::textChanged, this, &SessionLogViewer::applySearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &SessionLogViewer::gotoNextMatch);

    refreshFileList();
}

void SessionLogViewer::selectFile(const QString &filePath)
{
    for (int i = 0; i < m_fileList->count(); ++i) {
        if (m_fileList->item(i)->data(Qt::UserRole).toString() == filePath) {
            m_fileList->setCurrentRow(i);
            return;
        }
    }
    // Not visible under the current range filter: widen to "All time" and
    // retry once, so a direct "view this tab's log" never silently no-ops.
    if (m_rangeCombo->currentIndex() != 0) {
        m_rangeCombo->setCurrentIndex(0);
        for (int i = 0; i < m_fileList->count(); ++i) {
            if (m_fileList->item(i)->data(Qt::UserRole).toString() == filePath) {
                m_fileList->setCurrentRow(i);
                return;
            }
        }
    }
}

QDateTime SessionLogViewer::timestampFromFileName(const QString &fileName)
{
    const QString base = QFileInfo(fileName).fileName();
    if (!base.startsWith(QStringLiteral("session_")) || !base.endsWith(QStringLiteral(".log"))) {
        return {};
    }
    const QString stem = base.mid(8, base.length() - 8 - 4);
    return QDateTime::fromString(stem, QStringLiteral("yyyyMMdd_HHmmss_zzz"));
}

QString SessionLogViewer::stripAnsi(const QByteArray &raw)
{
    QByteArray out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == 0x1b) { // ESC
            if (i + 1 >= raw.size()) {
                break; // dangling escape at end of buffer
            }
            const unsigned char next = static_cast<unsigned char>(raw[i + 1]);
            if (next == '[') {
                // CSI: parameter (0x30-0x3F) + intermediate (0x20-0x2F)
                // bytes terminated by a final byte (0x40-0x7E).
                int j = i + 2;
                while (j < raw.size()) {
                    const unsigned char p = static_cast<unsigned char>(raw[j]);
                    if (p >= 0x40 && p <= 0x7e) {
                        ++j;
                        break;
                    }
                    if (p < 0x20 || p > 0x3f) {
                        break; // malformed CSI: bail out at this byte
                    }
                    ++j;
                }
                i = j;
            } else if (next == ']') {
                // OSC: terminated by BEL or ST (ESC \).
                int j = i + 2;
                while (j < raw.size()) {
                    const unsigned char p = static_cast<unsigned char>(raw[j]);
                    if (p == 0x07) {
                        ++j;
                        break;
                    }
                    if (p == 0x1b && j + 1 < raw.size()
                        && static_cast<unsigned char>(raw[j + 1]) == '\\') {
                        j += 2;
                        break;
                    }
                    ++j;
                }
                i = j;
            } else if (next == '(' || next == ')' || next == '*' || next == '+'
                       || next == '#' || next == '%') {
                i += 3; // two-byte sequence + one designator char
            } else {
                i += 2; // ESC =, ESC >, ESC 7, ESC 8, ESC M, ...
            }
            continue;
        }
        if (c == '\r') {
            // CR: normalize to LF; a following LF is consumed below so CRLF
            // collapses into a single newline.
            out.append('\n');
            if (i + 1 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == '\n') {
                i += 2;
            } else {
                ++i;
            }
            continue;
        }
        if (c == '\t' || c == '\n') {
            out.append(static_cast<char>(c));
            ++i;
            continue;
        }
        if (c < 0x20) {
            // Bell, backspace, NUL and friends: not meaningful as text.
            ++i;
            continue;
        }
        out.append(raw[i]);
        ++i;
    }
    return QString::fromUtf8(out);
}

void SessionLogViewer::refreshFileList()
{
    m_fileList->blockSignals(true);
    m_fileList->clear();
    m_fileList->blockSignals(false);
    m_content->clear();
    m_currentPath.clear();
    m_matches.clear();
    m_matchLabel->clear();

    const int days = m_rangeCombo->currentData().toInt();
    QDateTime cutoff;
    if (days == 1) {
        cutoff = QDate::currentDate().startOfDay();
    } else if (days > 1) {
        cutoff = QDateTime::currentDateTime().addDays(-days);
    }

    struct Entry {
        QString path;
        QDateTime start;
        QString display;
    };
    QList<Entry> entries;

    const QDir dir(logsDirectory());
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("session_*.log")}, QDir::Files, QDir::Name);
    for (const QFileInfo &info : files) {
        QDateTime start = timestampFromFileName(info.fileName());
        if (!start.isValid()) {
            start = info.lastModified();
        }
        if (cutoff.isValid() && start < cutoff) {
            continue;
        }
        entries.append({info.absoluteFilePath(), start,
                        QStringLiteral("%1  ·  %2")
                            .arg(start.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                 humanSize(info.size()))});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.start > b.start; });

    for (const Entry &e : entries) {
        auto *item = new QListWidgetItem(e.display, m_fileList);
        item->setData(Qt::UserRole, e.path);
        item->setToolTip(e.path);
    }
}

void SessionLogViewer::loadSelectedFile()
{
    QListWidgetItem *item = m_fileList->currentItem();
    m_content->clear();
    m_matches.clear();
    m_currentMatch = -1;
    m_matchLabel->clear();
    m_currentPath.clear();
    if (!item) {
        return;
    }

    const QString path = item->data(Qt::UserRole).toString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_content->setPlainText(tr("Cannot open %1: %2").arg(path, file.errorString()));
        return;
    }

    QByteArray raw = file.readAll();
    file.close();
    if (raw.size() > kMaxDisplayBytes) {
        // Keep the TAIL of the log: recent activity beats the login banner
        // from days ago when a log outgrew the cap.
        raw.remove(0, raw.size() - kMaxDisplayBytes);
        m_content->setPlainText(
            tr("[%1 MB log truncated to the last %2 MB]\n\n")
                .arg(QString::number((file.size()) / (1024.0 * 1024.0), 'f', 1))
                .arg(kMaxDisplayBytes / (1024 * 1024)));
    } else {
        m_content->setPlainText(QString());
    }

    m_currentPath = path;
    m_content->appendPlainText(stripAnsi(raw));
    QTextCursor cursor = m_content->textCursor();
    cursor.movePosition(QTextCursor::Start);
    m_content->setTextCursor(cursor);

    if (!m_searchEdit->text().isEmpty()) {
        applySearch();
    }
}

void SessionLogViewer::applySearch()
{
    m_matches.clear();
    m_currentMatch = -1;
    QList<QTextEdit::ExtraSelection> selections;
    QTextDocument *doc = m_content->document();

    const QString needle = m_searchEdit->text();
    if (!needle.isEmpty()) {
        QTextCursor cursor(doc);
        QColor matchColor(Qt::yellow);
        matchColor.setAlpha(120);
        while (!cursor.isNull()) {
            cursor = doc->find(needle, cursor);
            if (cursor.isNull()) {
                break;
            }
            m_matches.push_back(cursor);
            if (m_matches.size() <= kMaxHighlightedMatches) {
                QTextEdit::ExtraSelection sel;
                sel.format.setBackground(matchColor);
                sel.cursor = cursor;
                selections.append(sel);
            } else {
                break; // avoid pathological highlight cost on giant logs
            }
        }
    }
    m_content->setExtraSelections(selections);

    if (needle.isEmpty()) {
        m_matchLabel->clear();
    } else if (m_matches.size() > kMaxHighlightedMatches) {
        m_matchLabel->setText(tr("> %1 matches").arg(kMaxHighlightedMatches));
    } else {
        m_matchLabel->setText(tr("%1 match(es)").arg(m_matches.size()));
    }
}

void SessionLogViewer::gotoNextMatch()
{
    if (m_matches.empty()) {
        return;
    }
    m_currentMatch = (m_currentMatch + 1) % static_cast<int>(m_matches.size());
    const QTextCursor &target = m_matches.at(static_cast<size_t>(m_currentMatch));
    m_content->setTextCursor(target);
    m_content->ensureCursorVisible();
    m_matchLabel->setText(tr("%1 / %2").arg(m_currentMatch + 1).arg(m_matches.size()));
}

void SessionLogViewer::exportContent()
{
    if (m_currentPath.isEmpty()) {
        QMessageBox::information(this, tr("Export"), tr("Select a log file first."));
        return;
    }

    const QString suggested =
        QFileInfo(m_currentPath).absolutePath() + QDir::separator()
        + QFileInfo(m_currentPath).completeBaseName() + QStringLiteral("_export.txt");
    const QString target = QFileDialog::getSaveFileName(
        this, tr("Export Log"), suggested, tr("Text Files (*.txt);;All Files (*)"));
    if (target.isEmpty()) {
        return;
    }

    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Cannot write %1: %2").arg(target, out.errorString()));
        return;
    }
    out.write(m_content->toPlainText().toUtf8());
    out.close();
    QMessageBox::information(this, tr("Export"), tr("Exported to %1").arg(target));
}

} // namespace hssh
