# Screen Implementation Plan: Python to LVGL

_Generated 2026-04-08 · Status updated 2026-06-09_

This document catalogs every screen in the Python SeedSigner codebase, identifies which can be implemented using an enhanced reusable `button_list_screen`, and which require dedicated LVGL screen types. Companion doc: [button_list_screen_parity.md](button_list_screen_parity.md) (feature-level gap analysis).

> **Status update (2026-06-09):** Progress since first writing — the **`is_bottom_list` + upper-body scaffold** (Phase 1) is implemented; the **status/warning family** (Phase 3) shipped as a dedicated `large_icon_status_screen` entry point (icon + headline + body `text` + `warning_edges`), and the **first keyboard screen** (Phase 5a), `seed_add_passphrase_screen`, is done. Still outstanding: left-aligned text + per-button icons (rest of Phase 1), checkbox/radio variants + scroll restore + top-nav icon (Phase 2), structured body content blocks (Phase 4), and the remaining dedicated screens (more keyboards, QR display, camera preview). Section statuses below are annotated inline.
>
> **Status update (2026-06-23):** `button_list_screen` now exposes its own intro/body `text` key (`cfg["text"]`) via the shared `upper_body` scaffold — so the Group A "body text" enhancement is **done**.
>
> **Status update (2026-06-23, full-parity build):** **Phase 1 and Phase 2 are now complete.** Left-aligned text, per-button inline + right icons + icon color, checkbox/checked-selection (radio) variants, and the top-nav contextual icon all shipped — so Groups A (incl. icon-bearing rows), B, and C are fully serviceable by the enhanced `button_list_screen`. See the companion doc's **Build Spec — Full Compliance Target** section for outcomes. Remaining: Phase 4 structured body content (Group D) + the dedicated Phase 5 screens, and the seedsigner-side forwarding leaf.
>
> **Status update (2026-07-01):** More dedicated screens have shipped since. **Phase 5a keyboards** — a general `keyboard_screen` + `seed_mnemonic_entry_screen` (split-panel keyboard + live word-match list). **Phase 5b (partial)** — a portable `camera_preview_overlay` renderer (the ScanScreen wraps it). **Opening splash** — `splash_screen`. And **Phase 5c QR display is now DONE**: a dedicated `qr_display_screen` (qrcodegen bundled in LVGL; numeric/alphanumeric/byte/auto modes; host-driven animation with a brightness-tip gate; hardware text hints + touch slider). See `docs/qr-display-screen-spec.md`. Still outstanding: Phase 4 structured body content (Group D), the SeedQR **transcription/zoom** screens (5c niche), the rest of Phase 5b (full camera ScanScreen + image-entropy), the IO-test screen (5d), and the seedsigner-side forwarding leaves.

## Source Files

- **Python screens**: `seedsigner/src/seedsigner/gui/screens/` — `screen.py`, `seed_screens.py`, `psbt_screens.py`, `tools_screens.py`, `settings_screens.py`, `scan_screens.py`
- **Python components**: `seedsigner/src/seedsigner/gui/components.py` — `TextArea`, `IconTextLine`, `FormattedAddress`, `BtcAmount`, `Button`, `TopNav`
- **LVGL screen**: `components/seedsigner/seedsigner.cpp` — `button_list_screen()`, `main_menu_screen()`
- **LVGL components**: `components/seedsigner/components.cpp` — `button()`, `button_list()`, `top_nav()`


## The Key Insight

The vast majority of Python screens follow one pattern:

```
┌──────────────────────┐
│  Top Nav (title)     │
├──────────────────────┤
│                      │
│  Body content        │  ← TextArea, IconTextLine, formatted data
│  (optional)          │
│                      │
├──────────────────────┤
│  Button list         │  ← pinned to bottom (is_bottom_list=True)
│  (1-N buttons)       │
└──────────────────────┘
```

As of 2026-06-23 the LVGL `button_list_screen` supports both a body content area (intro/body `text` via `cfg["text"]`) and the **`is_bottom_list`** layout mode — the two enhancements that let a single reusable `button_list_screen` handle ~30 of the ~38 Python screens not yet implemented. The remaining per-screen needs are left-aligned text, per-button icons, and the checkbox/radio + top-nav-icon variants (see the companion doc's Build Spec).


## Part 1: Screens Already Implemented in LVGL

| Screen | LVGL Function | Status |
|---|---|---|
| Main Menu | `main_menu_screen()` | Done |
| Screensaver | `screensaver_screen()` | Done |
| Basic button list menus | `button_list_screen()` | Done (incl. `is_bottom_list` + upper-body scaffold) |
| Status / Warning / Error / Success family | `large_icon_status_screen()` | Done (icon + headline + body text + `warning_edges`) |
| Passphrase entry (keyboard) | `seed_add_passphrase_screen()` | Done (first Phase 5a keyboard screen) |
| General keyboard / text entry | `keyboard_screen()` | Done (Phase 5a) |
| Seed mnemonic entry | `seed_mnemonic_entry_screen()` | Done (Phase 5a — split keyboard + live word-match) |
| Camera live-preview overlay | `camera_preview_overlay_screen()` | Done (Phase 5b renderer; full ScanScreen wraps it) |
| Opening splash | `splash_screen()` | Done |
| QR display (SeedQR / xpub / PSBT / address / BBQR / animated) | `qr_display_screen()` | Done (Phase 5c — see `docs/qr-display-screen-spec.md`) |


## Part 2: Screens Achievable with Enhanced `button_list_screen`

These Python screens are all `ButtonListScreen` subclasses. They add custom components (TextArea, icons, formatted text) between top_nav and buttons, but the layout is fundamentally the same pattern. Grouped by which enhancements they require.

### Group A: Needs `is_bottom_list` + body text

These screens display informational text above bottom-pinned buttons. The body content is plain text (TextArea) or simple icon+text lines. This is the biggest group and the highest-value enhancement. _(2026-06-23: `is_bottom_list`, the upper-body scaffold, and the `button_list_screen` body-text key (`cfg["text"]`) are all done — so the plain-text rows in this group are fully serviceable now. What remains is left-aligned text + per-button icons for the icon+text rows; see the companion doc's Build Spec.)_

| Screen | File | Body Content | Buttons |
|---|---|---|---|
| `SeedFinalizeScreen` | seed_screens.py | IconTextLine showing fingerprint | "Done" (1 button, no back) |
| `SeedWordsBackupTestPromptScreen` | seed_screens.py | TextArea explaining backup verification | "Verify" / "Skip" |
| `SeedTranscribeSeedQRFormatScreen` | seed_screens.py | IconTextLine explaining Standard vs Compact QR | Format selection buttons |
| `SeedTranscribeSeedQRConfirmQRPromptScreen` | seed_screens.py | TextArea with instructions | "Confirm" / "Redo" |
| `PSBTFinalizeScreen` | psbt_screens.py | Sign icon centered | "Approve" / "Cancel" |
| `PSBTOpReturnScreen` | psbt_screens.py | OP_RETURN data (text or hex) | "Next" / "Cancel" |
| `ToolsCalcFinalWordFinalizePromptScreen` | tools_screens.py | TextArea about entropy bits | Action buttons |
| `ToolsCalcFinalWordDoneScreen` | tools_screens.py | Confirmation text | "Done" |
| `LoadMultisigWalletDescriptorScreen` | seed_screens.py | Instructions text | Action buttons |
| `SeedSignMessageConfirmMessageScreen` | seed_screens.py | Message text display | "Approve" / "Deny" |
| `SeedSignMessageConfirmAddressScreen` | seed_screens.py | Address display | "Approve" / "Deny" |

**Enhancement needed**: JSON config additions:
```json
{
  "body_text": "Informational text here...",
  "body_text_centered": true,
  "is_bottom_list": true
}
```

### Group B: Needs Group A + per-button icons

These screens use left-aligned buttons with inline icons for contextual menus.

| Screen | File | Body Content | Button Features |
|---|---|---|---|
| `SeedOptionsScreen` | seed_screens.py | None (icon-menu only) | Left icons per button, left-aligned text, top_nav icon |
| `PSBTAddressDetailsScreen` | psbt_screens.py | FormattedAddress component | Bottom buttons |
| `SeedExportXpubDetailsScreen` | seed_screens.py | Multiple IconTextLines (fingerprint, derivation, xpub) | Bottom buttons, WarningEdges |
| `SeedReviewPassphraseScreen` | seed_screens.py | Passphrase display + fingerprint comparisons | Bottom buttons |

**Enhancement needed**: Per-button icon support in JSON:
```json
{
  "button_list": [
    {"label": "Export Xpub", "icon": "QRCODE"},
    {"label": "Sign PSBT", "icon": "SIGN"}
  ],
  "is_button_text_centered": false
}
```

### Group C: Needs checkbox/radio button variants

Settings screens that use checked selection or checkbox buttons.

| Screen | File | Body Content | Button Features |
|---|---|---|---|
| `SettingsEntryUpdateSelectionScreen` | settings_screens.py | Setting name + help text | CheckedSelectionButton or CheckboxButton, checked_buttons list |

**Enhancement needed**: Button type and checked state in JSON:
```json
{
  "button_type": "checkbox",
  "checked_buttons": [0, 2],
  "button_list": ["Option A", "Option B", "Option C"]
}
```

### Group D: Needs Group A + structured body content

These screens have more complex body layouts — multiple formatted sections, amounts with icons, derivation paths — but still follow the top_nav / body / bottom-buttons pattern. They can work with `button_list_screen` if the body content supports a list of typed content blocks rather than just plain text.

| Screen | File | Body Content | Complexity |
|---|---|---|---|
| `PSBTOverviewScreen` | psbt_screens.py | BtcAmount + transaction flow diagram (spend, change, fee, inputs/outputs counts) | Multiple formatted lines with icons |
| `PSBTMathScreen` | psbt_screens.py | Input/spend/fee/change amounts with calculation layout | Multiple amount lines |
| `PSBTChangeDetailsScreen` | psbt_screens.py | Address QR + derivation path + verification status | QR image + text lines |
| `ToolsCalcFinalWordScreen` | tools_screens.py | Bit-level entropy visualization (entropy bits, checksum, final word) | Multiple styled TextAreas |
| `MultisigWalletDescriptorScreen` | seed_screens.py | Multisig policy details | Formatted text sections |

**Enhancement needed**: Structured body content in JSON:
```json
{
  "body": [
    {"type": "text", "text": "Spending", "centered": true},
    {"type": "icon_text", "icon": "BITCOIN_ALT", "text": "0.00150000 BTC"},
    {"type": "divider"},
    {"type": "text", "text": "Fee: 0.00001200 BTC", "font_size": "small"}
  ],
  "is_bottom_list": true,
  "button_list": ["Approve", "Cancel"]
}
```

This is the most ambitious enhancement. An alternative is to implement some of these as dedicated screen types if the body content is too varied.


## Part 3: LargeIconStatusScreen Family

These follow a distinct pattern that's close to button_list_screen but has a large centered icon + headline above the body text. Could be a mode of `button_list_screen` or a separate `status_screen` entry point.

```
┌──────────────────────┐
│  Top Nav (title)     │
├──────────────────────┤
│                      │
│      [ICON]          │  ← Large status icon (success, warning, error)
│    Headline          │  ← Bold status text
│  Body text here      │  ← Explanatory text
│                      │
├──────────────────────┤
│  Button(s)           │
└──────────────────────┘
```

| Screen | File | Icon | Color | Used For |
|---|---|---|---|---|
| `LargeIconStatusScreen` | screen.py | Configurable | Configurable | Generic status display |
| `WarningScreen` | screen.py | WARNING | Yellow | Caution messages + animated warning edges |
| `DireWarningScreen` | screen.py | WARNING | Red | Serious warnings |
| `ErrorScreen` | screen.py | ERROR | Red | Error messages |
| `SeedAddressVerificationSuccessScreen` | seed_screens.py | SUCCESS | Green | Address verified |

**Status (2026-06-09): IMPLEMENTED** as a dedicated `large_icon_status_screen()` entry point (not a `button_list_screen` mode, as originally proposed). Actual JSON config:
```json
{
  "top_nav": { "title": "Backup Verified", "show_back_button": false },
  "status_type": "success",
  "status_headline": "Success!",
  "text": "All mnemonic backup words were successfully verified!",
  "button_list": ["OK"],
  "is_bottom_list": true,
  "warning_edges": false
}
```

`status_type` selects the icon/color (success / warning / error / …). The animated warning-edge pattern for `WarningScreen` / `DireWarningScreen` is the `warning_edges` boolean. This single screen covers the whole family above.


## Part 4: SeedWordsScreen (Paginated Word Display)

`SeedWordsScreen` deserves special mention. It displays seed words in a paginated list (4 words per page) with page navigation, warning edges, and word-number prefixes. It's a ButtonListScreen subclass but the "buttons" are really just numbered word display lines (not selectable in the usual sense).

| Screen | File | Notes |
|---|---|---|
| `SeedWordsScreen` | seed_screens.py | Paginated word list with WarningEdges, page_index/num_pages tracking |

**Recommendation**: This could work as a `button_list_screen` with:
- Body text showing page indicator ("Page 2 of 3")
- Read-only button-styled lines (non-selectable) for the words
- Warning edges flag
- Navigation buttons ("Next" / "Done")

Or it could be a dedicated `seed_words_screen` if the display requirements diverge too much from buttons.


## Part 5: Screens Requiring Dedicated LVGL Implementations

These screens have fundamentally different rendering needs that cannot be handled by any button list variant.

### 5a. Keyboard / Text Entry Screens

| Screen | File | Description |
|---|---|---|
| `KeyboardScreen` | screen.py | General-purpose keyboard with configurable grid layout, char sets, save button |
| `SeedMnemonicEntryScreen` | seed_screens.py | 6x5 keyboard + live word match list (split-panel layout) |
| `SeedAddPassphraseScreen` | seed_screens.py | Passphrase entry with visibility toggle |
| `ToolsDiceEntropyEntryScreen` | tools_screens.py | 3x3 dice icon keyboard (KeyboardScreen variant) |
| `ToolsCoinFlipEntryScreen` | tools_screens.py | 1x4 binary keyboard for coin flips (KeyboardScreen variant) |

**Recommendation**: One `keyboard_screen` LVGL entry point with configurable grid dimensions, charset, and optional side panel for word matches.

### 5b. Camera / Live Preview Screens

| Screen | File | Description |
|---|---|---|
| `ScanScreen` | scan_screens.py | Live camera preview + QR decode progress bar |
| `ToolsImageEntropyLivePreviewScreen` | tools_screens.py | Camera preview for entropy capture |
| `ToolsImageEntropyFinalImageScreen` | tools_screens.py | Static image review with reshoot/accept |

**Recommendation**: These are hardware-specific. On ESP32-S3, camera feeds directly to display buffer. Likely one `camera_preview_screen` with mode flags.

### 5c. QR Display Screens

| Screen | File | Description |
|---|---|---|
| `QRDisplayScreen` | screen.py | Full-screen QR code with brightness up/down controls — **DONE** as `qr_display_screen()` |
| `SeedTranscribeSeedQRWholeQRScreen` | seed_screens.py | QR code with zone highlights for transcription — deferred |
| `SeedTranscribeSeedQRZoomedInScreen` | seed_screens.py | Zoomed QR view with 5x5/7x7 module grid, zone navigation — deferred |

**Status (2026-07-01):** `QRDisplayScreen` is **implemented** as the dedicated `qr_display_screen()` — static + host-driven animated QRs, numeric/alphanumeric/byte/auto encoding, brightness-tip-gated animation, hardware text hints + touch slider. Full spec: `docs/qr-display-screen-spec.md`. The two SeedQR **transcription/zoom** screens are **deferred** (niche); if built they'd likely be a separate `qr_transcribe_screen`.

### 5d. Hardware Test Screen

| Screen | File | Description |
|---|---|---|
| `IOTestScreen` | settings_screens.py | Multi-region button layout for testing joystick, keys, camera |

**Recommendation**: Dedicated `io_test_screen`. This is a diagnostic tool, not a user-facing flow.

### 5e. Simple Text-Only Screens

| Screen | File | Description |
|---|---|---|
| `ResetScreen` | settings_screens.py | TextArea message only (no buttons) |
| `PowerOffNotRequiredScreen` | settings_screens.py | TextArea message only |
| `DonateScreen` | settings_screens.py | Donation info display |

**Recommendation**: These are just `button_list_screen` with body text and zero buttons, or one dismiss button. No new screen type needed.


## Part 6: Implementation Priority

### Phase 1: Maximum coverage with minimum effort

Add these features to `button_list_screen` to unlock ~20 screens:

1. ✅ **`is_bottom_list`** — Pin buttons to bottom of screen _(done)_
2. ✅ **`body_text`** — Simple text block between top_nav and buttons _(done; `button_list_screen` exposes its own `text` key into the scaffold `upper_body`)_
3. ✅ **`is_button_text_centered`** — Left-align option for icon menus _(done 2026-06-23)_
4. ✅ **Per-button icons** — `icon` / `right_icon` / `icon_color` on object `button_list` entries _(done 2026-06-23)_

### Phase 2: Settings and selection screens

5. ✅ **Checkbox/radio button variants** — `button_style` + `checked_buttons` _(done 2026-06-23)_
6. ✅ **`scroll_y_initial_offset`** — documented no-op; native restores via `initial_selected_index`
7. ✅ **`top_nav.icon` / `top_nav.icon_color`** — Icon in top nav bar _(done 2026-06-23; 26px icon font)_

### Phase 3: Status/warning pattern — ✅ done (as `large_icon_status_screen`)

8. ✅ **`status_type` / `status_headline` / `text`** — Large centered icon + headline + body _(shipped as a dedicated `large_icon_status_screen`, not a `button_list_screen` mode)_
9. ✅ **`warning_edges`** — Animated warning border pattern

### Phase 4: Structured body content

10. **Body content blocks** — Array of typed content items (text, icon_text, divider, amount) for PSBT and complex info screens

### Phase 5: Dedicated screen types

11. ◑ **`keyboard_screen`** — Text entry with configurable grid _(first instance done: `seed_add_passphrase_screen`; a general configurable keyboard_screen + the other text-entry screens remain)_
12. ⬜ **`qr_display_screen`** — QR code rendering with controls
13. ⬜ **`camera_preview_screen`** — Live camera feed (hardware-specific)


## Summary: Screen Count by Category

| Category | Count | Implementation |
|---|---|---|
| Already implemented | 5 | main_menu, screensaver, basic button_list (+`is_bottom_list`), large_icon_status, passphrase keyboard |
| Phase 1 (body text + bottom list + icons) | ~17 | Enhanced button_list_screen |
| Phase 2 (checkboxes + scroll restore) | ~3 | Enhanced button_list_screen |
| Phase 3 (status/warning pattern) | ~5 | Enhanced button_list_screen (status mode) |
| Phase 4 (structured body content) | ~5 | Enhanced button_list_screen (body blocks) |
| Phase 5a (keyboard/text entry) | ~5 | New keyboard_screen |
| Phase 5b (camera/live preview) | ~3 | New camera_preview_screen |
| Phase 5c (QR display) | ~3 | New qr_display_screen |
| Phase 5d (IO test) | 1 | New io_test_screen |
| **Total** | **~45** | |

With just the Phase 1 enhancements to `button_list_screen`, roughly **40% of all remaining screens** become implementable with no new screen types.
