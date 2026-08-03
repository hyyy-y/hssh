#ifndef HSSH_APP_DIALOGS_EDITFOLDERDIALOG_H
#define HSSH_APP_DIALOGS_EDITFOLDERDIALOG_H

#include <QDialog>

class QLineEdit;

namespace hssh {

class EditFolderDialog : public QDialog {
    Q_OBJECT

public:
    explicit EditFolderDialog(QWidget *parent = nullptr);
    ~EditFolderDialog() override;

    void setFolderName(const QString &name);
    [[nodiscard]] QString folderName() const;

private:
    void setupUi();
    void accept() override;

    QLineEdit *m_nameEdit = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_EDITFOLDERDIALOG_H
