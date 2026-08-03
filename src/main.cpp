#include <QApplication>
#include <QFile>
#include <QLibraryInfo>
#include <QLocale>
#include <QPalette>
#include <QTranslator>

#include "app/MainWindow.h"
#include "hssh/Version.h"

namespace {

void applyDarkTheme(QApplication &app)
{
    // Dark palette for native widgets that QSS does not fully cover
    // (dialogs, spin box buttons, disabled text, ...).
    QPalette palette;
    const QColor window(0x25, 0x25, 0x26);
    const QColor base(0x1b, 0x1b, 0x1c);
    const QColor text(0xcc, 0xcc, 0xcc);
    const QColor disabled(0x6a, 0x6a, 0x6a);
    const QColor accent(0x26, 0x4f, 0x78);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, QColor(0x23, 0x23, 0x24));
    palette.setColor(QPalette::ToolTipBase, QColor(0x2d, 0x2d, 0x30));
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(0x33, 0x33, 0x38));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Link, QColor(0x00, 0x7a, 0xcc));
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    app.setPalette(palette);

    QFile styleFile(QStringLiteral(":/styles/dark.qss"));
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }
}

bool loadApplicationTranslator(QApplication &app)
{
    const QString localeName = QLocale::system().name(); // e.g. "zh_CN", "en_US"
    const QString baseName = QStringLiteral("hssh_") + localeName;

    auto *translator = new QTranslator(&app);
    if (translator->load(baseName, QStringLiteral(":/i18n"))) {
        app.installTranslator(translator);
        return true;
    }

    // Try language-only fallback (e.g. "hssh_zh" for "zh_CN").
    const QString languageOnly = localeName.left(localeName.indexOf(QLatin1Char('_')));
    if (!languageOnly.isEmpty()) {
        const QString fallbackName = QStringLiteral("hssh_") + languageOnly;
        if (translator->load(fallbackName, QStringLiteral(":/i18n"))) {
            app.installTranslator(translator);
            return true;
        }
    }

    delete translator;
    return false;
}

void loadQtTranslator(QApplication &app)
{
    const QString localeName = QLocale::system().name();

    // Load Qt's own translations from the application directory (deployed by windeployqt).
    auto *qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(QStringLiteral("qt_") + localeName,
                              QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(qtTranslator);
        return;
    }

    // Fallback: load from the executable's directory/translations.
    const QString translationsDir = QCoreApplication::applicationDirPath() + QStringLiteral("/translations");
    if (qtTranslator->load(QStringLiteral("qt_") + localeName, translationsDir)) {
        app.installTranslator(qtTranslator);
        return;
    }

    delete qtTranslator;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    app.setApplicationName(QStringLiteral("hssh"));
    app.setOrganizationName(QStringLiteral("hssh-project"));
    app.setApplicationDisplayName(QStringLiteral("hssh - Modern SSH Client"));
    app.setApplicationVersion(QStringLiteral(HSSH_VERSION_STRING));

    loadQtTranslator(app);
    loadApplicationTranslator(app);
    applyDarkTheme(app);

    hssh::MainWindow window;
    window.show();

    return app.exec();
}
