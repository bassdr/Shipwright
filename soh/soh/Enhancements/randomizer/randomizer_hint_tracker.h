#pragma once

#include "soh/SohGui/SohGuiWindow.h"
#include <ship/window/gui/GuiWindow.h>

typedef enum {
    // Every hint location grouped by area, with unread hint text masked.
    HINT_TRACKER_VIEW_LOCATIONS,
    // Only hints the player has read, grouped by hint type by usefulness.
    HINT_TRACKER_VIEW_JOURNAL,
} HintTrackerViewMode;

namespace HintTracker {

class HintTrackerSettingsWindow final : public SohGui::Window {
  public:
    using SohGui::Window::Window;

  protected:
    void InitElement() override{};
    void DrawElement() override;
    void UpdateElement() override{};
};

class HintTrackerWindow final : public SohGui::Window {
  public:
    using SohGui::Window::Window;
    void Draw() override;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override{};
};
} // namespace HintTracker
