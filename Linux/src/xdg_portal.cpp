#include "opendisplay/xdg_portal.hpp"

#include "opendisplay/log.hpp"

#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QEventLoop>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>

#include <stdexcept>

#include <unistd.h>

namespace od {
namespace {

constexpr auto portalService = "org.freedesktop.portal.Desktop";
constexpr auto portalPath = "/org/freedesktop/portal/desktop";
constexpr auto requestInterface = "org.freedesktop.portal.Request";
constexpr auto sessionInterface = "org.freedesktop.portal.Session";
constexpr auto screenCastInterface = "org.freedesktop.portal.ScreenCast";

QString token(const QString& prefix) {
    return prefix + QString::number(QRandomGenerator::global()->generate64(), 16);
}

QString requestPath(const QDBusConnection& bus, const QString& handleToken) {
    auto sender = bus.baseService();
    sender.remove(0, 1);
    sender.replace('.', '_');
    return QStringLiteral("/org/freedesktop/portal/desktop/request/") + sender + '/'
        + handleToken;
}

}  // namespace

XdgPortal::XdgPortal(QObject* parent)
    : QObject(parent), bus_(QDBusConnection::sessionBus()) {
    if (!bus_.isConnected()) {
        throw std::runtime_error("cannot connect to the D-Bus session bus");
    }
}

QVariantMap XdgPortal::request(const QString& interface, const QString& method,
                               const QVariantList& arguments, QVariantMap options) {
    if (waiting_) {
        throw std::runtime_error("a portal request is already pending");
    }
    const auto handleToken = token(QStringLiteral("od"));
    const auto expectedPath = requestPath(bus_, handleToken);
    options.insert(QStringLiteral("handle_token"), handleToken);

    if (!bus_.connect(portalService, expectedPath, requestInterface, QStringLiteral("Response"),
                      this, SLOT(requestResponse(uint,QVariantMap)))) {
        throw std::runtime_error("cannot subscribe to the portal response");
    }

    waiting_ = true;
    cancelled_ = false;
    responseCode_ = 2;
    responseResults_.clear();

    auto message = QDBusMessage::createMethodCall(portalService, portalPath, interface, method);
    auto callArguments = arguments;
    callArguments.append(options);
    message.setArguments(callArguments);
    const auto reply = bus_.call(message, QDBus::Block, 30'000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        waiting_ = false;
        bus_.disconnect(portalService, expectedPath, requestInterface, QStringLiteral("Response"),
                        this, SLOT(requestResponse(uint,QVariantMap)));
        throw std::runtime_error((method + QStringLiteral(" failed: ") + reply.errorMessage())
                                     .toStdString());
    }

    // The portal hands its authorization dialog to the compositor, so the only
    // way to stay responsive is to run a nested event loop. That loop also
    // delivers queued calls to this thread, any of which may destroy the portal
    // owner, so nothing below may touch a member before the guard is checked.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &XdgPortal::requestFinished, &loop, &QEventLoop::quit);
    connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);
    timeout.start(300'000);

    const QPointer<XdgPortal> alive(this);
    loop.exec();
    if (alive.isNull()) {
        // Destroyed while the loop ran. Every member, bus_ included, is gone;
        // writing waiting_ or disconnecting through bus_ here would scribble on
        // freed memory and only abort much later, inside an unrelated malloc.
        throw std::runtime_error("the portal request was abandoned");
    }

    waiting_ = false;
    bus_.disconnect(portalService, expectedPath, requestInterface, QStringLiteral("Response"),
                    this, SLOT(requestResponse(uint,QVariantMap)));
    if (cancelled_) {
        throw std::runtime_error("portal request cancelled");
    }
    if (!timeout.isActive()) {
        throw std::runtime_error("portal request timed out");
    }
    if (responseCode_ != 0) {
        throw std::runtime_error(responseCode_ == 1 ? "portal request cancelled"
                                                    : "portal request denied");
    }
    return responseResults_;
}

void XdgPortal::cancel() {
    if (!waiting_) return;
    cancelled_ = true;
    waiting_ = false;
    emit requestFinished();
}

QString XdgPortal::createSession(const QString& interface) {
    QVariantMap options;
    options.insert(QStringLiteral("session_handle_token"), token(QStringLiteral("session")));
    const auto results = request(interface, QStringLiteral("CreateSession"), {},
                                 std::move(options));
    const auto sessionValue = unwrap(results.value(QStringLiteral("session_handle")));
    const auto sessionPath = sessionValue.canConvert<QDBusObjectPath>()
        ? sessionValue.value<QDBusObjectPath>().path() : sessionValue.toString();
    if (sessionPath.isEmpty()) {
        throw std::runtime_error("portal did not return a session handle");
    }
    return sessionPath;
}

int XdgPortal::openPipeWireRemote(const QString& sessionPath) {
    auto message = QDBusMessage::createMethodCall(portalService, portalPath,
                                                   screenCastInterface,
                                                   QStringLiteral("OpenPipeWireRemote"));
    message.setArguments({QVariant::fromValue(QDBusObjectPath(sessionPath)), QVariantMap{}});
    const auto reply = bus_.call(message, QDBus::Block, 30'000);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        throw std::runtime_error("portal did not provide a PipeWire remote"
                                 + (reply.errorMessage().isEmpty()
                                        ? std::string{}
                                        : ": " + reply.errorMessage().toStdString()));
    }
    const auto descriptor = reply.arguments().front().value<QDBusUnixFileDescriptor>();
    const int fd = ::dup(descriptor.fileDescriptor());
    if (fd < 0) {
        throw std::runtime_error("cannot duplicate PipeWire portal descriptor");
    }
    return fd;
}

void XdgPortal::closeSession(const QString& sessionPath) {
    if (sessionPath.isEmpty()) return;
    auto message = QDBusMessage::createMethodCall(portalService, sessionPath,
                                                   sessionInterface, QStringLiteral("Close"));
    bus_.call(message, QDBus::NoBlock);
}

void XdgPortal::callNoReply(const QString& interface, const QString& method,
                            const QVariantList& arguments) {
    auto message = QDBusMessage::createMethodCall(portalService, portalPath, interface, method);
    message.setArguments(arguments);
    bus_.call(message, QDBus::NoBlock);
}

QVariant XdgPortal::property(const QString& interface, const char* name) const {
    QDBusInterface portal(portalService, portalPath, interface, bus_);
    return portal.property(name);
}

void XdgPortal::requestResponse(const uint response, const QVariantMap& results) {
    if (!waiting_) return;
    responseCode_ = response;
    responseResults_ = results;
    waiting_ = false;
    emit requestFinished();
}

QVariant XdgPortal::unwrap(QVariant value) {
    while (value.metaType() == QMetaType::fromType<QDBusVariant>()) {
        value = value.value<QDBusVariant>().variant();
    }
    return value;
}

std::optional<PortalStream> XdgPortal::firstStream(const QVariant& raw,
                                                   const int fallbackWidth,
                                                   const int fallbackHeight) {
    const auto value = unwrap(raw);
    if (!value.canConvert<QDBusArgument>()) return std::nullopt;
    // Portal replies are read-only QDBusArgument objects; constness selects
    // Qt's read overloads for beginArray() and beginStructure().
    const auto argument = value.value<QDBusArgument>();
    debug("Portal streams D-Bus signature: " + argument.currentSignature().toStdString());
    if (argument.currentType() != QDBusArgument::ArrayType) return std::nullopt;
    argument.beginArray();
    if (argument.atEnd() || argument.currentType() != QDBusArgument::StructureType) {
        argument.endArray();
        return std::nullopt;
    }
    PortalStream stream;
    QVariantMap properties;
    argument.beginStructure();
    if (argument.currentType() != QDBusArgument::BasicType) {
        argument.endStructure();
        argument.endArray();
        return std::nullopt;
    }
    argument >> stream.nodeId;
    if (argument.currentType() != QDBusArgument::MapType) {
        argument.endStructure();
        argument.endArray();
        return std::nullopt;
    }
    argument >> properties;
    argument.endStructure();
    argument.endArray();
    stream.logicalWidth = fallbackWidth;
    stream.logicalHeight = fallbackHeight;

    const auto sizeValue = unwrap(properties.value(QStringLiteral("size")));
    if (sizeValue.canConvert<QDBusArgument>()) {
        const auto size = sizeValue.value<QDBusArgument>();
        if (size.currentType() == QDBusArgument::StructureType) {
            size.beginStructure();
            size >> stream.logicalWidth >> stream.logicalHeight;
            size.endStructure();
        }
    }
    return stream;
}

}  // namespace od
