#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

// Minimal writer for Excel workbooks (.xlsx, Office Open XML) without any
// dependency beyond QtCore: cell values and formulas, styles, merged cells,
// frozen header rows, auto filters and native line charts. Rows and columns
// are 0-based. Everything is kept in memory and written in one go.
class XlsxWriter
{
public:
    // 0xRRGGBB; default-constructed means "not set".
    struct Color
    {
        constexpr Color() = default;
        constexpr Color(quint32 rgb)
            : rgb(rgb & 0xFFFFFF)
            , valid(true)
        {
        }
        bool isValid() const { return valid; }
        bool operator==(const Color &other) const { return valid == other.valid && rgb == other.rgb; }

        quint32 rgb = 0;
        bool valid = false;
    };

    struct Style
    {
        enum Align { General, Left, Center, Right };
        enum Border { NoBorder = 0, BorderLeft = 1, BorderRight = 2, BorderTop = 4, BorderBottom = 8, BorderAll = 15 };

        QString numberFormat; // Excel format code, e.g. "0.0" or "0%"; empty = General
        bool bold = false;
        bool italic = false;
        double fontSize = 11.0;
        Color fontColor;   // invalid = default
        Color fill;        // invalid = none
        int borders = NoBorder;
        Color borderColor; // invalid = default
        Align align = General;
        bool wrap = false;

        bool operator==(const Style &other) const;
    };

    struct ChartSeries
    {
        QString name;
        int column = 0;
        int firstRow = 0;
        int lastRow = 0;
        Color color;
    };

    struct LineChart
    {
        QString title;
        QString xTitle;
        QString yTitle;
        // Category (x axis) labels, taken from these cells
        int categoryColumn = 0;
        int categoryFirstRow = 0;
        int categoryLastRow = 0;
        QList<ChartSeries> series;
        // Placement: top left and bottom right cell
        int fromRow = 0;
        int fromColumn = 0;
        int toRow = 15;
        int toColumn = 8;
    };

    class Sheet;

    XlsxWriter();
    ~XlsxWriter();
    XlsxWriter(const XlsxWriter &) = delete;
    XlsxWriter &operator=(const XlsxWriter &) = delete;

    void setCreator(const QString &creator) { m_creator = creator; }
    void setTitle(const QString &title) { m_title = title; }

    // Sheet names are made valid for Excel (max. 31 characters, no []:*?/\).
    Sheet &addSheet(const QString &name);

    // The files of the package (path, content), e.g. for inspection in tests.
    QList<QPair<QString, QByteArray>> files() const;
    QByteArray toByteArray() const;
    bool save(const QString &path, QString *errorString = nullptr) const;

    // "A1"-style reference, e.g. cellRef(0, 0) == "A1", absolute adds '$'.
    static QString cellRef(int row, int column, bool absolute = false);
    static QString columnName(int column);

private:
    int styleIndex(const Style &style);
    QByteArray stylesXml() const;
    QByteArray workbookXml() const;
    QByteArray chartXml(const Sheet &sheet, const LineChart &chart) const;

    std::vector<std::unique_ptr<Sheet>> m_sheets;
    QList<Style> m_styles;
    QString m_creator;
    QString m_title;
};

class XlsxWriter::Sheet
{
public:
    const QString &name() const { return m_name; }

    void setText(int row, int column, const QString &text, const Style &style = {});
    void setNumber(int row, int column, double value, const Style &style = {});
    void setDateTime(int row, int column, const QDateTime &value, const Style &style);
    // Formula without the leading '=', plus the value Excel shows until it recalculates.
    void setFormula(int row, int column, const QString &formula, double cachedValue, const Style &style = {});
    void setStyle(int row, int column, const Style &style); // empty, formatted cell

    void setColumnWidth(int column, double characters);
    void setRowHeight(int row, double points);
    void mergeCells(int row, int column, int lastRow, int lastColumn);
    void freezeRows(int rows);
    void setAutoFilter(int row, int column, int lastRow, int lastColumn);
    void setShowGridLines(bool show) { m_showGridLines = show; }
    void setTabColor(const Color &color) { m_tabColor = color; }
    void setLandscape(bool landscape) { m_landscape = landscape; }
    void addLineChart(const LineChart &chart) { m_charts.append(chart); }

private:
    friend class XlsxWriter;

    struct Cell
    {
        enum Type { Blank, Number, Text, Formula } type = Blank;
        double number = 0.0;
        QString text; // text or formula
        int style = 0;
    };

    Sheet(XlsxWriter *writer, const QString &name);
    void set(int row, int column, Cell cell);
    QString cellText(int row, int column) const;
    double cellNumber(int row, int column, bool *ok) const;
    QByteArray xml(int drawingRelId) const;

    XlsxWriter *m_writer;
    QString m_name;
    QMap<int, QMap<int, Cell>> m_rows;
    QMap<int, double> m_columnWidths;
    QMap<int, double> m_rowHeights;
    QStringList m_merged;
    QString m_autoFilter;
    int m_frozenRows = 0;
    bool m_showGridLines = true;
    bool m_landscape = false;
    Color m_tabColor;
    QList<LineChart> m_charts;
};
