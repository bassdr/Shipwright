#pragma once

#include <nlohmann/json.hpp>
#include <ship/window/gui/GuiWindow.h>

namespace SohGui {

/**
 * @brief GuiWindow with the InitElement() hook libultraship replaced with OnInit().
 *
 * OnInit() must chain to the base to preserve the component initialization contract.
 * Bridging once here keeps that out of every window that only wants setup code.
 */
class Window : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;

  protected:
    /** @brief One-time setup, called after the base OnInit() has run. */
    virtual void InitElement() {
    }

    void OnInit(const nlohmann::json& initArgs = nlohmann::json::object()) override {
        Ship::GuiWindow::OnInit(initArgs);
        InitElement();
    }
};

} // namespace SohGui
