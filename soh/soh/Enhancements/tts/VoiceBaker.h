#pragma once

#include <string>

namespace SOH {

// Generates missing voice clips on the player's machine, off the game thread.
//
// The clip cache is content-addressed, so synthesising on demand and baking
// ahead are the same thing: a line is rendered the first time it is spoken and
// read from disk every time after. Synthesis runs about 25x faster than the
// speech it produces, which fits inside a textbox fade-in.
class VoiceBaker {
  public:
    static VoiceBaker& Instance();

    // Game thread. Queues the line if it is not already queued, and returns
    // immediately; the clip plays when it is ready, or never if the voice has no
    // model installed.
    // Loading a model costs about a second and every voice has its own, so the
    // first line each character speaks would otherwise pay for it mid-conversation.
    // Loads them on the worker thread, ahead of anyone talking.
    void Warm(const std::string& language);

    void Request(const std::string& text, const std::string& language, const std::string& profile, float gain);

    // Stops the worker and waits for the line in flight.
    void Shutdown();

  private:
    VoiceBaker() = default;
};

} // namespace SOH
