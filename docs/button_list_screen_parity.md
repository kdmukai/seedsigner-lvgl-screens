# Button List Screen: Python vs LVGL Parity Analysis

_Generated 2026-03-31 · Status updated 2026-06-23_

This document catalogs every option available in the Python/PIL `ButtonListScreen` and identifies which are not yet supported in the LVGL C implementation. Use it as a roadmap for incremental feature parity.

> **Status update (2026-06-09):** Since first writing, `is_bottom_list` is implemented (via a shared screen scaffold that stacks an optional `upper_body` above the button list and pins the buttons to the bottom). The status/warning family is implemented as a **separate** `large_icon_status_screen` entry point (status icon + headline + body text + `warning_edges` + bottom buttons) rather than as a `button_list_screen` mode. A first keyboard/text-entry screen, `seed_add_passphrase_screen`, also exists. Remaining `button_list_screen` gaps below (left-aligned text, per-button icons, checkbox/radio variants, top-nav icon, scroll restore) are still accurate.
>
> **Status update (2026-06-23):** `button_list_screen` now exposes its own intro/body `text` key (`cfg["text"]`), rendered into the scaffold's `upper_body` above the buttons — the same mechanism `large_icon_status_screen` uses — so **body text is no longer a gap**.
>
> **Status update (2026-06-23, full-parity build):** **All remaining High + Medium gaps are now CLOSED** — left-aligned text, per-button inline + right icons + icon color, checkbox / checked-selection (radio) styles, and the top-nav contextual icon (with a new 26px icon font). Details + outcomes are in the **[Build Spec — Full Compliance Target](#build-spec--full-compliance-target-2026-06-23)** section at the end of this doc. Scroll restore is a documented no-op (native restores via `initial_selected_index`). The only remaining end-to-end work is the seedsigner-side forwarding leaf (`to_lvgl()` / `button_list_lvgl_cfg()`).

## Source Files

- **Python**: `seedsigner/src/seedsigner/gui/screens/screen.py` — `ButtonListScreen` (line ~295), `ButtonOption` (line ~270)
- **Python Button**: `seedsigner/src/seedsigner/gui/components.py` — `Button` (line ~1316), `CheckedSelectionButton` (line ~1608), `CheckboxButton` (line ~1626)
- **LVGL screen**: `components/seedsigner/seedsigner.cpp` — `button_list_screen()` (line ~267)
- **LVGL components**: `components/seedsigner/components.cpp` — `button()`, `button_list()`


## Python ButtonListScreen — Full Option Inventory

### Screen-level options (ButtonListScreen + parent BaseTopNavScreen)

| Option | Type | Default | Description |
|---|---|---|---|
| `title` | str | `"Screen Title"` | Top nav title text |
| `title_font_size` | int | system default | Top nav title font size |
| `show_back_button` | bool | `True` | Show back arrow in top nav |
| `show_power_button` | bool | `False` | Show power icon in top nav |
| `top_nav_icon_name` | str | `None` | Icon displayed next to title in top nav |
| `top_nav_icon_color` | str | `None` | Color of the top nav icon |
| `button_data` | list[ButtonOption] | `None` | The list of buttons to render |
| `selected_button` | int | `0` | Which button starts selected |
| `is_button_text_centered` | bool | `True` | Center-align button text vs left-align |
| `is_bottom_list` | bool | `False` | Pin buttons to bottom of screen (used by confirmation screens) |
| `button_font_name` | str | system default | Font for all buttons on this screen |
| `button_font_size` | int | system default | Font size for all buttons on this screen |
| `button_selected_color` | str | `ACCENT_COLOR` | Highlight color when a button is selected |
| `Button_cls` | class | `Button` | Button class override (e.g. `CheckedSelectionButton`, `CheckboxButton`) |
| `checked_buttons` | List[int] | `None` | Indices of buttons that display a checkmark |
| `scroll_y_initial_offset` | int | `None` | Initial scroll position for returning to a scrolled list |

### Per-button options (ButtonOption dataclass)

| Option | Type | Default | Description |
|---|---|---|---|
| `button_label` | str | required | Button display text |
| `icon_name` | str | `None` | Left-side icon rendered inline with text |
| `icon_color` | str | `None` | Color of the left icon |
| `right_icon_name` | str | `None` | Right-justified icon (e.g. chevron for drill-down) |
| `button_label_color` | str | `None` | Custom text color for this button |
| `return_data` | Any | `None` | Arbitrary data returned when button is selected |
| `active_button_label` | str | `None` | Alternate text displayed when button is in active/selected state |
| `font_name` | str | `None` | Per-button font override |
| `font_size` | int | `None` | Per-button font size override |

### Button class variants

- **`Button`** — Standard button with text, optional inline icon, optional right icon. Supports scrollable text for long labels.
- **`CheckedSelectionButton`** — Left-aligned text with a checkmark icon for checked items. Used for single-select settings (radio-button style).
- **`CheckboxButton`** — Left-aligned text with a checkbox icon (checked or unchecked). Used for multi-select settings.


## LVGL button_list_screen — Currently Supported

### Screen-level (via JSON config)

| Option | JSON key | Status |
|---|---|---|
| Title text | `top_nav.title` | Supported |
| Back button | `top_nav.show_back_button` | Supported |
| Power button | `top_nav.show_power_button` | Supported |
| Bottom-pinned button list | `is_bottom_list` | Supported (shared scaffold; stacks optional upper body above bottom-pinned buttons) |
| Initial selected index | `initial_selected_index` | Supported |
| Intro/body text above buttons | `text` | Supported (2026-06-23; rendered into the scaffold's `upper_body`, the same mechanism as `large_icon_status_screen`) |
| Left-aligned button text | `is_button_text_centered` | Supported (2026-06-23; `false` left-aligns, physical even for RTL) |
| Checkbox / radio styles | `button_style` + `checked_buttons` | Supported (2026-06-23; `"checkbox"` / `"checked_selection"`) |
| Top-nav contextual icon | `top_nav.icon` + `top_nav.icon_color` | Supported (2026-06-23; centered icon+title group, 26px icon font) |
| Input mode | `input.mode` | Supported (LVGL-only, no Python equivalent) |
| Aux key policy | `input.keys.*` | Supported (LVGL-only, no Python equivalent) |

> Body text above the button list is rendered by the shared scaffold's `upper_body`. As of 2026-06-23 `button_list_screen` exposes its own `text` key for this (previously only `large_icon_status_screen` did).

### Per-button (via button_list_item_t / object `button_list` entry)

| Option | Status |
|---|---|
| `label` | Supported |
| `value` | Supported (void pointer, currently unused) |
| `icon` | Supported (2026-06-23; inline leading icon) |
| `right_icon` | Supported (2026-06-23; trailing right-justified icon) |
| `icon_color` | Supported (2026-06-23; custom leading-icon color, kept at rest) |
| `is_checked` | Supported (2026-06-23; via screen-level `checked_buttons`) |


## Gap Analysis

### High Priority — ✅ all resolved (2026-06-23)

These were needed for common screen types beyond basic menus; all now implemented.

| Feature | Python Option(s) | Used By | Status |
|---|---|---|---|
| Left-aligned text | `is_button_text_centered = False` | Seed options, tools menu, most non-main-menu screens | ✅ Done |
| Checked/radio buttons | `Button_cls = CheckedSelectionButton`, `checked_buttons` | All single-select settings screens | ✅ Done |
| Checkbox buttons | `Button_cls = CheckboxButton`, `checked_buttons` | All multi-select settings screens | ✅ Done |
| Inline left icon | `ButtonOption.icon_name` | Seed options, tools menu, many screens with contextual icons | ✅ Done |

### Medium Priority — ✅ all resolved (2026-06-23)

| Feature | Python Option(s) | Used By | Status |
|---|---|---|---|
| Top nav icon | `top_nav_icon_name`, `top_nav_icon_color` | `SeedOptionsScreen` (fingerprint icon), other contextual screens | ✅ Done |
| Right-side icon | `ButtonOption.right_icon_name` | Drill-down menu indicators | ✅ Done |
| Icon color | `ButtonOption.icon_color` | Custom-colored icons per button | ✅ Done |
| Scroll position restore | `scroll_y_initial_offset` | Returning to a long list at the previously scrolled position | ✅ No-op (uses `initial_selected_index`) |

### Low Priority

| Missing Feature | Python Option(s) | Used By |
|---|---|---|
| Title font size override | `title_font_size` | Rarely overridden outside main menu |
| Button font overrides (screen) | `button_font_name`, `button_font_size` | Rare per-screen font changes |
| Button font overrides (per-button) | `ButtonOption.font_name`, `ButtonOption.font_size` | Rare per-button font changes |
| Custom selected color | `button_selected_color` | Rare highlight color overrides |
| Per-button text color | `ButtonOption.button_label_color` | Rare per-button color overrides |
| Active button label | `ButtonOption.active_button_label` | Alt text when selected (uncommon) |


## Implementation Notes

The LVGL `button_list_screen` receives its config as a JSON string. Adding new options means:

1. Parsing the new JSON keys in `button_list_screen()` (`seedsigner.cpp`)
2. Extending `button_list_item_t` and/or `button()` in `components.cpp` for per-button features
3. Adding new component functions (e.g. `checked_button()`) for button variants
4. Updating `scenarios.json` with test cases for the new options


## Build Spec — Full Compliance Target (2026-06-23)

Goal: bring `button_list_screen` + the shared `top_nav` to full config parity with the
Python `ButtonListScreen` / `BaseTopNavScreen` / `ButtonOption`, so every list-shaped
Python screen can be served natively. Intro/body `text` and `is_bottom_list` already
shipped; the items below are what remain. Compliance target = **all High + Medium items**;
Low-priority cosmetics are optional, wired on demand.

Each item lists the **JSON contract** to parse, the **native code** to change, and
**acceptance** (a `scenarios.json` case + committed desktop screenshot; a `runner_core`
smoke for interactive changes).

> **Cross-repo contract (keep aligned):** this repo defines and parses the JSON; the
> `seedsigner` repo forwards the matching fields. Today `ButtonOption.to_lvgl()` emits a
> bare label string and `view.py:button_list_lvgl_cfg()` drops `is_button_text_centered`,
> `checked_buttons`, `top_nav_icon_name` and does not forward `text` / `show_power_button`.
> The JSON keys below are the agreed shape those will send; the Python edits are tracked as
> the seedsigner-side LVGL leaf, not here.

> **Status (2026-06-23): ALL High + Medium items DONE** on branch `feat/button-list-parity`.
> `button()` was refactored into `button_ex(button_opts_t*)` (with `button()` kept as a
> centered wrapper); a text-label identity tag (`find_button_text_label`) lets the icon
> labels coexist with the overflow/marquee/click helpers. Each item below carries its
> outcome. Every pre-existing screenshot stayed byte-identical across the whole series; new
> scenarios + committed desktop screenshots cover each feature; the `runner_core` smoke
> passes. RTL (ur) spot-checked: icons stay physically placed (UI not flipped) while text
> keeps its RTL post-pass. **Still pending (separate seedsigner-side LVGL leaf):** the
> `to_lvgl()` / `button_list_lvgl_cfg()` forwarding so parity is end-to-end.

### 1. Left-aligned button text — High — ✅ DONE
- **Outcome:** `cfg["is_button_text_centered"]` parsed in the scaffold and threaded through
  `button_list()` + both scaffold build paths; `apply_button_label_layout` gained an
  `is_text_centered` arg (physical LEFT for RTL per the no-flip decision). `large_icon_button`
  stays centered. Scenario: `left_aligned`.
- **JSON:** `cfg["is_button_text_centered"]` (bool, default `true`); `false` → left-align labels.
- **Native:** `button()` in `components.cpp` hardcodes `LV_TEXT_ALIGN_CENTER` — thread an
  alignment arg through `button_list()` ← `button_list_screen()`. Main-menu
  `large_icon_button` stays centered.
- **Accept:** left-aligned menu scenario; long-label wrap still left-aligns.

### 2. Per-button inline left icon (+ right icon, icon color) — High/Med — ✅ DONE
- **Outcome:** `button_list` entries may be objects
  `{ "label": str, "icon"?: str, "icon_color"?: str, "right_icon"?: str }` (string/array
  still accepted). `read_button_list_items()` parses them; `button_list_item_t` /
  `button_opts_t` gained `icon` / `right_icon` / `icon_color`. `apply_button_icon_layout()`
  positions the inline leading + right icons absolutely, mirroring Python `Button.__post_init__`
  (icon_padding = COMPONENT_PADDING, vertically centered, the centered/left text shift,
  `right_icon_x = width - icon_w - COMPONENT_PADDING`). A per-button color state keeps a custom
  `icon_color` at rest and flips every glyph to black on selection (Python `selected_icon_color`).
  Scenarios: `icons`, `icons_centered`.

### 3. Checkbox / radio (checked-selection) buttons — High — ✅ DONE
- **Outcome:** `cfg["button_style"]` (`"default" | "checkbox" | "checked_selection"`) +
  `cfg["checked_buttons"]: [int]`. Implemented by **parameterizing `button_ex` with
  `button_style_t`** (not a separate `checked_button()` — it reuses the #1 left-align + #2
  inline-icon machinery): CHECKBOX → `CHECKBOX_SELECTED`/`CHECKBOX` glyph; CHECKED_SELECTION →
  green `CHECK` when checked, reserving the glyph's width when unchecked so rows align.
  Presentation-only (host re-renders the set). Scenarios: `checkbox_multi`, `radio_single`.

### 4. Top-nav contextual icon — Med (explicitly in scope) — ✅ DONE
- **Outcome:** `cfg["top_nav"]["icon"]` + `["icon_color"]`. `top_nav()` gained
  `title_icon` / `title_icon_color` params (the pre-existing trailing params were OUTPUT
  back/power-button pointers, not icon hooks) and renders the icon+title group centered
  (Python TopNav `IconTextLine`, `icon_horizontal_spacer = COMPONENT_PADDING/2`). Needed a
  new **26px icon font family** (Python `ICON_FONT_SIZE+4`) baked at 26/35/52px and wired
  into `DisplayProfile.top_nav_icon_font`. A11 centering/overflow unchanged. Scenario:
  `top_nav_icon`.

### 5. Scroll position restore — Med (recommend no-op) — ✅ Documented no-op
- `scroll_y_initial_offset` is intentionally unsupported: the native screen restores position
  via `initial_selected_index` (focus-scrolls the item into view). No JSON key; revisit only
  if a screen needs exact pixel restore.

### Low priority (optional / cosmetic — defer unless a screen needs it)
Per-screen/per-button font overrides (`button_font_*`, `ButtonOption.font_*`),
`button_selected_color`, `ButtonOption.button_label_color`, `active_button_label`,
`title_font_size`. Wire on demand.

### Definition of done
- ✅ High + Medium items landed with `scenarios.json` cases + committed desktop screenshots;
  `runner_core` smoke passes. Series held every pre-existing screenshot byte-identical.
- ⬜ The matching seedsigner-side forwarding (`to_lvgl()` + `button_list_lvgl_cfg`) — opened
  as the LVGL leaf in that repo so parity is end-to-end. **Still pending.**
