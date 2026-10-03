#include "opendisplay/hyprland_portal.hpp"

#include "opendisplay/log.hpp"

#include <QDBusObjectPath>
#include <QSettings>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace od {
namespace {

constexpr auto screenCastInterface = "org.freedesktop.portal.ScreenCast";

/// Restore tokens are per-source, so they are keyed by the output the session
/// captures. The virtual output keeps a fixed name for exactly this reason.
QString restoreTokenKey(const std::string& outputName) {
    return QStringLiteral("portal/restoreToken/") + QString::fromStdString(outputName);
}

}  // namespace

HyprlandPortal::HyprlandPortal(QObject* parent) : QObject(parent), portal_(this) {}

HyprlandPortal::~HyprlandPortal() { stop(); }

PortalCapture HyprlandPortal::start(const DesktopRequest& request) {
    stop();
    const int requestedWidth = std::max(2, request.receiver.pixelsWide);
    const int requestedHeight = std::max(2, request.receiver.pixelsHigh);
    int captureWidth = requestedWidth;
    int captureHeight = requestedHeight;
    int captureLogicalWidth = requestedWidth;
    int captureLogicalHeight = requestedHeight;
    std::string inputOutputName;

    try {
        const auto currentOutputs = outputs_.outputs();
        const auto reference = selectReferenceOutput(currentOutputs,
                                                     request.display.referenceMonitor);
        inputOutputName = reference.name;
        if (request.mode == CaptureMode::Extend) {
            const auto layout = planDisplayLayout(reference, request.receiver, request.display,
                                                  request.refreshRate,
                                                  DisplayModePolicy::IntegerLogicalSize);
            log("Reference monitor: " + reference.name + " "
                + std::to_string(layout.reference.resolution.width) + 'x'
                + std::to_string(layout.reference.resolution.height) + " at scale "
                + std::to_string(layout.reference.scale));
            if (layout.usedPhysicalSizing) {
                debug("Virtual scale derived from reference and receiver physical sizes");
            } else if (!request.display.virtualScale) {
                debug("Virtual scale derived from the receiver native scale");
            }
            if (layout.adjustedResolution) {
                debug("Adjusted virtual resolution for integer logical geometry: "
                      + std::to_string(layout.resolution.width) + 'x'
                      + std::to_string(layout.resolution.height));
            }
            // Fixed, not PID-suffixed: the portal's restore token is keyed to
            // the output name, so a name that changed every run would make the
            // stored permission useless and re-open the chooser every time.
            virtualOutputName_ = "OpenDisplay";
            outputCreated_ = true;
            referencePinned_ = true;
            const auto configured = outputs_.create(virtualOutputName_, layout, reference);
            inputOutputName = configured.name;
            captureWidth = layout.resolution.width;
            captureHeight = layout.resolution.height;
            captureLogicalWidth = layout.logicalGeometry.width;
            captureLogicalHeight = layout.logicalGeometry.height;
            log("Hyprland virtual monitor ready: " + virtualOutputName_ + " "
                + std::to_string(layout.resolution.width) + 'x'
                + std::to_string(layout.resolution.height) + '@'
                + std::to_string(layout.refreshRate) + ", scale "
                + std::to_string(layout.scale));
            log("In the screen-sharing dialog, select monitor " + virtualOutputName_ + '.');
        } else {
            log("In the screen-sharing dialog, select monitor " + reference.name + '.');
            captureWidth = reference.resolution.width;
            captureHeight = reference.resolution.height;
            captureLogicalWidth = reference.logicalGeometry.width;
            captureLogicalHeight = reference.logicalGeometry.height;
        }

        // Hot-plugging a headless output can make it Hyprland's active monitor,
        // and new windows follow focus — which is how the chooser ends up on
        // the virtual display nobody can see. Put it where the pointer is, and
        // fall back to the reference monitor when the pointer sits on the
        // virtual output or Hyprland cannot say.
        std::string chooserOutput = reference.name;
        if (const auto atCursor = outputs_.outputAtCursor();
            atCursor && atCursor->name != virtualOutputName_) {
            chooserOutput = atCursor->name;
        }
        chooserPinned_ = outputs_.pinChooserTo(chooserOutput);
        outputs_.focus(chooserOutput);
        log("Opening the share chooser on " + chooserOutput + "…");

        const auto availableSources = portal_.property(screenCastInterface,
                                                       "AvailableSourceTypes");
        if (availableSources.isValid() && (availableSources.toUInt() & 1U) == 0) {
            throw std::runtime_error(
                "the active Hyprland portal does not advertise monitor capture");
        }
        const auto availableCursorModes = portal_.property(screenCastInterface,
                                                           "AvailableCursorModes");
        // The portal deliberately offers no way to pick a source for the user;
        // consent is the chooser's whole purpose. Reusing an earlier consent
        // through a restore token is the sanctioned alternative, so the chooser
        // only has to be answered once per output. It needs ScreenCast v4+.
        const auto portalVersion = portal_.property(screenCastInterface, "version");
        const bool canPersist = portalVersion.isValid() && portalVersion.toUInt() >= 4;
        const auto tokenKey = restoreTokenKey(request.mode == CaptureMode::Extend
                                                  ? virtualOutputName_ : reference.name);
        QSettings settings(QStringLiteral("OpenDisplay"), QStringLiteral("OpenDisplay"));

        log("Requesting a Hyprland screen-cast portal session…");
        sessionPath_ = portal_.createSession(screenCastInterface);

        QVariantMap sourceOptions;
        sourceOptions.insert(QStringLiteral("types"), 1U);  // MONITOR
        sourceOptions.insert(QStringLiteral("multiple"), false);
        const bool canEmbedCursor = !availableCursorModes.isValid()
            || (availableCursorModes.toUInt() & 2U) != 0;
        sourceOptions.insert(QStringLiteral("cursor_mode"), canEmbedCursor ? 2U : 1U);
        if (canPersist) {
            sourceOptions.insert(QStringLiteral("persist_mode"), 2U);  // until revoked
            const auto savedToken = settings.value(tokenKey).toString();
            if (!savedToken.isEmpty()) {
                sourceOptions.insert(QStringLiteral("restore_token"), savedToken);
                log("Reusing the stored screen-cast permission; the chooser should stay "
                    "closed.");
            }
        } else {
            debug("Portal ScreenCast is older than v4; the chooser cannot be skipped");
        }
        portal_.request(screenCastInterface, QStringLiteral("SelectSources"),
                        {QVariant::fromValue(QDBusObjectPath(sessionPath_))},
                        std::move(sourceOptions));
        // XDPH opens its picker from SelectSources, so by here the choice is
        // already made. Reassert focus for the Start request, which can still
        // raise a dialog when the stored permission is refused.
        outputs_.focus(chooserOutput);
        const auto started = portal_.request(
            screenCastInterface, QStringLiteral("Start"),
            {QVariant::fromValue(QDBusObjectPath(sessionPath_)), QString()}, {});
        if (canPersist) {
            // The portal issues a fresh token per use, so write back whatever
            // Start returned and forget ours when it declined to persist.
            const auto renewed = XdgPortal::unwrap(
                started.value(QStringLiteral("restore_token"))).toString();
            if (renewed.isEmpty()) {
                settings.remove(tokenKey);
                debug("Portal did not persist the screen-cast permission");
            } else {
                settings.setValue(tokenKey, renewed);
            }
        }
        const auto parsed = XdgPortal::firstStream(
            started.value(QStringLiteral("streams")),
            std::max(1, captureWidth), std::max(1, captureHeight));
        if (!parsed) {
            throw std::runtime_error("portal did not return a PipeWire stream");
        }
        auto stream = *parsed;
        if (request.mode == CaptureMode::Extend) {
            stream.logicalWidth = captureLogicalWidth;
            stream.logicalHeight = captureLogicalHeight;
        }
        if (request.requestInput) {
            pointer_.start(inputOutputName);
            inputEnabled_ = true;
        }
        const int fd = portal_.openPipeWireRemote(sessionPath_);
        log("Hyprland portal session ready; PipeWire node "
            + std::to_string(stream.nodeId) + ", portal size "
            + std::to_string(stream.logicalWidth) + 'x'
            + std::to_string(stream.logicalHeight));
        return {
            .sessionPath = sessionPath_.toStdString(),
            .stream = stream,
            .pipewireFd = fd,
            .captureWidth = captureWidth,
            .captureHeight = captureHeight,
        };
    } catch (...) {
        stop();
        throw;
    }
}

void HyprlandPortal::stop() {
    inputEnabled_ = false;
    pointer_.stop();
    if (!sessionPath_.isEmpty()) {
        portal_.closeSession(sessionPath_);
        sessionPath_.clear();
    }
    if (outputCreated_) {
        try {
            outputs_.remove(virtualOutputName_);
        } catch (const std::exception& error) {
            debug(std::string("Cannot remove Hyprland virtual output ")
                  + virtualOutputName_ + ": " + error.what());
        }
    }
    outputCreated_ = false;
    virtualOutputName_.clear();
    if (referencePinned_ || chooserPinned_) {
        try {
            outputs_.reload();
            debug("Reloaded Hyprland configuration after removing temporary monitor rules");
        } catch (const std::exception& error) {
            debug(std::string("Cannot restore Hyprland monitor rules: ") + error.what());
        }
    }
    referencePinned_ = false;
    chooserPinned_ = false;
}

void HyprlandPortal::cancel() { portal_.cancel(); }

void HyprlandPortal::pointer(const std::string_view phase, const double normalizedX,
                             const double normalizedY) {
    if (inputEnabled_) pointer_.pointer(phase, normalizedX, normalizedY);
}

void HyprlandPortal::scroll(const double dx, const double dy) {
    if (inputEnabled_) pointer_.scroll(dx, dy);
}

}  // namespace od
