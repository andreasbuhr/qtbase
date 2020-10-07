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
#include <private/qdatetimeparser_p.h>

#include <memory>

namespace {

// access to needed members in QDateTimeParser
class QDateTimeParserTestSubclass : public QDateTimeParser
{
public:
    QDateTimeParserTestSubclass()
        : QDateTimeParser(QMetaType::QDateTime, QDateTimeParser::DateTimeEdit, QCalendar())
    {
    }

    void setText(QString text) { m_text = text; }

    using ParsedSection = QDateTimeParser::ParsedSection;

    ParsedSection parseSection(const QDateTime &currentValue, int sectionIndex, int offset) const
    {
        return QDateTimeParser::parseSection(currentValue, sectionIndex, offset);
    }
};

} // end anonymous namespace

QT_BEGIN_NAMESPACE

bool operator==(const QDateTimeParserTestSubclass::ParsedSection &a,
                const QDateTimeParserTestSubclass::ParsedSection &b)
{
    return a.value == b.value && a.used == b.used && a.zeroes == b.zeroes && a.state == b.state;
}

// pretty printing for ParsedSection
char *toString(const QDateTimeParserTestSubclass::ParsedSection &section)
{
    using QTest::toString;
    return toString(QByteArray("ParsedSection(") + "state=" + QByteArray::number(section.state)
                    + ", value=" + QByteArray::number(section.value)
                    + ", used=" + QByteArray::number(section.used)
                    + ", zeros=" + QByteArray::number(section.zeroes));
}

QT_END_NAMESPACE

class tst_QDateTimeParser : public QObject
{
    Q_OBJECT

public:
    tst_QDateTimeParser();

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void parseSection_data();
    void parseSection();

    void intermediateYear_data();
    void intermediateYear();

private:
    std::unique_ptr<QDateTimeParserTestSubclass> testParser;
};

tst_QDateTimeParser::tst_QDateTimeParser()
{
}

void tst_QDateTimeParser::initTestCase()
{
    testParser = std::make_unique<QDateTimeParserTestSubclass>();
}

void tst_QDateTimeParser::cleanupTestCase()
{
    testParser.reset();
}

void tst_QDateTimeParser::parseSection_data()
{
    QTest::addColumn<QString>("formatString");
    QTest::addColumn<QString>("userInput");
    QTest::addColumn<int>("sectionIndex");
    QTest::addColumn<int>("offset");
    QTest::addColumn<QDateTimeParserTestSubclass::ParsedSection>("expectedResult");

    QTest::newRow("short-year-begin") << "yyyy_MM_dd" // format string
                                      << "200_12_15" // user input
                                      << 0 << 0 // section index and offset
                                      << QDateTimeParserTestSubclass::ParsedSection(
                                                 QDateTimeParserTestSubclass::Intermediate, // state
                                                 200, // value
                                                 3, // read
                                                 0 // zeros
                                         );

    QTest::newRow("short-year-middle")
            << "MM-yyyy-dd" // format string
            << "12-200-15" // user input
            << 1 << 3 // section index and offset
            << QDateTimeParserTestSubclass::ParsedSection(
                       QDateTimeParserTestSubclass::Intermediate, // state
                       200, // value
                       3, // read
                       0 // zeros
               );
}

void tst_QDateTimeParser::parseSection()
{
    QFETCH(QString, formatString);
    QFETCH(QString, userInput);
    QFETCH(int, sectionIndex);
    QFETCH(int, offset);
    QFETCH(QDateTimeParserTestSubclass::ParsedSection, expectedResult);

    QVERIFY(testParser->parseFormat(formatString));
    QDateTime val(QDate(1900, 1, 1).startOfDay());

    testParser->setText(userInput);
    auto result = testParser->parseSection(val, sectionIndex, offset);
    QCOMPARE(result, expectedResult);
}

void tst_QDateTimeParser::intermediateYear_data()
{
    QTest::addColumn<QString>("formatString");
    QTest::addColumn<QString>("userInput");
    QTest::addColumn<QDateTime>("expectedResult");

    QTest::newRow("short-year-begin")
        << "yyyy_MM_dd" << "200_12_15" << QDateTime(QDate(200, 12, 15).startOfDay());
    QTest::newRow("short-year-mid")
        << "MM_yyyy_dd" << "12_200_15" << QDateTime(QDate(200, 12, 15).startOfDay());
    QTest::newRow("short-year-end")
        << "MM_dd_yyyy" << "12_15_200" << QDateTime(QDate(200, 12, 15).startOfDay());
}

void tst_QDateTimeParser::intermediateYear()
{
    QFETCH(QString, formatString);
    QFETCH(QString, userInput);
    QFETCH(QDateTime, expectedResult);

    QVERIFY(testParser->parseFormat(formatString));

    QDateTime val(QDate(1900, 1, 1).startOfDay());
    const QDateTimeParser::StateNode tmp = testParser->parse(userInput, -1, val, false);
    QCOMPARE(tmp.state, QDateTimeParser::Intermediate);
    QCOMPARE(tmp.value, expectedResult);
}

QTEST_APPLESS_MAIN(tst_QDateTimeParser)

#include "tst_qdatetimeparser.moc"
