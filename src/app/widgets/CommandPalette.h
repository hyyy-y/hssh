#ifndef HSSH_APP_WIDGETS_COMMANDPALETTE_H
#define HSSH_APP_WIDGETS_COMMANDPALETTE_H

#include <QDialog>

#include <functional>

class QLineEdit;
class QListView;
class QStandardItemModel;

namespace hssh {

// PH2-07: fuzzy command palette (Ctrl+Shift+P). Items are contributed by
// the MainWindow (menu actions, saved sessions, open tabs); recents are
// persisted through the Config store.
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    struct Item {
        QString id;      // stable id used for the recents list
        QString title;   // shown to the user
        QString category; // "Command" / "Session" / "Tab" ...
        QString shortcut; // display-only key sequence
        std::function<void()> action;
    };

    explicit CommandPalette(QWidget *parent = nullptr);

    void setItems(const QList<Item> &items);
    // Drive the filter programmatically (tests); the internal edit does the
    // same on text changes.
    void setFilter(const QString &text);
    [[nodiscard]] int resultCount() const { return m_filtered.size(); }
    [[nodiscard]] QString resultIdAt(int row) const;
    void runResult(int row);

    // Fuzzy scoring: -1 = no match, otherwise larger is better.
    [[nodiscard]] static int fuzzyScore(const QString &text, const QString &query);

    // Recents persistence (Config key ui/commandPaletteRecents, max 10).
    [[nodiscard]] static QStringList recentIds();
    static void pushRecent(const QString &id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refilter();

    QList<Item> m_items;
    QList<int> m_filtered; // indices into m_items
    QString m_query;
    QLineEdit *m_edit = nullptr;
    QListView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_COMMANDPALETTE_H
