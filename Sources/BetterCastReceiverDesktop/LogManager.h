#pragma once

// In-memory + on-disk log, shared by everything that has something to report.
//
// Lived in MainWindow.h, which meant every core file - capture, encode, the
// virtual display driver, discovery, the network layer - included QMainWindow
// and the whole widget stack purely to reach this singleton. Nothing outside
// the UI needs widgets, and a front end that is not Qt Widgets could not link
// the core at all while this lived there.

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTime>

// Simple log manager (mirrors macOS LogManager)
class LogManager : public QObject {
    Q_OBJECT
public:
    static LogManager& instance() {
        static LogManager lm;
        return lm;
    }

    void log(const QString& msg) {
        QString entry = QString("[%1] %2")
            .arg(QTime::currentTime().toString("HH:mm:ss"), msg);
        m_entries.append(entry);
        if (m_entries.size() > 1000) m_entries.removeFirst();
        qDebug().noquote() << msg;
        writeToFile(entry);
        emit logAdded(entry);
    }

    void clear() { m_entries.clear(); }
    const QStringList& entries() const { return m_entries; }

    /// Where the on-disk log lives, so the UI can point users at it.
    QString logFilePath() const { return m_logPath; }

signals:
    void logAdded(const QString& entry);

private:
    /// Mirror every entry to disk as well as memory.
    ///
    /// The in-memory list dies with the process, which made every crash report
    /// useless: the app quits, the user reopens it to copy the logs, and all they can
    /// send is the *restarted* run. Three filed issues (#35, #42, #43) contain nothing
    /// but startup lines for exactly this reason.
    ///
    /// The previous run is kept as bettercast.log.1, so after a crash the evidence is
    /// still there once the app has been reopened.
    LogManager() {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dir.isEmpty()) return;
        QDir().mkpath(dir);
        m_logPath = dir + "/bettercast.log";

        QFile::remove(m_logPath + ".1");
        QFile::rename(m_logPath, m_logPath + ".1");

        m_logFile.setFileName(m_logPath);
        m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    }

    void writeToFile(const QString& entry) {
        if (!m_logFile.isOpen()) return;
        m_logFile.write(entry.toUtf8());
        m_logFile.write("\n");
        // Flush every line. Buffered writes are exactly what gets lost when the process
        // dies, and the last few lines before a crash are the ones worth having.
        m_logFile.flush();
    }

    QStringList m_entries;
    QFile m_logFile;
    QString m_logPath;
};
