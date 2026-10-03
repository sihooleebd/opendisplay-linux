#include "opendisplay/kde_portal.hpp"

#include "opendisplay/log.hpp"

#include <QDBusObjectPath>
#include <QVariant>

#include <linux/input-event-codes.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace od {
namespace {

constexpr auto screenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr auto remoteDesktopInterface = "org.freedesktop.portal.RemoteDesktop";

}  // namespace

KdePortal::KdePortal(QObject* parent) : QObject(parent), portal_(this) {}

KdePortal::~KdePortal() { stop(); }

PortalCapture KdePortal::start(const DesktopRequest& desktopRequest) {
    stop();
    const auto mode = desktopRequest.mode;
    const auto requestInput = desktopRequest.requestInput;
    std::vector<DisplayOutput> outputsBefore;
    std::optional<DisplayLayout> layout;
    if (mode == CaptureMode::Extend) {
        outputsBefore = outputs_.outputs();
        outputsBefore_ = outputsBefore;
        const auto reference = selectReferenceOutput(
            outputsBefore, desktopRequest.display.referenceMonitor);
        layout = planDisplayLayout(reference, desktopRequest.receiver,
                                   desktopRequest.display, desktopRequest.refreshRate);
        log("Reference monitor: " + reference.name + " "
            + std::to_string(layout->reference.resolution.width) + 'x'
            + std::to_string(layout->reference.resolution.height) + " at scale "
            + std::to_string(layout->reference.scale));
        if (layout->usedPhysicalSizing) {
            debug("Virtual scale derived from reference and receiver physical sizes");
        } else if (!desktopRequest.display.virtualScale) {
            debug("Virtual scale derived from the receiver native scale");
        }
        if (layout->adjustedResolution) {
            debug("Adjusted virtual resolution for KDE custom-mode and integer logical "
                  "geometry: "
                  + std::to_string(layout->resolution.width) + 'x'
                  + std::to_string(layout->resolution.height));
        }
    }
    const auto availableSources = portal_.property(screenCastInterface, "AvailableSourceTypes");
    const auto availableCursorModes = portal_.property(screenCastInterface,
                                                       "AvailableCursorModes");
    if (mode == CaptureMode::Extend && availableSources.isValid()
        && (availableSources.toUInt() & 4U) == 0) {
        throw std::runtime_error(
            "the active KDE portal does not advertise virtual-monitor capture; use --mode mirror");
    }
    if (requestInput) {
        const auto availableDevices = portal_.property(remoteDesktopInterface,
                                                       "AvailableDeviceTypes");
        if (availableDevices.isValid() && (availableDevices.toUInt() & 2U) == 0) {
            throw std::runtime_error(
                "the active KDE portal does not advertise pointer control; use --no-input");
        }
    }
    log("Requesting a KDE remote-desktop portal session…");

    sessionPath_ = portal_.createSession(remoteDesktopInterface);

    if (requestInput) {
        QVariantMap deviceOptions;
        deviceOptions.insert(QStringLiteral("types"), 2U);  // POINTER
        portal_.request(remoteDesktopInterface, QStringLiteral("SelectDevices"),
                        {QVariant::fromValue(QDBusObjectPath(sessionPath_))},
                        std::move(deviceOptions));
    }

    QVariantMap sourceOptions;
    sourceOptions.insert(QStringLiteral("types"), mode == CaptureMode::Extend ? 4U : 1U);
    sourceOptions.insert(QStringLiteral("multiple"), false);
    // The CLI does not yet transmit cursor sprites, so prefer embedding the
    // cursor in video and fall back to the universally available hidden mode.
    const bool canEmbedCursor = !availableCursorModes.isValid()
        || (availableCursorModes.toUInt() & 2U) != 0;
    sourceOptions.insert(QStringLiteral("cursor_mode"), canEmbedCursor ? 2U : 1U);
    portal_.request(screenCastInterface, QStringLiteral("SelectSources"),
                    {QVariant::fromValue(QDBusObjectPath(sessionPath_))},
                    std::move(sourceOptions));

    const auto started = portal_.request(
        remoteDesktopInterface, QStringLiteral("Start"),
        {QVariant::fromValue(QDBusObjectPath(sessionPath_)), QString()}, {});
    const int requestedWidth = std::max(2, desktopRequest.receiver.pixelsWide);
    const int requestedHeight = std::max(2, desktopRequest.receiver.pixelsHigh);
    const auto parsed = XdgPortal::firstStream(started.value(QStringLiteral("streams")),
                                              std::max(1, requestedWidth / 2),
                                              std::max(1, requestedHeight / 2));
    if (!parsed) {
        stop();
        throw std::runtime_error("portal did not return a PipeWire stream");
    }
    stream_ = *parsed;
    inputEnabled_ = requestInput;
    int captureWidth = requestedWidth;
    int captureHeight = requestedHeight;
    if (layout) {
        try {
            const auto virtualOutput = outputs_.waitForAddedOutput(
                outputsBefore, std::chrono::seconds(5));
            virtualOutput_ = virtualOutput;
            outputTranslation_ = outputs_.apply(virtualOutput, *layout);
        } catch (...) {
            stop();
            throw;
        }
        stream_.logicalWidth = layout->logicalGeometry.width;
        stream_.logicalHeight = layout->logicalGeometry.height;
        captureWidth = layout->resolution.width;
        captureHeight = layout->resolution.height;
    }

    int fd = -1;
    try {
        fd = portal_.openPipeWireRemote(sessionPath_);
    } catch (...) {
        stop();
        throw;
    }
    log("KDE portal session ready; PipeWire node " + std::to_string(stream_.nodeId)
        + ", portal size " + std::to_string(stream_.logicalWidth) + "x"
        + std::to_string(stream_.logicalHeight));
    return PortalCapture{.sessionPath = sessionPath_.toStdString(), .stream = stream_,
                         .pipewireFd = fd, .captureWidth = captureWidth,
                         .captureHeight = captureHeight};
}

void KdePortal::stop() {
    if (sessionPath_.isEmpty()) {
        return;
    }
    portal_.closeSession(sessionPath_);
    sessionPath_.clear();
    bool virtualOutputRemoved = false;
    if (virtualOutput_) {
        try {
            virtualOutputRemoved = outputs_.waitForRemovedOutput(
                *virtualOutput_, std::chrono::seconds(3));
            if (!virtualOutputRemoved) {
                debug("Timed out waiting for KDE to remove virtual output "
                      + virtualOutput_->name);
            }
        } catch (const std::exception& error) {
            debug(std::string("Cannot verify KDE virtual output removal: ") + error.what());
        }
        virtualOutput_.reset();
    }
    if (virtualOutputRemoved && (outputTranslation_.x != 0 || outputTranslation_.y != 0)) {
        try {
            outputs_.restorePositions(outputsBefore_);
        } catch (const std::exception& error) {
            debug(std::string("Cannot restore KDE output positions: ") + error.what());
        }
    }
    outputsBefore_.clear();
    outputTranslation_ = {};
    stream_ = {};
    inputEnabled_ = false;
    pointerDown_ = false;
}

void KdePortal::notify(const QString& method, const QVariantList& arguments) {
    if (!inputEnabled_ || sessionPath_.isEmpty()) {
        return;
    }
    QVariantList full{QVariant::fromValue(QDBusObjectPath(sessionPath_)), QVariantMap{}};
    full.append(arguments);
    portal_.callNoReply(remoteDesktopInterface, method, full);
}

void KdePortal::cancel() { portal_.cancel(); }

void KdePortal::pointer(const std::string_view phase, const double normalizedX,
                        const double normalizedY) {
    const double x = std::clamp(normalizedX, 0.0, 1.0) * stream_.logicalWidth;
    const double y = std::clamp(normalizedY, 0.0, 1.0) * stream_.logicalHeight;
    notify(QStringLiteral("NotifyPointerMotionAbsolute"), {stream_.nodeId, x, y});
    if (phase == "began") {
        pointerDown_ = true;
        notify(QStringLiteral("NotifyPointerButton"), {BTN_LEFT, 1U});
    } else if ((phase == "ended" || phase == "cancelled") && pointerDown_) {
        pointerDown_ = false;
        notify(QStringLiteral("NotifyPointerButton"), {BTN_LEFT, 0U});
    }
}

void KdePortal::scroll(const double dx, const double dy) {
    notify(QStringLiteral("NotifyPointerAxis"), {dx, dy});
}

}  // namespace od
