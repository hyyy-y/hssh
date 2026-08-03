#include "TextDiff.h"

#include <QRegularExpression>

namespace hssh {

namespace {
constexpr qsizetype kMaxLinesForExactDiff = 20000;
}

QStringList splitIntoLines(const QString &text)
{
    return text.split(QRegularExpression(QStringLiteral("\r\n|\r|\n")));
}

QList<DiffLine> diffLines(const QStringList &oldLines, const QStringList &newLines)
{
    const qsizetype n = oldLines.size();
    const qsizetype m = newLines.size();

    // Guard the O(n*m) table; huge files get a coarse whole-file result.
    if (n > kMaxLinesForExactDiff || m > kMaxLinesForExactDiff || n * m > 4000000) {
        QList<DiffLine> result;
        result.reserve(n + m);
        for (const QString &line : oldLines) {
            result.append({DiffLine::Tag::Removed, line});
        }
        for (const QString &line : newLines) {
            result.append({DiffLine::Tag::Added, line});
        }
        return result;
    }

    // LCS length table, row-major, (n+1) x (m+1).
    std::vector<int> lcs(static_cast<size_t>((n + 1) * (m + 1)), 0);
    const auto at = [m, &lcs](qsizetype i, qsizetype j) -> int & {
        return lcs[static_cast<size_t>(i * (m + 1) + j)];
    };
    for (qsizetype i = n - 1; i >= 0; --i) {
        for (qsizetype j = m - 1; j >= 0; --j) {
            if (oldLines.at(i) == newLines.at(j)) {
                at(i, j) = at(i + 1, j + 1) + 1;
            } else {
                at(i, j) = qMax(at(i + 1, j), at(i, j + 1));
            }
        }
    }

    // Walk the table to produce the alignment. On ties prefer Removed first
    // so deletions print above their replacement additions.
    QList<DiffLine> result;
    result.reserve(n + m);
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < n && j < m) {
        if (oldLines.at(i) == newLines.at(j)) {
            result.append({DiffLine::Tag::Equal, oldLines.at(i)});
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            result.append({DiffLine::Tag::Removed, oldLines.at(i)});
            ++i;
        } else {
            result.append({DiffLine::Tag::Added, newLines.at(j)});
            ++j;
        }
    }
    while (i < n) {
        result.append({DiffLine::Tag::Removed, oldLines.at(i)});
        ++i;
    }
    while (j < m) {
        result.append({DiffLine::Tag::Added, newLines.at(j)});
        ++j;
    }
    return result;
}

} // namespace hssh
