#pragma once

#include "opendisplay/desktop_backend.hpp"
#include "opendisplay/hyprland_output_controller.hpp"
#include "opendisplay/wayland_virtual_pointer.hpp"
#include "opendisplay/xdg_portal.hpp"

#include <QObject>

#include <string>
#include <string_view>

namespace od {

class HyprlandPortal final : public QObject, public DesktopBackend {
    Q_OBJECT

public:
    explicit HyprlandPortal(QObject* parent = nullptr);
    ~HyprlandPortal() override;

    PortalCapture start(const DesktopRequest& request) override;
    void stop() override;
    void cancel() override;
    void pointer(std::string_view phase, double normalizedX, double normalizedY) override;
    void scroll(double dx, double dy) override;

private:
    XdgPortal portal_;
    HyprlandOutputController outputs_;
    WaylandVirtualPointer pointer_;
    QString sessionPath_;
    std::string virtualOutputName_;
    bool outputCreated_ = false;
    bool referencePinned_ = false;
    bool chooserPinned_ = false;
    bool inputEnabled_ = false;
};

}  // namespace od
