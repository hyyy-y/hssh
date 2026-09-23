#ifndef HSSH_APP_DIALOGS_APPEARANCEDIALOG_H
#define HSSH_APP_DIALOGS_APPEARANCEDIALOG_H

#include <QDialog>

class QComboBox;
class QFontComboBox;
class QSpinBox;
class QSlider;

namespace hssh {

// PH1-05: terminal font (family + size), app theme (dark/light/high
// contrast) and window opacity. Changes apply live; OK persists them to
// the Config store.
class AppearanceDialog : public QDialog {
    Q_OBJECT

public:
    explicit AppearanceDialog(QWidget *parent = nullptr);

private:
    void apply();

    QFontComboBox *m_fontBox = nullptr;
    QSpinBox *m_sizeBox = nullptr;
    QComboBox *m_themeBox = nullptr;
    QSlider *m_opacitySlider = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_APPEARANCEDIALOG_H
