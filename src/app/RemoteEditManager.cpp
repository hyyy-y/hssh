#include "RemoteEditManager.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

namespace hssh {

RemoteEditManager &RemoteEditManager::instance()
{
    static RemoteEditManager manager;
    return manager;
}

RemoteEditManager::RemoteEditManager(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_debounce(new QTimer(this))
{
    // Editors often save in bursts (format-on-save, autosave); a short quiet
    // period collapses them into one upload.
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(500);
    connect(m_debounce, &QTimer::timeout, this, &RemoteEditManager::flushDirty);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &RemoteEditManager::onFileChanged);
}

bool RemoteEditManager::startEdit(const QString &sessionKey, const QString &remotePath,
                                  const QString &localPath, quint32 permissions,
                                  QString *conflict)
{
    for (auto it = m_edits.constBegin(); it != m_edits.constEnd(); ++it) {
        if (it->sessionKey == sessionKey && it->remotePath == remotePath) {
            if (conflict) {
                *conflict = QStringLiteral("%1 %2").arg(sessionKey, remotePath);
            }
            return false;
        }
    }

    Edit edit;
    edit.sessionKey = sessionKey;
    edit.remotePath = remotePath;
    edit.permissions = permissions;
    m_edits.insert(localPath, edit);
    return true;
}

void RemoteEditManager::beginWatch(const QString &localPath)
{
    const auto it = m_edits.find(localPath);
    if (it == m_edits.end()) {
        return;
    }
    if (!it->watching) {
        m_watcher->addPath(localPath); // may silently fail until the file exists
        it->watching = true;
    }
    // The download that just finished must not look like an editor save.
    markSynced(localPath);
}

void RemoteEditManager::markSynced(const QString &localPath)
{
    const auto it = m_edits.find(localPath);
    if (it != m_edits.end()) {
        it->syncedMsec = QFileInfo(localPath).lastModified().toMSecsSinceEpoch();
        it->dirty = false;
    }
}

bool RemoteEditManager::isEdited(const QString &sessionKey, const QString &remotePath) const
{
    for (const Edit &edit : m_edits) {
        if (edit.sessionKey == sessionKey && edit.remotePath == remotePath) {
            return true;
        }
    }
    return false;
}

QString RemoteEditManager::localPathFor(const QString &sessionKey, const QString &remotePath) const
{
    for (auto it = m_edits.constBegin(); it != m_edits.constEnd(); ++it) {
        if (it->sessionKey == sessionKey && it->remotePath == remotePath) {
            return it.key();
        }
    }
    return QString();
}

void RemoteEditManager::endEdit(const QString &sessionKey, const QString &remotePath)
{
    const QString local = localPathFor(sessionKey, remotePath);
    if (local.isEmpty()) {
        return;
    }
    m_watcher->removePath(local);
    m_edits.remove(local);
}

void RemoteEditManager::endEditsForSession(const QString &sessionKey)
{
    QList<QString> drop;
    for (auto it = m_edits.constBegin(); it != m_edits.constEnd(); ++it) {
        if (it->sessionKey == sessionKey) {
            drop.append(it.key());
        }
    }
    for (const QString &local : drop) {
        m_watcher->removePath(local);
        m_edits.remove(local);
    }
}

void RemoteEditManager::onFileChanged(const QString &localPath)
{
    const auto it = m_edits.find(localPath);
    if (it == m_edits.end()) {
        return;
    }
    // Editors that save via rename (write temp, replace) make the watcher
    // drop the path — re-arm it every time.
    if (it->watching) {
        m_watcher->addPath(localPath);
    }
    it->dirty = true;
    m_debounce->start();
}

void RemoteEditManager::flushDirty()
{
    QList<QString> gone;
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        if (!it->dirty) {
            continue;
        }
        const QString local = it.key();
        const QFileInfo info(local);
        if (!info.exists()) {
            // Still missing after the debounce grace: a real deletion, not
            // the mid-flight moment of an atomic save.
            gone.append(local);
            continue;
        }
        it->dirty = false;
        if (info.lastModified().toMSecsSinceEpoch() != it->syncedMsec) {
            emit editSaved(it->sessionKey, it->remotePath, local, it->permissions);
        }
    }
    for (const QString &local : gone) {
        const Edit edit = m_edits.value(local);
        m_watcher->removePath(local);
        m_edits.remove(local);
        emit editGone(edit.sessionKey, edit.remotePath);
    }
}

} // namespace hssh
