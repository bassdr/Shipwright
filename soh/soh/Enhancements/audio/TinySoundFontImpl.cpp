// Isolated translation unit for the vendored single-file libraries the
// TinySoundFont backend links against: stb_vorbis (decodes the Ogg Vorbis samples
// in SF3 soundfonts) and TinySoundFont itself.
//
// Two ordering/isolation rules are load-bearing here:
//   1. stb_vorbis must be included BEFORE tsf.h. TSF only compiles its SF3 decode
//      path when stb_vorbis is visible (it gates on STB_VORBIS_INCLUDE_STB_VORBIS_H,
//      which including stb_vorbis.c defines). Without it, SF3 sample data is read as
//      raw PCM and renders as tuned noise.
//   2. Nothing else may be included after these. stb_vorbis's implementation defines
//      bare macros (L/C/R) it never undefs; if any header followed (e.g. fmt via
//      spdlog, which uses C/R/L as template parameters) it would break. tsf.h itself
//      uses none of those identifiers, so it is safe to include right after.
//
// This is why the backend's own code lives in TinySoundFont.cpp, which includes
// only tsf.h's (extern) declarations and links against the definitions compiled here.
#if ENABLE_TINYSOUNDFONT

#include <stb_vorbis.c>

#define TSF_IMPLEMENTATION
#include <tsf.h>

#endif // ENABLE_TINYSOUNDFONT
