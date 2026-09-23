#include "utils/Theme.h"

#include <QApplication>
#include <QFile>
#include <QPalette>

namespace hssh {

void applyTheme(QApplication &app, const QString &name)
{
    if (name == QLatin1String("light")) {
        app.setStyleSheet(QString());
        QPalette palette;
        const QColor window(0xf5, 0xf5, 0xf5);
        const QColor base(0xff, 0xff, 0xff);
        const QColor text(0x1a, 0x1a, 0x1a);
        const QColor disabled(0x9a, 0x9a, 0x9a);
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, QColor(0xee, 0xee, 0xee));
        palette.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xff));
        palette.setColor(QPalette::ToolTipText, text);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, QColor(0xe6, 0xe6, 0xe6));
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::BrightText, Qt::darkRed);
        palette.setColor(QPalette::Link, QColor(0x00, 0x66, 0xcc));
        palette.setColor(QPalette::Highlight, QColor(0x00, 0x78, 0xd4));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::PlaceholderText, disabled);
        palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
        app.setPalette(palette);
        return;
    }
    if (name == QLatin1String("highcontrast")) {
        app.setStyleSheet(QString());
        QPalette palette;
        palette.setColor(QPalette::Window, Qt::black);
        palette.setColor(QPalette::WindowText, Qt::white);
        palette.setColor(QPalette::Base, Qt::black);
        palette.setColor(QPalette::AlternateBase, QColor(0x10, 0x10, 0x10));
        palette.setColor(QPalette::ToolTipBase, Qt::black);
        palette.setColor(QPalette::ToolTipText, Qt::white);
        palette.setColor(QPalette::Text, Qt::white);
        palette.setColor(QPalette::Button, QColor(0x20, 0x20, 0x20));
        palette.setColor(QPalette::ButtonText, Qt::white);
        palette.setColor(QPalette::BrightText, QColor(0xff, 0xd7, 0x00));
        palette.setColor(QPalette::Link, QColor(0x00, 0xbf, 0xff));
        palette.setColor(QPalette::Highlight, QColor(0xff, 0xd7, 0x00));
        palette.setColor(QPalette::HighlightedText, Qt::black);
        palette.setColor(QPalette::PlaceholderText, QColor(0xa0, 0xa0, 0xa0));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x80, 0x80, 0x80));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x80, 0x80, 0x80));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x80, 0x80, 0x80));
        app.setPalette(palette);
        return;
    }
    // "dark" (default): palette + detailed QSS.
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

} // namespace hssh

