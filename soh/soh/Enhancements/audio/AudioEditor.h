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
    void UpdateElement() override{};
    ~AudioEditor(){};
};

void AudioEditor_RandomizeAll();
void AudioEditor_AutoRandomizeAll();
void AudioEditor_RandomizeGroup(SeqType group);
void AudioEditor_ResetAll();
void AudioEditor_ResetGroup(SeqType group);
void AudioEditor_LockAll();
void AudioEditor_UnlockAll();
// Re-installs the modern (float) audio pipeline and FluidSynth mix source on the
// current AudioPlayer when the modern pipeline is enabled. Registered with Audio
// (see OTRAudio_Init) as the "player initialised" hook so a backend switch, which
// builds a fresh AudioPlayer, restores it in one place.
void AudioEditor_ReapplyModernAudioPipeline();

extern "C" {
#endif

u16 AudioEditor_GetReplacementSeq(u16 seqId);

#ifdef __cplusplus
}
#endif
