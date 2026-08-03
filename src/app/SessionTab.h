#ifndef HSSH_APP_SESSIONTAB_H
#define HSSH_APP_SESSIONTAB_H

#include "core/SessionConfig.h"

#include <QWidget>

namespace hssh {

class SessionConfig;
class TerminalSession;

class SessionTab : public QWidget {
    Q_OBJECT

public:
    explicit SessionTab(const SessionConfig &config, QWidget *parent = nullptr);
    ~SessionTab() override;

    [[nodiscard]] static SessionTab *createLocal(const QString &shellType, QWidget *parent = nullptr);

    [[nodiscard]] SessionConfig config() const;

signals:
    void sizeChanged(int columns, int rows);

public slots:
    void connectSession();
    void disconnectSession();
    void runCommand(const QString &command);

private:
    void setupUi();

    SessionConfig m_config;
    TerminalSession *m_terminalSession = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_SESSIONTAB_H
