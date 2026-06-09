# Button List Screen: Python vs LVGL Parity Analysis

_Generated 2026-03-31 · Status updated 2026-06-09_

This document catalogs every option available in the Python/PIL `ButtonListScreen` and identifies which are not yet supported in the LVGL C implementation. Use it as a roadmap for incremental feature parity.

> **Status update (2026-06-09):** Since first writing, `is_bottom_list` is implemented (via a shared screen scaffold that stacks an optional `upper_body` above the button list and pins the buttons to the bottom). The status/warning family is implemented as a **separate** `large_icon_status_screen` entry point (status icon + headline + body text + `warning_edges` + bottom buttons) rather than as a `button_list_screen` mode. A first keyboard/text-entry screen, `seed_add_passphrase_screen`, also exists. Remaining `button_list_screen` gaps below (left-aligned text, per-button icons, checkbox/radio variants, top-nav icon, scroll restore) are still accurate.

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
| Input mode | `input.mode` | Supported (LVGL-only, no Python equivalent) |
| Aux key policy | `input.keys.*` | Supported (LVGL-only, no Python equivalent) |

> Body text above the button list is rendered by the shared scaffold's `upper_body` and is currently driven by `large_icon_status_screen` (via its `text` key); `button_list_screen` does not yet expose a body-text JSON key of its own.

### Per-button (via button_list_item_t)

| Option | Status |
|---|---|
| `label` | Supported |
| `value` | Supported (void pointer, currently unused) |


## Gap Analysis — Not Yet Supported in LVGL

### High Priority

These are needed for common screen types beyond basic menus.

| Missing Feature | Python Option(s) | Used By |
|---|---|---|
| Left-aligned text | `is_button_text_centered = False` | Seed options, tools menu, most non-main-menu screens |
| Checked/radio buttons | `Button_cls = CheckedSelectionButton`, `checked_buttons` | All single-select settings screens |
| Checkbox buttons | `Button_cls = CheckboxButton`, `checked_buttons` | All multi-select settings screens |
| Inline left icon | `ButtonOption.icon_name` | Seed options, tools menu, many screens with contextual icons |

### Medium Priority

| Missing Feature | Python Option(s) | Used By |
|---|---|---|
| Top nav icon | `top_nav_icon_name`, `top_nav_icon_color` | `SeedOptionsScreen` (fingerprint icon), other contextual screens |
| Right-side icon | `ButtonOption.right_icon_name` | Drill-down menu indicators |
| Icon color | `ButtonOption.icon_color` | Custom-colored icons per button |
| Scroll position restore | `scroll_y_initial_offset` | Returning to a long list at the previously scrolled position |

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
