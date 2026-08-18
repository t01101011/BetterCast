#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;

// Checks GitHub Releases for a newer BetterCast, mirroring the macOS app.
//
// Windows shares the macOS release line: one vN tag carries the .dmg, the .apk
// and the Windows installer, so both apps read the same "latest release" and
// compare the same way. Before that, Windows tags looked like windows-1.0.1
// while macOS used v17, and releases/latest returns whichever came last
// repo-wide — a Windows build would have compared its own 1.0.0 against a
// macOS v17, read 17 > 1, and claimed an update forever.
class UpdateChecker : public QObject {
    Q_OBJECT

public:
    explicit UpdateChecker(QObject* parent = nullptr);

    // This build's release tag, e.g. "v17" — the major version, which is what
    // the tags carry. Derived from the version CMake compiles in, so it cannot
    // drift from the installer the way three hardcoded copies did.
    static QString currentTag();

    // Full version for display, e.g. "17.0.0".
    static QString currentVersion();

    // Leading integer of a tag: "v17" and "V17.2" both give 17, anything
    // unparseable gives 0 so a malformed tag never looks newer than a real one.
    static int versionNumber(const QString& tag);

    void check();

signals:
    // updateAvailable is false when this build is current — callers still get
    // latestTag so they can show "you are on the latest version".
    void finished(bool updateAvailable, const QString& latestTag,
                  const QString& url, const QString& notes);
    void failed(const QString& message);

private:
    QNetworkAccessManager* m_net = nullptr;
    bool m_inFlight = false;
};
