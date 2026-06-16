#ifndef LOCALE_LOADER_H
#define LOCALE_LOADER_H

// ---------------------------------------------------------------------------
// Shared locale-pack loader (host-agnostic orchestration)
// ---------------------------------------------------------------------------
//
// Turning "the user picked locale X" into a correctly-rendered screen takes a
// fixed sequence: clear the previous locale's fonts + glyph runs, set the active
// locale, look up from the canonical manifest (locale_fonts.h ->
// supported_locales_json) which role fonts a locale needs and at what px, register
// each, and — for complex-script locales — load runs.bin and install the glyph
// run table. That orchestration is identical on every platform, and getting it
// subtly wrong (e.g. forgetting runs.bin) renders tofu. So it lives here once,
// shared by every host (desktop tools, the WASM playground, the ESP32 firmware,
// the Pi Zero extension).
//
// The ONE thing that genuinely differs per host is acquiring the pack BYTES:
// reading a local file, fetching over HTTP, or reading + signature-verifying a
// flash/SD partition on the signing device. That stays in the host, behind the
// `ss_pack_provider_t` seam below. (The render layer deliberately never opens
// files or verifies signatures — see font_registry.h.)

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Host-supplied byte provider. Fill *bytes/*len with the contents of pack file
// `file` (e.g. "ur.ttf", "runs.bin") for `locale`. Return true on success. The
// bytes need only stay valid for the DURATION OF THIS CALL — the loader copies
// whatever it must keep. This is where per-host I/O lives, and where the signing
// device performs signature verification before handing bytes back. `user` is the
// opaque pointer passed to ss_load_locale().
typedef bool (*ss_pack_provider_t)(const char* locale, const char* file,
                                   const uint8_t** bytes, size_t* len, void* user);

// Switch to `locale`. Clears the previously loaded fonts + glyph runs, sets the
// active locale, then walks the manifest and — via `provider` — registers every
// role font and (for shaping locales) loads runs.bin + installs the glyph run
// table, in the right order. English / baked-floor locales need no provider calls
// and succeed trivially (provider may be NULL only for those). On any provider or
// registration failure it reports which file, restores the baked floor, and
// returns false. The loader OWNS the registered font byte buffers (tiny_ttf reads
// them lazily for the font's lifetime), freeing them on the next load / unload.
bool ss_load_locale(const char* locale, ss_pack_provider_t provider, void* user);

// Clear everything ss_load_locale installed (fonts, glyph runs, owned buffers),
// restoring the baked floor and clearing the active locale.
void ss_unload_locale(void);

// JSON array of the pack files `locale` needs ("["ur.ttf","runs.bin"]", or
// "["vi_regular.ttf","vi_semibold.ttf"]", or "[]" for a baked-floor locale).
// For hosts that must PRE-FETCH every blob before loading (e.g. the browser, whose
// I/O is async): get this list, fetch + stage each file, then drive ss_load_locale
// with a provider that reads from the staging area. Returned pointer is a static
// buffer, valid until the next call.
const char* ss_locale_pack_files(const char* locale);

#ifdef __cplusplus
}
#endif

#endif // LOCALE_LOADER_H
