#ifndef HSSH_APP_WIDGETS_LOCALFILEWIDGET_H
#define HSSH_APP_WIDGETS_LOCALFILEWIDGET_H

#include <QColor>
#include <QHash>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QFileSystemModel;
class QLineEdit;
class QTableView;
QT_END_NAMESPACE

namespace hssh {

class LocalFileProxyModel;

// Local filesystem browser pane: navigation bar + directory listing.
// Used standalone (left dock) and as the left pane of FileCompareWidget.
class LocalFileWidget : public QWidget {
    Q_OBJECT

public:
    explicit LocalFileWidget(QWidget *parent = nullptr);
    ~LocalFileWidget() override;

    [[nodiscard]] QString currentPath() const { return m_currentPath; }
    void navigateTo(const QString &path);
    void refresh();

    // Local filesystem paths of the selected rows (files and directories).
    [[nodiscard]] QStringList selectedPaths() const;

    // Compare highlighting: filename -> text color, applied to column 0.
    void setRowColors(const QHash<QString, QColor> &colors);
    void clearRowColors();

signals:
    void pathChanged(const QString &path);
    void fileActivated(const QString &localPath);

private:
    void goUp();
    void goHome();
    void onActivated(const QModelIndex &proxyIndex);

    QFileSystemModel *m_fsModel = nullptr;
    LocalFileProxyModel *m_proxy = nullptr;
    QTableView *m_view = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QString m_currentPath;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_LOCALFILEWIDGET_H
