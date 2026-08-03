#ifndef HSSH_CORE_SESSIONMODEL_H
#define HSSH_CORE_SESSIONMODEL_H

#include "SessionConfig.h"

#include <QAbstractItemModel>
#include <QList>
#include <memory>

namespace hssh {

class SessionNode;

class SessionModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum class NodeType {
        Folder,
        Session
    };

    enum class Role {
        NodeTypeRole = Qt::UserRole + 1,
        SessionConfigRole
    };

    explicit SessionModel(QObject *parent = nullptr);
    ~SessionModel() override;

    // QAbstractItemModel interface
    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex &child) const override;
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // CRUD
    QModelIndex addFolder(const QString &name, const QModelIndex &parent = {});
    QModelIndex addSession(const SessionConfig &config, const QModelIndex &parent = {});
    bool removeNode(const QModelIndex &index);
    bool setNodeName(const QModelIndex &index, const QString &name);
    bool setSessionConfig(const QModelIndex &index, const SessionConfig &config);

    [[nodiscard]] SessionConfig sessionConfig(const QModelIndex &index) const;
    [[nodiscard]] NodeType nodeType(const QModelIndex &index) const;

    void clear();

    [[nodiscard]] QList<SessionConfig> allSessions() const;
    void loadSessions(const QList<SessionConfig> &sessions, const QMap<QString, QString> &folderPaths);

private:
    SessionNode *nodeFromIndex(const QModelIndex &index) const;

    std::unique_ptr<SessionNode> m_root;
};

} // namespace hssh

#endif // HSSH_CORE_SESSIONMODEL_H
