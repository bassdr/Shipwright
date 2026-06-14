#pragma once
#include <stdint.h>

#ifdef __cplusplus

#include <libultraship/libultraship.h>
#include <imgui.h>
#include "AudioCollection.h"

class AudioEditor final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void DrawElement() override;
    void InitElement() override;
    void UpdateElement() override {};
    ~AudioEditor() {};
};

void AudioEditor_RandomizeAll();
void AudioEditor_AutoRandomizeAll();
void AudioEditor_RandomizeGroup(SeqType group);
void AudioEditor_ResetAll();
void AudioEditor_ResetGroup(SeqType group);
void AudioEditor_LockAll();
void AudioEditor_UnlockAll();
// Re-installs the float pipeline and FluidSynth mix source on the current
// AudioPlayer when the float pipeline is enabled. Registered (see OTRAudio_Init)
// as the player-initialised hook so a backend switch, which builds a fresh
// AudioPlayer, restores it in one place.
void AudioEditor_ReapplyModernAudioPipeline();
// Pushes the current Master Volume CVar onto the running FluidSynth's master
// gain so the synth tracks the slider. A no-op when no synth is installed; call
// from the Master Volume slider's handler.
void AudioEditor_ApplySynthMasterVolume();

extern "C" {
#endif

u16 AudioEditor_GetReplacementSeq(u16 seqId);

#ifdef __cplusplus
}
#endif
