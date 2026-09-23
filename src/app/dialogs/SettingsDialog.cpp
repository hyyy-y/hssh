#include "SettingsDialog.h"

#include "utils/Config.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QApplication>
#include <QCoreApplication>
#include <QComboBox>
#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace hssh {

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    resize(560, 460);
    buildUi();
}

void SettingsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *body = new QHBoxLayout();
    layout->addLayout(body);

    m_groupList = new QListWidget(this);
    m_groupList->setFixedWidth(160);
    body->addWidget(m_groupList);

    m_pages = new QStackedWidget(this);
    body->addWidget(m_pages, 1);

    const QVector<ConfigKey> keys = Config::registeredKeys();

    // ConfigKey labels/groups/tooltips are static strings (registered
    // without a tr() context) — route them through the "ConfigKeys" catalog
    // so the settings pages localize too (falls back to the raw text).
    const auto cfgText = [](const QString &text) {
        return QCoreApplication::translate("ConfigKeys", text.toUtf8().constData());
    };

    // Group keys by ConfigKey::group, preserving first-appearance order.
    QStringList groupOrder;
    QMap<QString, QVector<ConfigKey>> byGroup;
    for (const ConfigKey &key : keys) {
        const QString group = key.group.isEmpty() ? tr("General") : cfgText(key.group);
        if (!byGroup.contains(group)) {
            groupOrder.append(group);
        }
        byGroup[group].append(key);
    }

    for (const QString &group : groupOrder) {
        auto *page = new QWidget(this);
        auto *form = new QFormLayout(page);
        form->setContentsMargins(16, 12, 16, 12);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        for (const ConfigKey &key : byGroup[group]) {
            QVariant original;
            QWidget *editor = createEditor(key, page, &original);
            if (!editor) {
                continue;
            }
            const QString label = key.label.isEmpty() ? key.key : cfgText(key.label);
            form->addRow(label + QLatin1Char(':'), editor);
            if (!key.tooltip.isEmpty()) {
                editor->setToolTip(cfgText(key.tooltip));
            }
            m_rows.append({key, editor, original});
        }

        auto *scroll = new QScrollArea(this);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        m_pages->addWidget(scroll);
        m_groupList->addItem(group);
    }

    if (!m_groupList->count()) {
        m_groupList->addItem(tr("General"));
        auto *empty = new QLabel(tr("No settings available."), this);
        m_pages->addWidget(empty);
    }
    m_groupList->setCurrentRow(0);
    connect(m_groupList, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        applyChanges();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QWidget *SettingsDialog::createEditor(const ConfigKey &key, QWidget *parent, QVariant *original)
{
    const QVariant current = Config::instance().value(key.key, key.defaultValue);
    *original = current;

    switch (key.type) {
    case ConfigKeyType::Bool: {
        auto *check = new QCheckBox(parent);
        check->setChecked(current.toBool());
        return check;
    }
    case ConfigKeyType::Int: {
        auto *spin = new QSpinBox(parent);
        spin->setRange(0, 1000000);
        spin->setValue(current.toInt());
        return spin;
    }
    case ConfigKeyType::String: {
        auto *edit = new QLineEdit(parent);
        edit->setText(current.toString());
        return edit;
    }
    case ConfigKeyType::Password: {
        auto *edit = new QLineEdit(parent);
        edit->setEchoMode(QLineEdit::Password);
        edit->setText(current.toString());
        return edit;
    }
    case ConfigKeyType::Enum: {
        auto *combo = new QComboBox(parent);
        combo->addItems(key.enumOptions);
        const int idx = combo->findText(current.toString());
        combo->setCurrentIndex(idx < 0 ? 0 : idx);
        return combo;
    }
    case ConfigKeyType::Font: {
        auto *combo = new QFontComboBox(parent);
        if (!current.toString().isEmpty()) {
            combo->setCurrentFont(QFont(current.toString()));
        }
        return combo;
    }
    case ConfigKeyType::Color: {
        auto *button = new QPushButton(parent);
        const QColor color(current.toString());
        if (color.isValid()) {
            button->setStyleSheet(QStringLiteral("background-color: %1;").arg(color.name()));
        }
        button->setText(tr("Choose..."));
        connect(button, &QPushButton::clicked, this, [this, button, key, original]() {
            const QColor initial(Config::instance().value(key.key, key.defaultValue).toString());
            const QColor chosen = QColorDialog::getColor(initial, this, key.label);
            if (chosen.isValid()) {
                button->setStyleSheet(QStringLiteral("background-color: %1;").arg(chosen.name()));
                button->setText(chosen.name());
                *original = chosen.name();
            }
        });
        return button;
    }
    }
    return nullptr;
}

QVariant SettingsDialog::readEditor(const Row &row) const
{
    switch (row.key.type) {
    case ConfigKeyType::Bool:
        return static_cast<QCheckBox *>(row.editor)->isChecked();
    case ConfigKeyType::Int:
        return static_cast<QSpinBox *>(row.editor)->value();
    case ConfigKeyType::String:
    case ConfigKeyType::Password:
        return static_cast<QLineEdit *>(row.editor)->text();
    case ConfigKeyType::Enum:
        return static_cast<QComboBox *>(row.editor)->currentText();
    case ConfigKeyType::Font:
        return static_cast<QFontComboBox *>(row.editor)->currentFont().family();
    case ConfigKeyType::Color:
        return row.original; // Updated in-place by the color dialog.
    }
    return {};
}

void SettingsDialog::applyChanges()
{
    bool languageChanged = false;
    for (const Row &row : m_rows) {
        const QVariant newValue = readEditor(row);
        if (newValue == row.original) {
            continue;
        }
        Config::instance().setValue(row.key.key, newValue);
        if (row.key.key == QLatin1String("ui/language")) {
            languageChanged = true;
        }
    }
    Config::instance().sync();

    // The interface language only loads at startup (Qt has no global
    // retranslate); offer an immediate restart instead of a silent surprise.
    if (languageChanged) {
        const auto answer = QMessageBox::question(
            this, tr("Language changed"),
            tr("The interface language takes effect after a restart. Restart now?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            const QString exe = QCoreApplication::applicationFilePath();
            const QStringList args = QCoreApplication::arguments().mid(1);
            QProcess::startDetached(exe, args);
            QApplication::quit();
        }
    }
}

} // namespace hssh
