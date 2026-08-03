#ifndef HSSH_TERMINAL_SHELLPROCESS_H
#define HSSH_TERMINAL_SHELLPROCESS_H

#include <QObject>
#include <QSize>

namespace hssh {

class ShellProcess : public QObject {
    Q_OBJECT

public:
    explicit ShellProcess(QObject *parent = nullptr);
    ~ShellProcess() override;

    virtual bool start() = 0;
    virtual void write(const QByteArray &data) = 0;
    virtual void resize(int columns, int rows) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isRunning() const = 0;

signals:
    void dataReceived(const QByteArray &data);
    void finished(int exitCode);
    void errorOccurred(const QString &message);

protected:
    QSize m_size{80, 24};
};

} // namespace hssh

#endif // HSSH_TERMINAL_SHELLPROCESS_H
