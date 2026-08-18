#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {
constexpr const char* kReleasesLatest =
    "https://api.github.com/repos/StephenLovino/BetterCast/releases/latest";
constexpr int kTimeoutMs = 10000;
} // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString UpdateChecker::currentVersion() {
#ifdef BETTERCAST_VERSION
    return QStringLiteral(BETTERCAST_VERSION);
#else
    return QCoreApplication::applicationVersion();
#endif
}

QString UpdateChecker::currentTag() {
    const QString major = currentVersion().section(QLatin1Char('.'), 0, 0);
    return QStringLiteral("v") + major;
}

int UpdateChecker::versionNumber(const QString& tag) {
    int i = 0;
    while (i < tag.size() && !tag.at(i).isDigit()) i++;

    QString digits;
    while (i < tag.size() && tag.at(i).isDigit()) {
        digits += tag.at(i);
        i++;
    }

    bool ok = false;
    const int value = digits.toInt(&ok);
    return ok ? value : 0;
}

void UpdateChecker::check() {
    if (m_inFlight) return;
    m_inFlight = true;

    QNetworkRequest request((QUrl(QString::fromLatin1(kReleasesLatest))));
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub rejects API requests without one.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("BetterCast/%1").arg(currentVersion()));

    QNetworkReply* reply = m_net->get(request);

    // QNetworkAccessManager has no per-request timeout before Qt 5.15's
    // transferTimeout, and a silently hung check would leave the UI saying
    // "Checking..." forever.
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(kTimeoutMs);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_inFlight = false;

        if (reply->error() != QNetworkReply::NoError) {
            emit failed(reply->errorString());
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            emit failed(QStringLiteral("GitHub returned an unexpected response"));
            return;
        }

        const QJsonObject obj = doc.object();
        const QString tag   = obj.value(QStringLiteral("tag_name")).toString();
        const QString url   = obj.value(QStringLiteral("html_url")).toString();
        const QString notes = obj.value(QStringLiteral("body")).toString();

        if (tag.isEmpty()) {
            emit failed(QStringLiteral("No published release found"));
            return;
        }

        const bool newer = versionNumber(tag) > versionNumber(currentTag());
        emit finished(newer, tag, url, notes);
    });
}
