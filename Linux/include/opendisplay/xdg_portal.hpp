#pragma once

#include "opendisplay/desktop_backend.hpp"

#include <QDBusConnection>
#include <QObject>
#include <QVariantMap>

#include <optional>

namespace od {

/// Shared client-side mechanics for ScreenCast and RemoteDesktop portals.
/// Compositor backends remain responsible for choosing the portal workflow.
class XdgPortal final : public QObject {
    Q_OBJECT

public:
    explicit XdgPortal(QObject* parent = nullptr);

    QVariantMap request(const QString& interface, const QString& method,
                        const QVariantList& arguments, QVariantMap options);
    /// Abandons a request that is still waiting on the user. Safe to call from
    /// a slot delivered inside request()'s own nested event loop.
    void cancel();
    QString createSession(const QString& interface);
    int openPipeWireRemote(const QString& sessionPath);
    void closeSession(const QString& sessionPath);
    void callNoReply(const QString& interface, const QString& method,
                     const QVariantList& arguments);
    QVariant property(const QString& interface, const char* name) const;

    static QVariant unwrap(QVariant value);
    static std::optional<PortalStream> firstStream(const QVariant& value,
                                                   int fallbackWidth,
                                                   int fallbackHeight);

signals:
    /// Emitted once a pending request has a verdict, including cancellation.
    void requestFinished();

private slots:
    void requestResponse(uint response, const QVariantMap& results);

private:
    QDBusConnection bus_;
    bool waiting_ = false;
    bool cancelled_ = false;
    uint responseCode_ = 2;
    QVariantMap responseResults_;
};

}  // namespace od
