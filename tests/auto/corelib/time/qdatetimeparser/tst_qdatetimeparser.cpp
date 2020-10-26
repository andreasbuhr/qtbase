/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the test suite of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QtTest/QtTest>

#define QDATETIMEPARSER_TEST

#include <private/qdatetimeparser_p.h>

// access to needed members in QDateTimeParser
class QDTPUnitTest
{
    QDateTimeParser p;

public:
    QDTPUnitTest() : p(QMetaType::QDateTime, QDateTimeParser::DateTimeEdit) { }

    // forward data structures
    using ParsedSection = QDateTimeParser::ParsedSection;
    using State = QDateTimeParser::State;

    // function to manipulate private internals
    void setText(QString text) { p.m_text = text; }

    // forwarding of methods
    bool parseFormat(QStringView format) { return p.parseFormat(format); }
    ParsedSection parseSection(const QDateTime &currentValue, int sectionIndex, int offset) const
    {
        return p.parseSection(currentValue, sectionIndex, offset);
    }
};

QT_BEGIN_NAMESPACE

bool operator==(const QDTPUnitTest::ParsedSection &a, const QDTPUnitTest::ParsedSection &b)
{
    return a.value == b.value && a.used == b.used && a.zeroes == b.zeroes && a.state == b.state;
}

// pretty printing for ParsedSection
char *toString(const QDTPUnitTest::ParsedSection &section)
{
    using QTest::toString;
    return toString(QByteArray("ParsedSection(") + "state=" + QByteArray::number(section.state)
                    + ", value=" + QByteArray::number(section.value)
                    + ", used=" + QByteArray::number(section.used)
                    + ", zeros=" + QByteArray::number(section.zeroes) + ")");
}

QT_END_NAMESPACE

class tst_QDateTimeParser : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parseSection_data();
    void parseSection();

    void intermediateYear_data();
    void intermediateYear();
};

void tst_QDateTimeParser::parseSection_data()
{
    QTest::addColumn<QString>("format");
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("sectionIndex");
    QTest::addColumn<int>("offset");
    QTest::addColumn<QDTPUnitTest::ParsedSection>("expected");

    using ParsedSection = QDTPUnitTest::ParsedSection;
    using State = QDTPUnitTest::State;
    QTest::newRow("short-year-begin")
        << "yyyy_MM_dd" << "200_12_15" << 0 << 0
        << ParsedSection(State::Intermediate ,200, 3, 0);

    QTest::newRow("short-year-middle")
        << "MM-yyyy-dd" << "12-200-15" << 1 << 3
        << ParsedSection(State::Intermediate, 200, 3, 0);
}

void tst_QDateTimeParser::parseSection()
{
    QFETCH(QString, format);
    QFETCH(QString, input);
    QFETCH(int, sectionIndex);
    QFETCH(int, offset);
    QFETCH(QDTPUnitTest::ParsedSection, expected);

    QDTPUnitTest testParser;

    QVERIFY(testParser.parseFormat(format));
    QDateTime val(QDate(1900, 1, 1).startOfDay());

    testParser.setText(input);
    auto result = testParser.parseSection(val, sectionIndex, offset);
    QCOMPARE(result, expected);
}

void tst_QDateTimeParser::intermediateYear_data()
{
    QTest::addColumn<QString>("format");
    QTest::addColumn<QString>("input");
    QTest::addColumn<QDate>("expected");

    QTest::newRow("short-year-begin")
        << "yyyy_MM_dd" << "200_12_15" << QDate(200, 12, 15);
    QTest::newRow("short-year-mid")
        << "MM_yyyy_dd" << "12_200_15" << QDate(200, 12, 15);
    QTest::newRow("short-year-end")
        << "MM_dd_yyyy" << "12_15_200" << QDate(200, 12, 15);
}

void tst_QDateTimeParser::intermediateYear()
{
    QFETCH(QString, format);
    QFETCH(QString, input);
    QFETCH(QDate, expected);

    QDateTimeParser testParser(QMetaType::QDateTime, QDateTimeParser::DateTimeEdit);

    QVERIFY(testParser.parseFormat(format));

    QDateTime val(QDate(1900, 1, 1).startOfDay());
    const QDateTimeParser::StateNode tmp = testParser.parse(input, -1, val, false);
    QCOMPARE(tmp.state, QDateTimeParser::Intermediate);
    QCOMPARE(tmp.value, expected.startOfDay());
}

QTEST_APPLESS_MAIN(tst_QDateTimeParser)

#include "tst_qdatetimeparser.moc"
