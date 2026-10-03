#pragma once

#include "opendisplay/desktop_backend.hpp"
#include "opendisplay/kde_output_controller.hpp"
#include "opendisplay/xdg_portal.hpp"

#include <QObject>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace od {

/// KDE implementation of the platform capture/output/input boundary. It uses
/// the public RemoteDesktop + ScreenCast portals, so future compositor
/// backends can implement the same surface without touching Session.
class KdePortal final : public QObject, public DesktopBackend {
    Q_OBJECT

public:
    explicit KdePortal(QObject* parent = nullptr);
    ~KdePortal() override;

    PortalCapture start(const DesktopRequest& request) override;
    void stop() override;
    void cancel() override;
    void pointer(std::string_view phase, double normalizedX, double normalizedY) override;
    void scroll(double dx, double dy) override;

private:
    void notify(const QString& method, const QVariantList& arguments);

    XdgPortal portal_;
    QString sessionPath_;
    PortalStream stream_;
    bool inputEnabled_ = false;
    bool pointerDown_ = false;
    KdeOutputController outputs_;
    std::optional<DisplayOutput> virtualOutput_;
    std::vector<DisplayOutput> outputsBefore_;
    OutputTranslation outputTranslation_;
};

}  // namespace od
