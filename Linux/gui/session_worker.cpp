#include "session_worker.hpp"

#include "opendisplay/desktop_backend_factory.hpp"
#include "opendisplay/discovery.hpp"

#include <QTimer>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

namespace od::gui {
namespace {

/// Marks a frame that may run a nested event loop, so stop() defers instead of
/// destroying the session underneath it.
class CallGuard {
public:
    explicit CallGuard(bool& flag) : flag_(flag) { flag_ = true; }
    ~CallGuard() { flag_ = false; }
    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;

private:
    bool& flag_;
};

}  // namespace

SessionWorker::SessionWorker(QObject* parent) : QObject(parent), timer_(new QTimer(this)) {
    timer_->setInterval(std::chrono::milliseconds(20));
    connect(timer_, &QTimer::timeout, this, &SessionWorker::tick);
}

void SessionWorker::start(od::Options options) {
    if (session_ || inSessionCall_) return;

    const CallGuard guard(inSessionCall_);
    stopRequested_ = false;
    emit stateChanged(QStringLiteral("Connecting"),
                      QStringLiteral("Searching for an OpenDisplay receiver…"), false, true);
    try {
        const auto endpoint = od::chooseEndpoint(options);
        const QString endpointName = QString::fromStdString(
            endpoint.name.empty() ? (endpoint.kind == od::TransportKind::Usb
                                         ? endpoint.udid
                                         : endpoint.host)
                                  : endpoint.name);
        emit stateChanged(QStringLiteral("Connecting"),
                          QStringLiteral("Negotiating with %1…").arg(endpointName), false, true);
        session_ = std::make_unique<od::Session>(
            options, od::makeDesktopBackend(options.compositor));
        // Portal authorization inside start() runs a nested event loop, so a
        // queued stop() can land before this returns.
        session_->start(endpoint);
        if (stopRequested_) {
            throw std::runtime_error("the connection was cancelled");
        }
        timer_->start();
        emit stateChanged(QStringLiteral("Connected"),
                          QStringLiteral("Streaming to %1").arg(endpointName), true, false);
    } catch (const std::exception& error) {
        const bool cancelled = stopRequested_;
        stopRequested_ = false;
        timer_->stop();
        if (session_) session_->stop();
        session_.reset();
        if (cancelled) {
            emit stateChanged(QStringLiteral("Disconnected"),
                              QStringLiteral("Ready to connect."), false, false);
        } else {
            emit stateChanged(QStringLiteral("Connection failed"),
                              QString::fromUtf8(error.what()), false, false);
        }
    }
}

void SessionWorker::tick() {
    if (!session_) {
        timer_->stop();
        return;
    }
    // Reconfiguring for a rotated receiver re-runs the portal request, so this
    // call can spin a nested event loop just like start() does.
    const CallGuard guard(inSessionCall_);
    try {
        if (session_->tick() && !stopRequested_) return;
        const bool cancelled = stopRequested_;
        stopRequested_ = false;
        timer_->stop();
        session_->stop();
        session_.reset();
        emit stateChanged(QStringLiteral("Disconnected"),
                          cancelled ? QStringLiteral("Ready to connect.")
                                    : QStringLiteral("The receiver ended the connection."),
                          false, false);
    } catch (const std::exception& error) {
        const bool cancelled = stopRequested_;
        stopRequested_ = false;
        timer_->stop();
        session_->stop();
        session_.reset();
        if (cancelled) {
            emit stateChanged(QStringLiteral("Disconnected"),
                              QStringLiteral("Ready to connect."), false, false);
        } else {
            emit stateChanged(QStringLiteral("Connection failed"),
                              QString::fromUtf8(error.what()), false, false);
        }
    }
}

void SessionWorker::stop() {
    if (inSessionCall_) {
        // start() or tick() is suspended in the portal's nested event loop
        // further down this stack. Ask the pending request to give up so that
        // frame unwinds and runs the teardown itself.
        stopRequested_ = true;
        if (session_) session_->cancel();
        return;
    }
    timer_->stop();
    if (!session_) return;
    // Tearing a session down can itself run a nested loop (KScreen's output
    // operations do), so hold the guard here too and let any stop() that
    // arrives meanwhile fold into this one.
    const CallGuard guard(inSessionCall_);
    session_->stop();
    session_.reset();
    stopRequested_ = false;
    emit stateChanged(QStringLiteral("Disconnected"),
                      QStringLiteral("Ready to connect."), false, false);
}

}  // namespace od::gui
