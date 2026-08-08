#include "SessionModel.h"

#include <QUuid>
#include <functional>

namespace hssh {

class SessionNode {
public:
    explicit SessionNode(SessionModel::NodeType type, SessionNode *parent = nullptr)
        : type(type)
        , parent(parent)
    {
    }

    ~SessionNode()
    {
        qDeleteAll(children);
    }

    int row() const
    {
        if (!parent) {
            return 0;
        }
        return parent->children.indexOf(const_cast<SessionNode *>(this));
    }

    SessionModel::NodeType type;
    SessionNode *parent = nullptr;
    QList<SessionNode *> children;

    QString id;
    QString name;
    SessionConfig config;
};

SessionModel::SessionModel(QObject *parent)
    : QAbstractItemModel(parent)
    , m_root(std::make_unique<SessionNode>(NodeType::Folder))
{
    m_root->id = QStringLiteral("root");
    m_root->name = QStringLiteral("Sessions");
}

SessionModel::~SessionModel() = default;

QModelIndex SessionModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column != 0) {
        return {};
    }

    SessionNode *parentNode = nodeFromIndex(parent);
    if (!parentNode || row >= parentNode->children.size()) {
        return {};
    }

    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex SessionModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) {
        return {};
    }

    SessionNode *childNode = nodeFromIndex(child);
    if (!childNode || !childNode->parent || childNode->parent == m_root.get()) {
        return {};
    }

    return createIndex(childNode->parent->row(), 0, childNode->parent);
}

int SessionModel::rowCount(const QModelIndex &parent) const
{
    SessionNode *parentNode = nodeFromIndex(parent);
    if (!parentNode) {
        return 0;
    }
    return parentNode->children.size();
}

int SessionModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return 1;
}

QVariant SessionModel::data(const QModelIndex &index, int role) const
{
    SessionNode *node = nodeFromIndex(index);
    if (!node) {
        return {};
    }

    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return node->name.isEmpty() ? node->config.displayName() : node->name;
    case static_cast<int>(Role::NodeTypeRole):
        return static_cast<int>(node->type);
    case static_cast<int>(Role::SessionConfigRole):
        if (node->type == NodeType::Session) {
            return QVariant::fromValue(node->config);
        }
        break;
    }

    return {};
}

Qt::ItemFlags SessionModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
}

QVariant SessionModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    Q_UNUSED(section)
    Q_UNUSED(orientation)
    if (role == Qt::DisplayRole) {
        return tr("Sessions");
    }
    return {};
}

QModelIndex SessionModel::addFolder(const QString &name, const QModelIndex &parent)
{
    SessionNode *parentNode = nodeFromIndex(parent);
    if (!parentNode) {
        return {};
    }

    const int row = parentNode->children.size();
    beginInsertRows(parent, row, row);

    auto *node = new SessionNode(NodeType::Folder, parentNode);
    node->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    node->name = name;
    parentNode->children.append(node);

    endInsertRows();
    return index(row, 0, parent);
}

QModelIndex SessionModel::addSession(const SessionConfig &config, const QModelIndex &parent)
{
    SessionNode *parentNode = nodeFromIndex(parent);
    if (!parentNode) {
        return {};
    }

    const int row = parentNode->children.size();
    beginInsertRows(parent, row, row);

    auto *node = new SessionNode(NodeType::Session, parentNode);
    node->id = config.id();
    node->name = config.name();
    node->config = config;
    parentNode->children.append(node);

    endInsertRows();
    return index(row, 0, parent);
}

bool SessionModel::removeNode(const QModelIndex &index)
{
    SessionNode *node = nodeFromIndex(index);
    if (!node || node == m_root.get() || !node->parent) {
        return false;
    }

    SessionNode *parentNode = node->parent;
    const int row = node->row();

    beginRemoveRows(parent(index), row, row);
    parentNode->children.removeAt(row);
    delete node;
    endRemoveRows();

    return true;
}

bool SessionModel::setNodeName(const QModelIndex &index, const QString &name)
{
    SessionNode *node = nodeFromIndex(index);
    if (!node || node == m_root.get()) {
        return false;
    }

    node->name = name;
    if (node->type == NodeType::Session) {
        node->config.setName(name);
    }

    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
}

bool SessionModel::setSessionConfig(const QModelIndex &index, const SessionConfig &config)
{
    SessionNode *node = nodeFromIndex(index);
    if (!node || node->type != NodeType::Session) {
        return false;
    }

    node->config = config;
    node->id = config.id();
    node->name = config.name();

    emit dataChanged(index, index, {Qt::DisplayRole, static_cast<int>(Role::SessionConfigRole)});
    return true;
}

SessionConfig SessionModel::sessionConfig(const QModelIndex &index) const
{
    SessionNode *node = nodeFromIndex(index);
    if (!node || node->type != NodeType::Session) {
        return {};
    }
    return node->config;
}

SessionModel::NodeType SessionModel::nodeType(const QModelIndex &index) const
{
    SessionNode *node = nodeFromIndex(index);
    if (!node) {
        return NodeType::Folder;
    }
    return node->type;
}

QString SessionModel::nodeId(const QModelIndex &index) const
{
    SessionNode *node = nodeFromIndex(index);
    if (!node || node == m_root.get()) {
        return {};
    }
    return node->id;
}

void SessionModel::clear()
{
    beginResetModel();
    m_root->children.clear();
    endResetModel();
}

QList<SessionConfig> SessionModel::allSessions() const
{
    QList<SessionConfig> result;
    std::function<void(SessionNode *)> collect = [&](SessionNode *node) {
        if (node->type == NodeType::Session) {
            result.append(node->config);
        }
        for (SessionNode *child : node->children) {
            collect(child);
        }
    };
    collect(m_root.get());
    return result;
}

void SessionModel::loadSessions(const QList<SessionConfig> &sessions,
                                const QMap<QString, QString> &folderPaths,
                                const QMap<QString, QString> &folderNames)
{
    beginResetModel();
    m_root->children.clear();

    QMap<QString, SessionNode *> folderMap;
    folderMap[QStringLiteral("root")] = m_root.get();

    // First pass: create every folder node, detached. Parents can appear
    // after children in id order, so two passes are required.
    for (auto it = folderPaths.cbegin(); it != folderPaths.cend(); ++it) {
        const QString folderId = it.key();
        auto *folderNode = new SessionNode(NodeType::Folder);
        folderNode->id = folderId;
        folderNode->name = folderNames.value(folderId, folderId);
        folderMap[folderId] = folderNode;
    }

    // Second pass: attach folders to their parents (fall back to root when
    // the parent is missing from the map).
    for (auto it = folderPaths.cbegin(); it != folderPaths.cend(); ++it) {
        SessionNode *folderNode = folderMap.value(it.key());
        SessionNode *parentNode = folderMap.value(it.value(), m_root.get());
        folderNode->parent = parentNode;
        parentNode->children.append(folderNode);
    }

    // Second pass: create sessions
    for (const SessionConfig &config : sessions) {
        const QString folderId = config.group();
        SessionNode *parentNode = folderMap.value(folderId, m_root.get());

        auto *sessionNode = new SessionNode(NodeType::Session, parentNode);
        sessionNode->id = config.id();
        sessionNode->name = config.name();
        sessionNode->config = config;
        parentNode->children.append(sessionNode);
    }

    endResetModel();
}

SessionNode *SessionModel::nodeFromIndex(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return m_root.get();
    }
    return static_cast<SessionNode *>(index.internalPointer());
}

} // namespace hssh
