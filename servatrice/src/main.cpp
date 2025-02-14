/***************************************************************************
 *   Copyright (C) 2008 by Max-Wilhelm Bruker   *
 *   brukie@laptop   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "passwordhasher.h"
#include "rng_sfmt.h"
#include "servatrice.h"
#include "server_logger.h"
#include "settingscache.h"
#include "signalhandler.h"
#include "smtpclient.h"
#include "version_string.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QMetaType>
#include <QTextCodec>
#include <QtGlobal>
#include <iostream>

RNG_Abstract *rng;
ServerLogger *logger;
QThread *loggerThread;
SettingsCache *settingsCache;
SignalHandler *signalhandler;
SmtpClient *smtpClient;

/* Prototypes */

void testRNG();
void testHash();
void myMessageOutput(QtMsgType type, const QMessageLogContext &, const QString &msg);
void myMessageOutput2(QtMsgType type, const QMessageLogContext &, const QString &msg);

/* Implementations */

void testRNG()
{
    const int n = 500000;
    std::cerr << "Testing random number generator (n = " << n << " * bins)..." << std::endl;

    const int min = 1;
    const int minMax = 2;
    const int maxMax = 10;

    QVector<QVector<int>> numbers(maxMax - minMax + 1);
    QVector<double> chisq(maxMax - minMax + 1);
    for (int max = minMax; max <= maxMax; ++max) {
        numbers[max - minMax] = rng->makeNumbersVector(n * (max - min + 1), min, max);
        chisq[max - minMax] = rng->testRandom(numbers[max - minMax]);
    }
    for (int i = 0; i <= maxMax - min; ++i) {
        std::cerr << (min + i);
        for (auto &number : numbers) {
            if (i < number.size())
                std::cerr << "\t" << number[i];
            else
                std::cerr << "\t";
        }
        std::cerr << std::endl;
    }
    std::cerr << std::endl << "Chi^2 =";
    for (double j : chisq)
        std::cerr << "\t" << QString::number(j, 'f', 3).toStdString();
    std::cerr << std::endl << "k =";
    for (int j = 0; j < chisq.size(); ++j)
        std::cerr << "\t" << (j - min + minMax);
    std::cerr << std::endl << std::endl;
}

void testHash()
{
    const int n = 5000;
    std::cerr << "Benchmarking password hash function (n =" << n << ")..." << std::endl;
    QDateTime startTime = QDateTime::currentDateTime();
    for (int i = 0; i < n; ++i)
        PasswordHasher::computeHash("aaaaaa", "aaaaaaaaaaaaaaaa");
    QDateTime endTime = QDateTime::currentDateTime();
    std::cerr << startTime.secsTo(endTime) << "secs" << std::endl;
}

void myMessageOutput(QtMsgType /*type*/, const QMessageLogContext &, const QString &msg)
{
    logger->logMessage(msg);
}

void myMessageOutput2(QtMsgType /*type*/, const QMessageLogContext &, const QString &msg)
{
    logger->logMessage(msg);
    std::cerr << msg.toStdString() << std::endl;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Cockatrice");
    QCoreApplication::setApplicationName("Servatrice");
    QCoreApplication::setApplicationVersion(VERSION_STRING);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption testRandomOpt("test-random", "Test PRNG (chi^2)");
    parser.addOption(testRandomOpt);

    QCommandLineOption testHashFunctionOpt("test-hash", "Test password hash function");
    parser.addOption(testHashFunctionOpt);

    QCommandLineOption logToConsoleOpt("log-to-console", "Write server logs to console");
    parser.addOption(logToConsoleOpt);

    QCommandLineOption configPathOpt("config", "Read server configuration from <file>", "file", "");
    parser.addOption(configPathOpt);

    parser.process(app);

    bool testRandom = parser.isSet(testRandomOpt);
    bool testHashFunction = parser.isSet(testHashFunctionOpt);
    bool logToConsole = parser.isSet(logToConsoleOpt);
    QString configPath = parser.value(configPathOpt);

    qRegisterMetaType<QList<int>>("QList<int>");

    if (configPath.isEmpty()) {
        configPath = SettingsCache::guessConfigurationPath();
    } else if (!QFile::exists(configPath)) {
        qCritical() << "Could not find configuration file at" << configPath;
        return 1;
    }
    qWarning() << "Using configuration file: " << configPath;
    settingsCache = new SettingsCache(configPath);

    loggerThread = new QThread;
    loggerThread->setObjectName("logger");
    logger = new ServerLogger(logToConsole);
    logger->moveToThread(loggerThread);

    loggerThread->start();
    QMetaObject::invokeMethod(logger, "startLog", Qt::BlockingQueuedConnection,
                              Q_ARG(QString, settingsCache->value("server/logfile", QString("server.log")).toString()));

    if (logToConsole)
        qInstallMessageHandler(myMessageOutput);
    else
        qInstallMessageHandler(myMessageOutput2);

    signalhandler = new SignalHandler();

    rng = new RNG_SFMT;

    std::cerr << "Servatrice " << VERSION_STRING << " starting." << std::endl;
    std::cerr << "-------------------------" << std::endl;

    if (testRandom) {
        testRNG();
    }
    if (testHashFunction) {
        testHash();
    }
    if (testRandom || testHashFunction) {
        return 0;
    }

    smtpClient = new SmtpClient();

    auto *server = new Servatrice();
    QObject::connect(server, SIGNAL(destroyed()), &app, SLOT(quit()), Qt::QueuedConnection);
    int retval = 0;
    if (server->initServer()) {
        std::cerr << "-------------------------" << std::endl;
        std::cerr << "Server initialized." << std::endl;

        qInstallMessageHandler(myMessageOutput);

        retval = QCoreApplication::exec();

        std::cerr << "Server quit." << std::endl;
        std::cerr << "-------------------------" << std::endl;
    }

    delete smtpClient;
    delete rng;
    delete signalhandler;
    delete settingsCache;

    logger->deleteLater();
    loggerThread->wait();
    delete loggerThread;

    // Delete all global objects allocated by libprotobuf.
    google::protobuf::ShutdownProtobufLibrary();

    QCoreApplication::quit();
    return retval;
}
