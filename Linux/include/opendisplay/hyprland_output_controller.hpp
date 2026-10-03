#pragma once

#include "opendisplay/display_layout.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace od {

/// Window title of the xdg-desktop-portal-hyprland share chooser. The chooser
/// sets no app id, so a window rule can only match it by title, and the title
/// is a plain untranslated literal in its Qt source.
inline constexpr std::string_view hyprlandChooserTitle = "Select what to share";

std::vector<DisplayOutput> parseHyprlandOutputs(std::string_view json);
bool hyprlandCommandResponseAccepted(bool processSucceeded, std::string_view output);
std::string hyprlandMonitorExpression(const std::string& outputName,
                                      const DisplayLayout& layout,
                                      int transform = -1);
std::string hyprlandFocusExpression(const std::string& outputName);
std::string hyprlandChooserRuleExpression(const std::string& outputName);
/// Output whose logical geometry contains the point, which Hyprland reports in
/// the same logical coordinate space as `hyprctl cursorpos`.
std::optional<DisplayOutput> outputContainingPoint(const std::vector<DisplayOutput>& outputs,
                                                   int x, int y);

class HyprlandOutputController {
public:
    std::vector<DisplayOutput> outputs() const;
    DisplayOutput create(const std::string& outputName,
                         const DisplayLayout& layout,
                         const DisplayOutput& detectedReference) const;
    void focus(const std::string& outputName) const;
    /// Forces the share chooser onto an output, overriding the focus-based
    /// placement Hyprland would otherwise use. Dropped by reload(). Returns
    /// false when the compositor refused the rule, which is not fatal.
    [[nodiscard]] bool pinChooserTo(const std::string& outputName) const;
    /// Output the pointer currently sits on, or nullopt when Hyprland cannot
    /// be asked or the pointer is outside every output.
    std::optional<DisplayOutput> outputAtCursor() const;
    void remove(const std::string& outputName) const;
    void reload() const;
};

}  // namespace od
