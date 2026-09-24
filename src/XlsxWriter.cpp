#include "XlsxWriter.h"

#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

// --- XML helpers ------------------------------------------------------------

QString escape(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&':
            out += QLatin1String("&amp;");
            break;
        case '<':
            out += QLatin1String("&lt;");
            break;
        case '>':
            out += QLatin1String("&gt;");
            break;
        case '"':
            out += QLatin1String("&quot;");
            break;
        default:
            // Control characters are not allowed in XML 1.0.
            if (c.unicode() >= 0x20 || c == QLatin1Char('\t') || c == QLatin1Char('\n') || c == QLatin1Char('\r'))
                out += c;
        }
    }
    return out;
}

QString number(double value)
{
    return QString::number(value, 'g', 15);
}

QString rgb(const XlsxWriter::Color &color)
{
    return QStringLiteral("%1").arg(color.rgb, 6, 16, QLatin1Char('0')).toUpper();
}

QString argb(const XlsxWriter::Color &color)
{
    return QStringLiteral("FF") + rgb(color);
}

const char *const kXmlHeader = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
const char *const kMainNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
const char *const kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char *const kPackageRelNs = "http://schemas.openxmlformats.org/package/2006/relationships";

// Days since 1899-12-30, the epoch of Excel's date serial numbers.
double excelSerial(const QDateTime &dateTime)
{
    const qint64 days = QDate(1899, 12, 30).daysTo(dateTime.date());
    return double(days) + double(dateTime.time().msecsSinceStartOfDay()) / 86'400'000.0;
}

// --- ZIP container ----------------------------------------------------------

quint32 crc32(const QByteArray &data)
{
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    quint32 crc = 0xFFFFFFFFu;
    for (const char byte : data)
        crc = table[(crc ^ quint8(byte)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

class ZipWriter
{
public:
    void add(const QString &name, const QByteArray &data) { m_files.append({name, data}); }

    QByteArray finish()
    {
        // By convention the content types come first; some readers rely on it.
        std::stable_partition(m_files.begin(), m_files.end(), [](const auto &file) {
            return file.first == QLatin1String("[Content_Types].xml");
        });
        for (const auto &[name, data] : std::as_const(m_files))
            write(name, data);
        return writeDirectory();
    }

private:
    void write(const QString &name, const QByteArray &data)
    {
        // qCompress() yields a 4 byte size prefix followed by a zlib stream
        // (2 byte header, raw deflate data, 4 byte Adler-32). ZIP wants the
        // raw deflate data only.
        const QByteArray zlib = qCompress(data, 9);
        const QByteArray deflated = zlib.mid(6, zlib.size() - 10);

        Entry entry;
        entry.name = name.toUtf8();
        entry.crc = crc32(data);
        entry.size = quint32(data.size());
        entry.compressedSize = quint32(deflated.size());
        entry.offset = quint32(m_data.size());

        put32(m_data, 0x04034b50);
        put16(m_data, 20);     // version needed: 2.0 (deflate)
        put16(m_data, 0x0800); // UTF-8 file names
        put16(m_data, 8);      // deflate
        put16(m_data, kDosTime);
        put16(m_data, kDosDate);
        put32(m_data, entry.crc);
        put32(m_data, entry.compressedSize);
        put32(m_data, entry.size);
        put16(m_data, quint16(entry.name.size()));
        put16(m_data, 0);
        m_data += entry.name;
        m_data += deflated;
        m_entries.append(entry);
    }

    QByteArray writeDirectory()
    {
        const quint32 directoryOffset = quint32(m_data.size());
        for (const Entry &entry : std::as_const(m_entries)) {
            put32(m_data, 0x02014b50);
            put16(m_data, 20); // made by
            put16(m_data, 20); // needed
            put16(m_data, 0x0800);
            put16(m_data, 8);
            put16(m_data, kDosTime);
            put16(m_data, kDosDate);
            put32(m_data, entry.crc);
            put32(m_data, entry.compressedSize);
            put32(m_data, entry.size);
            put16(m_data, quint16(entry.name.size()));
            put16(m_data, 0); // extra
            put16(m_data, 0); // comment
            put16(m_data, 0); // disk
            put16(m_data, 0); // internal attributes
            put32(m_data, 0); // external attributes
            put32(m_data, entry.offset);
            m_data += entry.name;
        }
        const quint32 directorySize = quint32(m_data.size()) - directoryOffset;
        put32(m_data, 0x06054b50);
        put16(m_data, 0);
        put16(m_data, 0);
        put16(m_data, quint16(m_entries.size()));
        put16(m_data, quint16(m_entries.size()));
        put32(m_data, directorySize);
        put32(m_data, directoryOffset);
        put16(m_data, 0);
        return m_data;
    }

    struct Entry
    {
        QByteArray name;
        quint32 crc = 0;
        quint32 size = 0;
        quint32 compressedSize = 0;
        quint32 offset = 0;
    };

    // 1980-01-01 00:00; the real time is in docProps/core.xml.
    static constexpr quint16 kDosTime = 0;
    static constexpr quint16 kDosDate = (0 << 9) | (1 << 5) | 1;

    static void put16(QByteArray &out, quint16 value)
    {
        out.append(char(value & 0xFF));
        out.append(char(value >> 8));
    }
    static void put32(QByteArray &out, quint32 value)
    {
        put16(out, quint16(value & 0xFFFF));
        put16(out, quint16(value >> 16));
    }

    QList<QPair<QString, QByteArray>> m_files;
    QByteArray m_data;
    QList<Entry> m_entries;
};

// Built-in number formats Excel knows without declaring them.
int builtinNumberFormat(const QString &code)
{
    if (code.isEmpty())
        return 0;
    if (code == QLatin1String("0"))
        return 1;
    if (code == QLatin1String("0.00"))
        return 2;
    if (code == QLatin1String("#,##0"))
        return 3;
    if (code == QLatin1String("#,##0.00"))
        return 4;
    if (code == QLatin1String("0%"))
        return 9;
    if (code == QLatin1String("0.00%"))
        return 10;
    return -1;
}

QString richText(const QString &text, int size, bool bold, bool rotated = false)
{
    return QStringLiteral("<c:tx><c:rich><a:bodyPr%1/><a:lstStyle/><a:p><a:pPr><a:defRPr sz=\"%2\" b=\"%3\"/></a:pPr>"
                          "<a:r><a:rPr lang=\"de-DE\" sz=\"%2\" b=\"%3\"/><a:t>%4</a:t></a:r></a:p></c:rich></c:tx>")
        .arg(rotated ? QStringLiteral(" rot=\"-5400000\" vert=\"horz\"") : QString())
        .arg(size)
        .arg(bold ? 1 : 0)
        .arg(escape(text));
}

QString solidLine(const XlsxWriter::Color &color, int width)
{
    return QStringLiteral("<a:ln w=\"%1\" cap=\"rnd\"><a:solidFill><a:srgbClr val=\"%2\"/></a:solidFill><a:round/></a:ln>")
        .arg(width)
        .arg(rgb(color));
}

} // namespace

// --- Style ------------------------------------------------------------------

bool XlsxWriter::Style::operator==(const Style &o) const
{
    return numberFormat == o.numberFormat && bold == o.bold && italic == o.italic && fontSize == o.fontSize &&
           fontColor == o.fontColor && fill == o.fill && borders == o.borders && borderColor == o.borderColor &&
           align == o.align && wrap == o.wrap;
}

// --- Sheet ------------------------------------------------------------------

XlsxWriter::Sheet::Sheet(XlsxWriter *writer, const QString &name)
    : m_writer(writer)
    , m_name(name)
{
}

void XlsxWriter::Sheet::set(int row, int column, Cell cell)
{
    m_rows[row][column] = std::move(cell);
}

void XlsxWriter::Sheet::setText(int row, int column, const QString &text, const Style &style)
{
    Cell cell;
    cell.type = Cell::Text;
    cell.text = text;
    cell.style = m_writer->styleIndex(style);
    set(row, column, cell);
}

void XlsxWriter::Sheet::setNumber(int row, int column, double value, const Style &style)
{
    Cell cell;
    cell.type = std::isfinite(value) ? Cell::Number : Cell::Blank;
    cell.number = value;
    cell.style = m_writer->styleIndex(style);
    set(row, column, cell);
}

void XlsxWriter::Sheet::setDateTime(int row, int column, const QDateTime &value, const Style &style)
{
    setNumber(row, column, excelSerial(value), style);
}

void XlsxWriter::Sheet::setFormula(int row, int column, const QString &formula, double cachedValue,
                                   const Style &style)
{
    Cell cell;
    cell.type = Cell::Formula;
    cell.text = formula;
    cell.number = cachedValue;
    cell.style = m_writer->styleIndex(style);
    set(row, column, cell);
}

void XlsxWriter::Sheet::setStyle(int row, int column, const Style &style)
{
    Cell cell;
    cell.style = m_writer->styleIndex(style);
    set(row, column, cell);
}

void XlsxWriter::Sheet::setColumnWidth(int column, double characters)
{
    m_columnWidths[column] = characters;
}

void XlsxWriter::Sheet::setRowHeight(int row, double points)
{
    m_rowHeights[row] = points;
}

void XlsxWriter::Sheet::mergeCells(int row, int column, int lastRow, int lastColumn)
{
    m_merged << cellRef(row, column) + QLatin1Char(':') + cellRef(lastRow, lastColumn);
}

void XlsxWriter::Sheet::freezeRows(int rows)
{
    m_frozenRows = rows;
}

void XlsxWriter::Sheet::setAutoFilter(int row, int column, int lastRow, int lastColumn)
{
    m_autoFilter = cellRef(row, column) + QLatin1Char(':') + cellRef(lastRow, lastColumn);
}

QString XlsxWriter::Sheet::cellText(int row, int column) const
{
    const auto r = m_rows.constFind(row);
    if (r == m_rows.constEnd())
        return {};
    const auto c = r->constFind(column);
    if (c == r->constEnd())
        return {};
    return c->type == Cell::Text ? c->text : c->type == Cell::Blank ? QString() : number(c->number);
}

double XlsxWriter::Sheet::cellNumber(int row, int column, bool *ok) const
{
    *ok = false;
    const auto r = m_rows.constFind(row);
    if (r == m_rows.constEnd())
        return 0.0;
    const auto c = r->constFind(column);
    if (c == r->constEnd() || (c->type != Cell::Number && c->type != Cell::Formula))
        return 0.0;
    *ok = true;
    return c->number;
}

QByteArray XlsxWriter::Sheet::xml(int drawingRelId) const
{
    QString x = QString::fromLatin1(kXmlHeader);
    x += QStringLiteral("<worksheet xmlns=\"%1\" xmlns:r=\"%2\">").arg(QLatin1String(kMainNs), QLatin1String(kRelNs));

    x += QStringLiteral("<sheetPr>");
    if (m_tabColor.isValid())
        x += QStringLiteral("<tabColor rgb=\"%1\"/>").arg(argb(m_tabColor));
    x += QStringLiteral("<pageSetUpPr fitToPage=\"1\"/></sheetPr>");

    x += QStringLiteral("<sheetViews><sheetView workbookViewId=\"0\"%1>")
             .arg(m_showGridLines ? QString() : QStringLiteral(" showGridLines=\"0\""));
    if (m_frozenRows > 0) {
        x += QStringLiteral("<pane ySplit=\"%1\" topLeftCell=\"%2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
                            "<selection pane=\"bottomLeft\" activeCell=\"%2\" sqref=\"%2\"/>")
                 .arg(m_frozenRows)
                 .arg(cellRef(m_frozenRows, 0));
    }
    x += QStringLiteral("</sheetView></sheetViews>");
    x += QStringLiteral("<sheetFormatPr defaultRowHeight=\"15\"/>");

    if (!m_columnWidths.isEmpty()) {
        x += QStringLiteral("<cols>");
        for (auto it = m_columnWidths.constBegin(); it != m_columnWidths.constEnd(); ++it) {
            x += QStringLiteral("<col min=\"%1\" max=\"%1\" width=\"%2\" customWidth=\"1\"/>")
                     .arg(it.key() + 1)
                     .arg(number(it.value()));
        }
        x += QStringLiteral("</cols>");
    }

    // Rows that only have a custom height still need a <row> element.
    QList<int> rows = m_rows.keys();
    for (auto it = m_rowHeights.constBegin(); it != m_rowHeights.constEnd(); ++it) {
        if (!m_rows.contains(it.key()))
            rows.append(it.key());
    }
    std::sort(rows.begin(), rows.end());

    x += QStringLiteral("<sheetData>");
    for (const int row : std::as_const(rows)) {
        x += QStringLiteral("<row r=\"%1\"").arg(row + 1);
        if (m_rowHeights.contains(row))
            x += QStringLiteral(" ht=\"%1\" customHeight=\"1\"").arg(number(m_rowHeights.value(row)));
        x += QLatin1Char('>');
        const QMap<int, Cell> cells = m_rows.value(row);
        for (auto it = cells.constBegin(); it != cells.constEnd(); ++it) {
            const Cell &cell = it.value();
            const QString ref = cellRef(row, it.key());
            const QString style = cell.style ? QStringLiteral(" s=\"%1\"").arg(cell.style) : QString();
            switch (cell.type) {
            case Cell::Blank:
                x += QStringLiteral("<c r=\"%1\"%2/>").arg(ref, style);
                break;
            case Cell::Number:
                x += QStringLiteral("<c r=\"%1\"%2><v>%3</v></c>").arg(ref, style, number(cell.number));
                break;
            case Cell::Text:
                x += QStringLiteral("<c r=\"%1\"%2 t=\"inlineStr\"><is><t xml:space=\"preserve\">%3</t></is></c>")
                         .arg(ref, style, escape(cell.text));
                break;
            case Cell::Formula:
                x += QStringLiteral("<c r=\"%1\"%2><f>%3</f>").arg(ref, style, escape(cell.text));
                if (std::isfinite(cell.number))
                    x += QStringLiteral("<v>%1</v>").arg(number(cell.number));
                x += QStringLiteral("</c>");
                break;
            }
        }
        x += QStringLiteral("</row>");
    }
    x += QStringLiteral("</sheetData>");

    if (!m_autoFilter.isEmpty())
        x += QStringLiteral("<autoFilter ref=\"%1\"/>").arg(m_autoFilter);
    if (!m_merged.isEmpty()) {
        x += QStringLiteral("<mergeCells count=\"%1\">").arg(m_merged.size());
        for (const QString &range : m_merged)
            x += QStringLiteral("<mergeCell ref=\"%1\"/>").arg(range);
        x += QStringLiteral("</mergeCells>");
    }
    x += QStringLiteral("<pageMargins left=\"0.5\" right=\"0.5\" top=\"0.6\" bottom=\"0.6\" header=\"0.3\" footer=\"0.3\"/>");
    x += QStringLiteral("<pageSetup paperSize=\"9\" orientation=\"%1\" fitToWidth=\"1\" fitToHeight=\"0\"/>")
             .arg(m_landscape ? QStringLiteral("landscape") : QStringLiteral("portrait"));
    if (drawingRelId > 0)
        x += QStringLiteral("<drawing r:id=\"rId%1\"/>").arg(drawingRelId);
    x += QStringLiteral("</worksheet>");
    return x.toUtf8();
}

// --- Writer -----------------------------------------------------------------

XlsxWriter::XlsxWriter()
{
    m_styles.append(Style{}); // index 0: default
}

XlsxWriter::~XlsxWriter() = default;

XlsxWriter::Sheet &XlsxWriter::addSheet(const QString &name)
{
    QString valid = name;
    for (const QChar c : QStringLiteral("[]:*?/\\"))
        valid.replace(c, QLatin1Char('_'));
    valid = valid.left(31);
    if (valid.isEmpty())
        valid = QStringLiteral("Sheet%1").arg(m_sheets.size() + 1);
    m_sheets.emplace_back(new Sheet(this, valid));
    return *m_sheets.back();
}

int XlsxWriter::styleIndex(const Style &style)
{
    const qsizetype index = m_styles.indexOf(style);
    if (index >= 0)
        return int(index);
    m_styles.append(style);
    return int(m_styles.size() - 1);
}

QString XlsxWriter::columnName(int column)
{
    QString name;
    for (int c = column + 1; c > 0; c = (c - 1) / 26)
        name.prepend(QLatin1Char(char('A' + (c - 1) % 26)));
    return name;
}

QString XlsxWriter::cellRef(int row, int column, bool absolute)
{
    const QString dollar = absolute ? QStringLiteral("$") : QString();
    return dollar + columnName(column) + dollar + QString::number(row + 1);
}

QByteArray XlsxWriter::stylesXml() const
{
    QStringList numFmts; // custom format codes, id = 164 + index
    QStringList fonts;
    QStringList fills{QStringLiteral("<fill><patternFill patternType=\"none\"/></fill>"),
                      QStringLiteral("<fill><patternFill patternType=\"gray125\"/></fill>")};
    QStringList borders{QStringLiteral("<border><left/><right/><top/><bottom/><diagonal/></border>")};
    QStringList xfs;

    auto indexOf = [](QStringList &list, const QString &item) {
        qsizetype i = list.indexOf(item);
        if (i < 0) {
            list.append(item);
            i = list.size() - 1;
        }
        return int(i);
    };

    for (const Style &s : m_styles) {
        int numFmtId = builtinNumberFormat(s.numberFormat);
        if (numFmtId < 0)
            numFmtId = 164 + indexOf(numFmts, s.numberFormat);

        QString font = QStringLiteral("<font>");
        if (s.bold)
            font += QStringLiteral("<b/>");
        if (s.italic)
            font += QStringLiteral("<i/>");
        font += QStringLiteral("<sz val=\"%1\"/>").arg(number(s.fontSize));
        if (s.fontColor.isValid())
            font += QStringLiteral("<color rgb=\"%1\"/>").arg(argb(s.fontColor));
        font += QStringLiteral("<name val=\"Calibri\"/><family val=\"2\"/></font>");
        const int fontId = indexOf(fonts, font);

        int fillId = 0;
        if (s.fill.isValid()) {
            fillId = indexOf(fills, QStringLiteral("<fill><patternFill patternType=\"solid\"><fgColor rgb=\"%1\"/>"
                                                   "<bgColor indexed=\"64\"/></patternFill></fill>")
                                        .arg(argb(s.fill)));
        }

        int borderId = 0;
        if (s.borders != Style::NoBorder) {
            const QString color = s.borderColor.isValid()
                                      ? QStringLiteral("<color rgb=\"%1\"/>").arg(argb(s.borderColor))
                                      : QStringLiteral("<color auto=\"1\"/>");
            auto side = [&](const char *name, int flag) {
                return (s.borders & flag) ? QStringLiteral("<%1 style=\"thin\">%2</%1>").arg(QLatin1String(name), color)
                                          : QStringLiteral("<%1/>").arg(QLatin1String(name));
            };
            borderId = indexOf(borders, QStringLiteral("<border>") + side("left", Style::BorderLeft) +
                                            side("right", Style::BorderRight) + side("top", Style::BorderTop) +
                                            side("bottom", Style::BorderBottom) + QStringLiteral("<diagonal/></border>"));
        }

        QString xf = QStringLiteral("<xf numFmtId=\"%1\" fontId=\"%2\" fillId=\"%3\" borderId=\"%4\" xfId=\"0\"")
                         .arg(numFmtId)
                         .arg(fontId)
                         .arg(fillId)
                         .arg(borderId);
        if (numFmtId)
            xf += QStringLiteral(" applyNumberFormat=\"1\"");
        xf += QStringLiteral(" applyFont=\"1\"");
        if (fillId)
            xf += QStringLiteral(" applyFill=\"1\"");
        if (borderId)
            xf += QStringLiteral(" applyBorder=\"1\"");
        if (s.align != Style::General || s.wrap) {
            static const char *const horizontal[] = {"general", "left", "center", "right"};
            xf += QStringLiteral(" applyAlignment=\"1\"><alignment horizontal=\"%1\" vertical=\"center\"%2/></xf>")
                      .arg(QLatin1String(horizontal[s.align]),
                           s.wrap ? QStringLiteral(" wrapText=\"1\"") : QString());
        } else {
            xf += QStringLiteral("/>");
        }
        xfs.append(xf);
    }

    QString x = QString::fromLatin1(kXmlHeader);
    x += QStringLiteral("<styleSheet xmlns=\"%1\">").arg(QLatin1String(kMainNs));
    if (!numFmts.isEmpty()) {
        x += QStringLiteral("<numFmts count=\"%1\">").arg(numFmts.size());
        for (qsizetype i = 0; i < numFmts.size(); ++i)
            x += QStringLiteral("<numFmt numFmtId=\"%1\" formatCode=\"%2\"/>").arg(164 + i).arg(escape(numFmts[i]));
        x += QStringLiteral("</numFmts>");
    }
    x += QStringLiteral("<fonts count=\"%1\">%2</fonts>").arg(fonts.size()).arg(fonts.join(QString()));
    x += QStringLiteral("<fills count=\"%1\">%2</fills>").arg(fills.size()).arg(fills.join(QString()));
    x += QStringLiteral("<borders count=\"%1\">%2</borders>").arg(borders.size()).arg(borders.join(QString()));
    x += QStringLiteral("<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>");
    x += QStringLiteral("<cellXfs count=\"%1\">%2</cellXfs>").arg(xfs.size()).arg(xfs.join(QString()));
    x += QStringLiteral("<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>");
    x += QStringLiteral("</styleSheet>");
    return x.toUtf8();
}

QByteArray XlsxWriter::workbookXml() const
{
    QString x = QString::fromLatin1(kXmlHeader);
    x += QStringLiteral("<workbook xmlns=\"%1\" xmlns:r=\"%2\">").arg(QLatin1String(kMainNs), QLatin1String(kRelNs));
    x += QStringLiteral("<bookViews><workbookView activeTab=\"0\"/></bookViews><sheets>");
    for (size_t i = 0; i < m_sheets.size(); ++i) {
        x += QStringLiteral("<sheet name=\"%1\" sheetId=\"%2\" r:id=\"rId%2\"/>")
                 .arg(escape(m_sheets[i]->name()))
                 .arg(i + 1);
    }
    x += QStringLiteral("</sheets>");

    QString names;
    for (size_t i = 0; i < m_sheets.size(); ++i) {
        const Sheet &sheet = *m_sheets[i];
        if (sheet.m_autoFilter.isEmpty())
            continue;
        const QStringList corners = sheet.m_autoFilter.split(QLatin1Char(':'));
        auto absolute = [](const QString &ref) {
            const qsizetype digit = ref.indexOf(QRegularExpression(QStringLiteral("[0-9]")));
            return QLatin1Char('$') + ref.left(digit) + QLatin1Char('$') + ref.mid(digit);
        };
        names += QStringLiteral("<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"%1\" hidden=\"1\">"
                                "'%2'!%3:%4</definedName>")
                     .arg(i)
                     .arg(escape(QString(sheet.name()).replace(QLatin1Char('\''), QStringLiteral("''"))),
                          absolute(corners.first()), absolute(corners.last()));
    }
    if (!names.isEmpty())
        x += QStringLiteral("<definedNames>%1</definedNames>").arg(names);

    x += QStringLiteral("<calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/></workbook>");
    return x.toUtf8();
}

QByteArray XlsxWriter::chartXml(const Sheet &sheet, const LineChart &chart) const
{
    const QString sheetRef = QLatin1Char('\'') + QString(sheet.name()).replace(QLatin1Char('\''), QStringLiteral("''")) +
                             QStringLiteral("'!");
    auto range = [&](int column, int first, int last) {
        return escape(sheetRef + cellRef(first, column, true) + QLatin1Char(':') + cellRef(last, column, true));
    };

    // Categories as strings so that e.g. "1×" or run numbers show as labels.
    QString categories = QStringLiteral("<c:cat><c:strRef><c:f>%1</c:f><c:strCache><c:ptCount val=\"%2\"/>")
                             .arg(range(chart.categoryColumn, chart.categoryFirstRow, chart.categoryLastRow))
                             .arg(chart.categoryLastRow - chart.categoryFirstRow + 1);
    for (int row = chart.categoryFirstRow; row <= chart.categoryLastRow; ++row) {
        categories += QStringLiteral("<c:pt idx=\"%1\"><c:v>%2</c:v></c:pt>")
                          .arg(row - chart.categoryFirstRow)
                          .arg(escape(sheet.cellText(row, chart.categoryColumn)));
    }
    categories += QStringLiteral("</c:strCache></c:strRef></c:cat>");

    QString x = QString::fromLatin1(kXmlHeader);
    x += QStringLiteral("<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
                        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"%1\">")
             .arg(QLatin1String(kRelNs));
    x += QStringLiteral("<c:roundedCorners val=\"0\"/><c:chart>");
    if (!chart.title.isEmpty())
        x += QStringLiteral("<c:title>%1<c:overlay val=\"0\"/></c:title>").arg(richText(chart.title, 1300, true));
    x += QStringLiteral("<c:autoTitleDeleted val=\"%1\"/>").arg(chart.title.isEmpty() ? 1 : 0);
    x += QStringLiteral("<c:plotArea><c:layout/><c:lineChart><c:grouping val=\"standard\"/><c:varyColors val=\"0\"/>");

    for (qsizetype i = 0; i < chart.series.size(); ++i) {
        const ChartSeries &s = chart.series[i];
        x += QStringLiteral("<c:ser><c:idx val=\"%1\"/><c:order val=\"%1\"/><c:tx><c:v>%2</c:v></c:tx>")
                 .arg(i)
                 .arg(escape(s.name));
        x += QStringLiteral("<c:spPr>%1</c:spPr>").arg(solidLine(s.color, 28575));
        x += QStringLiteral("<c:marker><c:symbol val=\"circle\"/><c:size val=\"6\"/><c:spPr><a:solidFill>"
                            "<a:srgbClr val=\"%1\"/></a:solidFill>%2</c:spPr></c:marker>")
                 .arg(rgb(s.color), solidLine(s.color, 9525));
        x += categories;
        x += QStringLiteral("<c:val><c:numRef><c:f>%1</c:f><c:numCache><c:formatCode>General</c:formatCode>"
                            "<c:ptCount val=\"%2\"/>")
                 .arg(range(s.column, s.firstRow, s.lastRow))
                 .arg(s.lastRow - s.firstRow + 1);
        for (int row = s.firstRow; row <= s.lastRow; ++row) {
            bool ok = false;
            const double value = sheet.cellNumber(row, s.column, &ok);
            if (ok)
                x += QStringLiteral("<c:pt idx=\"%1\"><c:v>%2</c:v></c:pt>").arg(row - s.firstRow).arg(number(value));
        }
        x += QStringLiteral("</c:numCache></c:numRef></c:val><c:smooth val=\"0\"/></c:ser>");
    }

    const QString gridColor = QStringLiteral("D9DDE3");
    const QString axisLine = QStringLiteral("<c:spPr><a:ln w=\"9525\"><a:solidFill><a:srgbClr val=\"%1\"/></a:solidFill>"
                                            "</a:ln></c:spPr>")
                                 .arg(gridColor);
    x += QStringLiteral("<c:marker val=\"1\"/><c:axId val=\"500000001\"/><c:axId val=\"500000002\"/></c:lineChart>");
    x += QStringLiteral("<c:catAx><c:axId val=\"500000001\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling>"
                        "<c:delete val=\"0\"/><c:axPos val=\"b\"/>");
    if (!chart.xTitle.isEmpty())
        x += QStringLiteral("<c:title>%1<c:overlay val=\"0\"/></c:title>").arg(richText(chart.xTitle, 1000, false));
    x += QStringLiteral("<c:numFmt formatCode=\"General\" sourceLinked=\"1\"/><c:majorTickMark val=\"out\"/>"
                        "<c:minorTickMark val=\"none\"/><c:tickLblPos val=\"low\"/>%1<c:crossAx val=\"500000002\"/>"
                        "<c:crosses val=\"autoZero\"/><c:auto val=\"1\"/><c:lblAlgn val=\"ctr\"/>"
                        "<c:lblOffset val=\"100\"/><c:noMultiLvlLbl val=\"0\"/></c:catAx>")
             .arg(axisLine);
    x += QStringLiteral("<c:valAx><c:axId val=\"500000002\"/><c:scaling><c:orientation val=\"minMax\"/>"
                        "<c:min val=\"0\"/></c:scaling><c:delete val=\"0\"/><c:axPos val=\"l\"/>"
                        "<c:majorGridlines>%1</c:majorGridlines>")
             .arg(axisLine);
    if (!chart.yTitle.isEmpty())
        x += QStringLiteral("<c:title>%1<c:overlay val=\"0\"/></c:title>").arg(richText(chart.yTitle, 1000, false, true));
    x += QStringLiteral("<c:numFmt formatCode=\"General\" sourceLinked=\"0\"/><c:majorTickMark val=\"none\"/>"
                        "<c:minorTickMark val=\"none\"/><c:tickLblPos val=\"nextTo\"/>"
                        "<c:spPr><a:ln><a:noFill/></a:ln></c:spPr><c:crossAx val=\"500000001\"/>"
                        "<c:crosses val=\"autoZero\"/><c:crossBetween val=\"between\"/></c:valAx>");
    x += QStringLiteral("</c:plotArea><c:legend><c:legendPos val=\"b\"/><c:overlay val=\"0\"/></c:legend>"
                        "<c:plotVisOnly val=\"1\"/><c:dispBlanksAs val=\"gap\"/></c:chart>"
                        "<c:spPr><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill>"
                        "<a:ln w=\"9525\"><a:solidFill><a:srgbClr val=\"%1\"/></a:solidFill></a:ln></c:spPr>"
                        "</c:chartSpace>")
             .arg(gridColor);
    return x.toUtf8();
}

QByteArray XlsxWriter::toByteArray() const
{
    ZipWriter zip;
    for (const auto &[name, data] : files())
        zip.add(name, data);
    return zip.finish();
}

QList<QPair<QString, QByteArray>> XlsxWriter::files() const
{
    struct
    {
        QList<QPair<QString, QByteArray>> files;
        void add(const QString &name, const QByteArray &data) { files.append({name, data}); }
    } zip;
    const QString header = QString::fromLatin1(kXmlHeader);

    QString contentTypes =
        header +
        QStringLiteral("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                       "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                       "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                       "<Override PartName=\"/xl/workbook.xml\" "
                       "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                       "<Override PartName=\"/xl/styles.xml\" "
                       "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
                       "<Override PartName=\"/docProps/core.xml\" "
                       "ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
                       "<Override PartName=\"/docProps/app.xml\" "
                       "ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>");

    int drawingCount = 0;
    int chartCount = 0;
    for (size_t i = 0; i < m_sheets.size(); ++i) {
        const Sheet &sheet = *m_sheets[i];
        const int sheetNumber = int(i + 1);
        contentTypes += QStringLiteral("<Override PartName=\"/xl/worksheets/sheet%1.xml\" "
                                       "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml."
                                       "worksheet+xml\"/>")
                            .arg(sheetNumber);

        if (sheet.m_charts.isEmpty()) {
            zip.add(QStringLiteral("xl/worksheets/sheet%1.xml").arg(sheetNumber), sheet.xml(0));
            continue;
        }

        const int drawing = ++drawingCount;
        zip.add(QStringLiteral("xl/worksheets/sheet%1.xml").arg(sheetNumber), sheet.xml(1));
        zip.add(QStringLiteral("xl/worksheets/_rels/sheet%1.xml.rels").arg(sheetNumber),
                (header + QStringLiteral("<Relationships xmlns=\"%1\"><Relationship Id=\"rId1\" "
                                         "Type=\"%2/drawing\" Target=\"../drawings/drawing%3.xml\"/></Relationships>")
                              .arg(QLatin1String(kPackageRelNs), QLatin1String(kRelNs))
                              .arg(drawing))
                    .toUtf8());
        contentTypes += QStringLiteral("<Override PartName=\"/xl/drawings/drawing%1.xml\" "
                                       "ContentType=\"application/vnd.openxmlformats-officedocument.drawing+xml\"/>")
                            .arg(drawing);

        QString drawingXml =
            header + QStringLiteral("<xdr:wsDr xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/"
                                    "spreadsheetDrawing\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/"
                                    "main\">");
        QString drawingRels = header + QStringLiteral("<Relationships xmlns=\"%1\">").arg(QLatin1String(kPackageRelNs));
        for (qsizetype c = 0; c < sheet.m_charts.size(); ++c) {
            const LineChart &chart = sheet.m_charts[c];
            const int chartNumber = ++chartCount;
            zip.add(QStringLiteral("xl/charts/chart%1.xml").arg(chartNumber), chartXml(sheet, chart));
            contentTypes += QStringLiteral("<Override PartName=\"/xl/charts/chart%1.xml\" "
                                           "ContentType=\"application/vnd.openxmlformats-officedocument.drawingml."
                                           "chart+xml\"/>")
                                .arg(chartNumber);
            drawingRels += QStringLiteral("<Relationship Id=\"rId%1\" Type=\"%2/chart\" Target=\"../charts/chart%3.xml\"/>")
                               .arg(c + 1)
                               .arg(QLatin1String(kRelNs))
                               .arg(chartNumber);
            drawingXml +=
                QStringLiteral(
                    "<xdr:twoCellAnchor editAs=\"oneCell\">"
                    "<xdr:from><xdr:col>%1</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>%2</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>"
                    "<xdr:to><xdr:col>%3</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>%4</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:to>"
                    "<xdr:graphicFrame macro=\"\"><xdr:nvGraphicFramePr><xdr:cNvPr id=\"%5\" name=\"Diagramm %6\"/>"
                    "<xdr:cNvGraphicFramePr/></xdr:nvGraphicFramePr><xdr:xfrm><a:off x=\"0\" y=\"0\"/>"
                    "<a:ext cx=\"0\" cy=\"0\"/></xdr:xfrm><a:graphic>"
                    "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">"
                    "<c:chart xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" xmlns:r=\"%7\" "
                    "r:id=\"rId%6\"/></a:graphicData></a:graphic></xdr:graphicFrame><xdr:clientData/>"
                    "</xdr:twoCellAnchor>")
                    .arg(chart.fromColumn)
                    .arg(chart.fromRow)
                    .arg(chart.toColumn)
                    .arg(chart.toRow)
                    .arg(c + 2)
                    .arg(c + 1)
                    .arg(QLatin1String(kRelNs));
        }
        drawingXml += QStringLiteral("</xdr:wsDr>");
        drawingRels += QStringLiteral("</Relationships>");
        zip.add(QStringLiteral("xl/drawings/drawing%1.xml").arg(drawing), drawingXml.toUtf8());
        zip.add(QStringLiteral("xl/drawings/_rels/drawing%1.xml.rels").arg(drawing), drawingRels.toUtf8());
    }
    contentTypes += QStringLiteral("</Types>");

    QString rootRels = header + QStringLiteral("<Relationships xmlns=\"%1\">"
                                               "<Relationship Id=\"rId1\" Type=\"%2/officeDocument\" Target=\"xl/workbook.xml\"/>"
                                               "<Relationship Id=\"rId2\" Type=\"%3/metadata/core-properties\" "
                                               "Target=\"docProps/core.xml\"/>"
                                               "<Relationship Id=\"rId3\" Type=\"%2/extended-properties\" "
                                               "Target=\"docProps/app.xml\"/></Relationships>")
                                    .arg(QLatin1String(kPackageRelNs), QLatin1String(kRelNs),
                                         QStringLiteral("http://schemas.openxmlformats.org/package/2006/relationships"));

    QString workbookRels = header + QStringLiteral("<Relationships xmlns=\"%1\">").arg(QLatin1String(kPackageRelNs));
    for (size_t i = 0; i < m_sheets.size(); ++i) {
        workbookRels += QStringLiteral("<Relationship Id=\"rId%1\" Type=\"%2/worksheet\" Target=\"worksheets/sheet%1.xml\"/>")
                            .arg(i + 1)
                            .arg(QLatin1String(kRelNs));
    }
    workbookRels += QStringLiteral("<Relationship Id=\"rId%1\" Type=\"%2/styles\" Target=\"styles.xml\"/></Relationships>")
                        .arg(m_sheets.size() + 1)
                        .arg(QLatin1String(kRelNs));

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QString core = header + QStringLiteral("<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/"
                                           "metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                                           "xmlns:dcterms=\"http://purl.org/dc/terms/\" "
                                           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
                                           "<dc:title>%1</dc:title><dc:creator>%2</dc:creator>"
                                           "<dcterms:created xsi:type=\"dcterms:W3CDTF\">%3</dcterms:created>"
                                           "<dcterms:modified xsi:type=\"dcterms:W3CDTF\">%3</dcterms:modified>"
                                           "</cp:coreProperties>")
                                .arg(escape(m_title), escape(m_creator), now);
    QString app = header + QStringLiteral("<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                                          "extended-properties\"><Application>%1</Application></Properties>")
                               .arg(escape(m_creator));

    zip.add(QStringLiteral("[Content_Types].xml"), contentTypes.toUtf8());
    zip.add(QStringLiteral("_rels/.rels"), rootRels.toUtf8());
    zip.add(QStringLiteral("docProps/core.xml"), core.toUtf8());
    zip.add(QStringLiteral("docProps/app.xml"), app.toUtf8());
    zip.add(QStringLiteral("xl/workbook.xml"), workbookXml());
    zip.add(QStringLiteral("xl/_rels/workbook.xml.rels"), workbookRels.toUtf8());
    zip.add(QStringLiteral("xl/styles.xml"), stylesXml());
    return zip.files;
}

bool XlsxWriter::save(const QString &path, QString *errorString) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(toByteArray()) < 0 || !file.commit()) {
        if (errorString)
            *errorString = file.errorString();
        return false;
    }
    return true;
}
