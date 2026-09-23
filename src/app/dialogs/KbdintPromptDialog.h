#ifndef HSSH_APP_DIALOGS_KBDINTPROMPTDIALOG_H
#define HSSH_APP_DIALOGS_KBDINTPROMPTDIALOG_H

#include <QDialog>

#include <QList>
#include <QStringList>

class QLineEdit;
class QCheckBox;

namespace hssh {

// PH2-13: one keyboard-interactive round (2FA): server name/instruction
// plus one input per prompt; hidden inputs are masked. "Remember" caches
// the answers for this tab's lifetime.
class KbdintPromptDialog : public QDialog {
    Q_OBJECT

public:
    explicit KbdintPromptDialog(const QString &name, const QString &instruction,
                                const QStringList &prompts, const QList<bool> &echo,
                                QWidget *parent = nullptr);

    [[nodiscard]] QStringList answers() const;
    [[nodiscard]] bool rememberForSession() const;

private:
    QList<QLineEdit *> m_edits;
    QCheckBox *m_remember = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_KBDINTPROMPTDIALOG_H
