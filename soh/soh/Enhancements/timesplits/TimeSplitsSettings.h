#pragma once
#ifndef TIMESPLITS_SETTINGS_H
#define TIMESPLITS_SETTINGS_H

#include <libultraship/libultraship.h>
#include "soh/SohGui/SohGuiWindow.h"

#ifdef __cplusplus
namespace TimeSplits {

class TimesplitsSettingsWindow final : public SohGui::Window {
  public:
    using SohGui::Window::Window;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override{};
};
} // namespace TimeSplits
#endif

#endif // TIMESPLITS_SETTINGS_H