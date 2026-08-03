#include "TransferRegistry.h"

namespace hssh {

TransferRegistry &TransferRegistry::instance()
{
    static TransferRegistry registry;
    return registry;
}

int TransferRegistry::beginTransfer(const QString &name, Direction direction, const QString &sessionName)
{
    Transfer transfer;
    transfer.id = m_nextId++;
    transfer.name = name;
    transfer.direction = direction;
    transfer.sessionName = sessionName;
    m_transfers.append(transfer);
    emit transferAdded(transfer.id);
    return transfer.id;
}

void TransferRegistry::updateProgress(int id, qint64 bytesDone, qint64 bytesTotal)
{
    for (Transfer &transfer : m_transfers) {
        if (transfer.id == id) {
            transfer.bytesDone = bytesDone;
            transfer.bytesTotal = bytesTotal;
            emit transferUpdated(id);
            return;
        }
    }
}

void TransferRegistry::finishTransfer(int id, bool ok, const QString &message)
{
    for (Transfer &transfer : m_transfers) {
        if (transfer.id == id) {
            transfer.finished = true;
            transfer.ok = ok;
            transfer.message = message;
            emit transferUpdated(id);
            return;
        }
    }
}

const TransferRegistry::Transfer *TransferRegistry::transfer(int id) const
{
    for (const Transfer &transfer : m_transfers) {
        if (transfer.id == id) {
            return &transfer;
        }
    }
    return nullptr;
}

} // namespace hssh
