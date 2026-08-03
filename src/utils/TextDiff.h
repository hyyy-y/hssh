#ifndef HSSH_UTILS_TEXTDIFF_H
#define HSSH_UTILS_TEXTDIFF_H

#include <QList>
#include <QString>
#include <QStringList>

namespace hssh {

struct DiffLine {
    enum class Tag {
        Equal,   // present in both, same position in the alignment
        Removed, // only in the first (old/local) text
        Added,   // only in the second (new/remote) text
    };

    Tag tag = Tag::Equal;
    QString text;
};

// Line-based diff of two texts using an LCS dynamic program.
// Output is an interleaved alignment: Equal/Removed/Added lines in order.
// Inputs larger than ~20k combined lines fall back to a whole-text
// remove+add (the quadratic table would be too expensive).
[[nodiscard]] QList<DiffLine> diffLines(const QStringList &oldLines, const QStringList &newLines);

// Convenience: split a file's text into lines (any newline convention).
[[nodiscard]] QStringList splitIntoLines(const QString &text);

} // namespace hssh

#endif // HSSH_UTILS_TEXTDIFF_H
