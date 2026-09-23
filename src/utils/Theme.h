#ifndef HSSH_UTILS_THEME_H
#define HSSH_UTILS_THEME_H

class QApplication;
class QString;

namespace hssh {

// PH1-05: app-wide theme presets. "dark" (default): palette + detailed QSS;
// "light" and "highcontrast": palettes only. Applies palette + stylesheet
// to the application; safe to call at runtime to switch themes.
void applyTheme(QApplication &app, const QString &name);

} // namespace hssh

#endif // HSSH_UTILS_THEME_H
