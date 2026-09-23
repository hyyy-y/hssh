#include "AppearanceDialog.h"

#include "utils/Config.h"
#include "utils/Theme.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>

namespace hssh {

AppearanceDialog::AppearanceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Appearance"));
    setMinimumWidth(380);

    auto *layout = new QFormLayout(this);

    m_fontBox = new QFontComboBox(this);
    m_fontBox->setFontFilters(QFontComboBox::MonospacedFonts);
    layout->addRow(tr("Terminal &font:"), m_fontBox);

    m_sizeBox = new QSpinBox(this);
    m_sizeBox->setRange(6, 32);
    layout->addRow(tr("Font &size:"), m_sizeBox);

    m_themeBox = new QComboBox(this);
    m_themeBox->addItem(tr("Dark"), QStringLiteral("dark"));
    m_themeBox->addItem(tr("Light"), QStringLiteral("light"));
    m_themeBox->addItem(tr("High contrast"), QStringLiteral("highcontrast"));
    layout->addRow(tr("&Theme:"), m_themeBox);

    m_opacitySlider = new QSlider(Qt::Horizontal, this);
    m_opacitySlider->setRange(60, 100); // percent
    m_opacitySlider->setToolTip(tr("Window opacity — below 100% the whole "
                                   "window (including text) becomes translucent"));
    layout->addRow(tr("Window &opacity:"), m_opacitySlider);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addRow(buttons);

    // Load current values (Config defaults mirror the code defaults).
    m_fontBox->setCurrentFont(QFont(
        Config::instance().value(QStringLiteral("terminal/fontFamily")).toString()));
    m_sizeBox->setValue(Config::instance()
                            .value(QStringLiteral("terminal/fontSize"), 10)
                            .toInt());
    const QString theme = Config::instance()
                              .value(QStringLiteral("ui/theme"), QStringLiteral("dark"))
                              .toString();
    const int themeIndex = m_themeBox->findData(theme);
    m_themeBox->setCurrentIndex(themeIndex < 0 ? 0 : themeIndex);
    m_opacitySlider->setValue(Config::instance()
                                  .value(QStringLiteral("ui/windowOpacity"), 100)
                                  .toInt());

    // Live preview on every change; OK persists, Cancel cannot undo the
    // preview (theme/font), which is the common Qt behavior.
    connect(m_fontBox, &QFontComboBox::currentFontChanged, this, &AppearanceDialog::apply);
    connect(m_sizeBox, &QSpinBox::valueChanged, this, &AppearanceDialog::apply);
    connect(m_themeBox, &QComboBox::currentIndexChanged, this, &AppearanceDialog::apply);
    connect(m_opacitySlider, &QSlider::valueChanged, this, &AppearanceDialog::apply);
}

void AppearanceDialog::apply()
{
    auto &config = Config::instance();
    const QString family = m_fontBox->currentFont().family();
    const int size = m_sizeBox->value();
    config.setValue(QStringLiteral("terminal/fontFamily"), family);
    config.setValue(QStringLiteral("terminal/fontSize"), size);
    const QString theme = m_themeBox->currentData().toString();
    config.setValue(QStringLiteral("ui/theme"), theme);
    const int opacity = m_opacitySlider->value();
    config.setValue(QStringLiteral("ui/windowOpacity"), opacity);
    config.sync();

    // Theme + opacity are app/window-wide.
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        hssh::applyTheme(*app, theme);
    }
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w->isWindow()) {
            w->setWindowOpacity(opacity / 100.0);
        }
    }
    // Terminal font is delivered through the parent MainWindow.
    if (parentWidget()) {
        QFont font(family);
        font.setPointSize(size);
        QMetaObject::invokeMethod(parentWidget(), "applyTerminalFont",
                                  Qt::DirectConnection, Q_ARG(QFont, font));
    }
}

} // namespace hssh
