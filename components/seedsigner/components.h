#ifndef SEEDSIGNER_COMPONENTS_H
#define SEEDSIGNER_COMPONENTS_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Sentinel for an unset icon color: "use the default color". 0xFFFFFFFF is outside
// the 24-bit 0xRRGGBB color space, so it can never collide with a real color. Shared
// by top_nav()'s title icon and button_opts_t / button_list_item_t.
#define SEEDSIGNER_ICON_COLOR_DEFAULT 0xFFFFFFFFu

// top_nav. title_icon (optional): a contextual icon glyph rendered left of the title,
// with the icon+title group centered (Python TopNav icon_name). title_icon_color
// defaults to the body font color. out_title_label (optional): receives the title
// label object so callers can update the title text in place (e.g. a keyboard
// screen's per-keystroke counter).
lv_obj_t* top_nav(lv_obj_t* lv_parent, const char *title, bool show_back_button, bool show_power_button, lv_obj_t **out_back_btn, lv_obj_t **out_power_btn, const lv_font_t *title_font = nullptr, const char *title_icon = nullptr, uint32_t title_icon_color = SEEDSIGNER_ICON_COLOR_DEFAULT, lv_obj_t **out_title_label = nullptr);

// Lay out a top-nav title label for its CURRENT text: centered on the full bar as if
// the back button weren't there, pinned left-justified against the back-button gap
// once centering would intrude on it, and clipped+marquee-scrolled once it overflows
// the region between the buttons. top_nav() calls this at creation; screens whose
// title changes at runtime (e.g. a keyboard's "Dice Roll 47/50" counter) call it again
// after each update so the staging is recomputed against the new (wider) text instead
// of scrolling within the slice measured for the initial value.
void top_nav_layout_title(lv_obj_t* top_nav, lv_obj_t* title_label, bool has_back_button, bool has_power_button, const lv_font_t* title_font = nullptr);

// Contextual back button — the single source of truth for the back button's chrome
// (icon, highlight states, instant transition) and its SEEDSIGNER_RET_BACK_BUTTON
// click wiring. top_nav() builds its own back button via this, and the camera
// live-preview touch overlay reuses it to place the same button alone in the gutter,
// so the styling/behavior only ever has to change in one place. `align`/`x_ofs`/`y_ofs`
// position the button within `lv_parent` (e.g. LV_ALIGN_LEFT_MID, EDGE_PADDING, 0 for
// the top nav; LV_ALIGN_TOP_LEFT, EDGE_PADDING, EDGE_PADDING for the preview gutter).
lv_obj_t* back_button(lv_obj_t* lv_parent, lv_align_t align, int32_t x_ofs, int32_t y_ofs);

// Configure an already start-justified, width-constrained single-line label to
// continuously marquee-scroll (circular wrap) an overflowing line at a steady
// ~40 px/sec, with two independently-tunable holds:
//   - `begin_hold_ms`: the initial pause before scrolling starts.
//   - `loop_hold_ms`:  the pause each time the line wraps back to the start.
// The title + status headline pass LINE_SCROLL_BEGIN_HOLD_MS for both (a ~1 s beat to
// read the start, then a beat each loop). The touch long-press-to-scroll gesture passes
// begin_hold=0 (immediate — the long-press itself is the pause, and the label clips back
// on release, so an initial hold would hide the motion behind a quick release) but keeps
// loop_hold=LINE_SCROLL_BEGIN_HOLD_MS so it still pauses each time it returns to the
// start. Shaped (hi/th) labels ride the same scroll offset via the glyph-run draw
// (Task 0); RTL (ur) is excluded there for now.
void label_set_line_autoscroll(lv_obj_t* label, uint32_t begin_hold_ms, uint32_t loop_hold_ms);

// Width (px) of a label's STORED text at `font`: lv_text_get_size over
// lv_label_get_text(label) with letter/line space 0 and unconstrained width. Callers
// compare it against an available width to detect overflow. Measures the STORED text,
// NOT the original argument — with LV_USE_ARABIC_PERSIAN_CHARS, lv_label_set_text
// rewrites Arabic/Persian into (narrower) presentation forms and the subset fonts carry
// those forms, so measuring the logical string over-counts and falsely trips overflow.
// NOT valid for shaped (glyph-run) labels (the codepoint measure mis-counts conjuncts /
// presentation forms) — use seedsigner_label_run_overflows() for those.
int32_t label_subset_text_width(lv_obj_t* label, const lv_font_t* font);

// Button visual style, mirroring Python ButtonListScreen.Button_cls:
//   DEFAULT           — plain button (optional inline icons).
//   CHECKBOX          — multi-select: a leading checkbox glyph, ticked when checked
//                       (CheckboxButton). Always left-aligned.
//   CHECKED_SELECTION — single-select / radio: a leading check glyph only when
//                       checked, but the space is reserved when unchecked so rows
//                       align (CheckedSelectionButton). Always left-aligned.
typedef enum {
    BUTTON_STYLE_DEFAULT = 0,
    BUTTON_STYLE_CHECKBOX,
    BUTTON_STYLE_CHECKED_SELECTION,
} button_style_t;

typedef struct {
    const char *label;
    void *value;
    const char *icon;        // leading inline icon glyph (SeedSigner icon font), or NULL
    const char *right_icon;  // trailing right-justified icon glyph, or NULL
    uint32_t    icon_color;  // leading-icon color 0xRRGGBB, or SEEDSIGNER_ICON_COLOR_DEFAULT
    uint32_t    label_color; // label text color 0xRRGGBB, or SEEDSIGNER_ICON_COLOR_DEFAULT
    bool        is_checked;  // checkbox / checked-selection state (ignored for DEFAULT)
} button_list_item_t;

// Options for button_ex() — the full-featured list-button builder. button() is a
// thin wrapper that fills the defaults (centered text, no icons). Parity features
// add fields here over time; designated initializers leave unset fields zeroed, so
// the wrapper and every call site stay source-compatible as the struct grows.
typedef struct {
    const char    *text;              // label text (NULL renders empty)
    lv_obj_t      *align_to;          // chain-align anchor: align below this object, or
                                      // NULL to align to the parent's top (ignored when
                                      // the parent uses a flex layout, which positions it)
    bool           is_text_centered;  // true: center the label; false: left-align it
    const char    *icon;              // leading inline icon glyph, or NULL
    const char    *right_icon;        // trailing right-justified icon glyph, or NULL
    uint32_t       icon_color;        // leading-icon color, or SEEDSIGNER_ICON_COLOR_DEFAULT
    uint32_t       label_color;       // label text color, or SEEDSIGNER_ICON_COLOR_DEFAULT
    button_style_t style;             // DEFAULT / CHECKBOX / CHECKED_SELECTION
    bool           is_checked;        // checked state for the checkbox/radio styles
    int32_t        icon_column_w;     // left-icon column width to reserve so left-aligned
                                      // labels start at the SAME x across a list (the max
                                      // left-icon width on that screen). 0 = use this
                                      // button's own icon width (standalone button).
} button_opts_t;

// Width (px) of an inline icon glyph in the active profile's inline icon font.
// Callers (button_list / the scaffold) take the MAX across a list's leading icons to
// size a shared icon column so left-aligned labels line up. 0 for NULL/empty.
int32_t inline_icon_width(const char* glyph);

// Reusable Bitcoin-amount readout — the LVGL port of Python components.BtcAmount.
// Lays out, left-to-right and vertically centered: a circular Bitcoin icon (network-
// colored) + the amount digits + the unit label. Used by the PSBT overview / detail /
// change screens and anywhere an on-chain value is shown.
//
// This is a pure RENDERER: the amount is FORMATTED BY THE HOST (denomination rules,
// Decimal quantization, digit grouping, and the unit word all live in the Python
// business logic / Settings). The component only draws the pre-formatted pieces, so
// the two platforms can never disagree on how a value rounds or groups.
//
//   primary   : the main number, already grouped/quantized (e.g. "0.00841234" or
//               "8,412,340"). Digits are ASCII, so this renders in the Latin digit
//               font. Required.
//   secondary : the btcsatshybrid trailing sats run drawn after a "|" separator
//               (Python's hybrid denomination), or NULL/empty for the plain modes.
//   unit      : the translated unit word ("sats" / "btc" / "tBtc" / "tSats"). Locale
//               font (may be non-Latin). Empty to omit.
//   icon_color: the Bitcoin icon color = the network color (mainnet accent / testnet /
//               regtest). SEEDSIGNER_ICON_COLOR_DEFAULT → mainnet ACCENT_COLOR.
//   primary_small : render `primary` one step smaller (Python drops to font_size-2 for
//               sats amounts above 1e9 so they still fit).
//
// Returns a content-sized, transparent container parented to `parent`; the caller
// positions it (e.g. drop it into a center-aligned flex column, or lv_obj_align it).
typedef struct {
    const char *primary;
    const char *secondary;
    const char *unit;
    uint32_t    icon_color;
    bool        primary_small;
} btc_amount_opts_t;

lv_obj_t* btc_amount(lv_obj_t* parent, const btc_amount_opts_t* opts);

// Reusable formatted Bitcoin-address readout, in a fixed-width font, that steers the
// signer's eye to the bytes that actually need verifying:
//
//   [ recognized format prefix ]  gray  — de-emphasized: it's just the address type
//                                          ("bc1q", "1", "3", "tb1p", "bcrt1q", …), not
//                                          something to check character-by-character.
//   [ first 7 chars after prefix ] network color — the human-verifiable head.
//   [ middle ]                     gray  — bulk of the address.
//   [ last 7 chars ]               network color — the human-verifiable tail.
//
// Long addresses wrap on a fixed characters-per-line grid so every line is the same
// width; `max_lines` caps the height (a single line becomes a compact "head…tail").
// Coloring is intrinsic per-character, so it survives any wrapping.
//
// The prefix is DETECTED here (longest match over the known base58 version chars and
// bech32 HRP+version prefixes across mainnet/testnet/signet/regtest); unrecognized
// input just gets a zero-length prefix and still highlights the first/last 7. But the
// highlight COLOR is caller-supplied (from the PSBT's network), because the base58
// `m`/`n`/`2` prefixes are shared by testnet/signet/regtest and can't be told apart
// from the string alone. Pure renderer otherwise — the host derives the address.
//
//   address    : the full address string (ASCII base58 / bech32). Required.
//   width      : layout column width in px. 0 = full display width. Children are laid
//                out relative to the returned container's left edge, so the caller
//                positions the container at its screen_x.
//   max_lines  : cap on the number of lines. 1 = single-line head…tail; 0 = no cap
//                (wrap to as many lines as the address needs).
//   accent_color : the NETWORK highlight color for the head/tail (mainnet accent /
//                testnet green / regtest blue). SEEDSIGNER_ICON_COLOR_DEFAULT →
//                ACCENT_COLOR (mainnet).
//   base_color : the de-emphasized gray for the prefix + middle. SEEDSIGNER_ICON_
//                COLOR_DEFAULT → LABEL_FONT_COLOR.
//
// Returns a content-height container (width == `width`) parented to `parent`.
typedef struct {
    const char *address;
    int32_t     width;
    int         max_lines;
    uint32_t    accent_color;
    uint32_t    base_color;
} formatted_address_opts_t;

lv_obj_t* formatted_address(lv_obj_t* parent, const formatted_address_opts_t* opts);

// Reusable icon + label/value readout — the LVGL port of Python components.IconTextLine.
// Lays out an optional icon to the LEFT of a two-line text column: a small gray label on
// top of a value below it. Used by SeedFinalize (fingerprint), SeedExportXpubDetails
// (fingerprint / derivation / xpub), SeedReviewPassphrase (changes-fingerprint), and any
// screen that shows a labeled field.
//
// Alignment mirrors Python: when an icon is present the label + value are always LEFT-
// aligned within the text column and `is_text_centered` only affects where the WHOLE
// icon+text block sits — but the block's position is owned by the PARENT here (drop the
// returned row into a center-aligned flex column to center it, or a start-aligned column
// at some x to left-align it). When there is NO icon, `is_text_centered` centers the
// label + value within the column itself.
//
//   icon_glyph : icon text — a PUA icon glyph (rendered via `icon_font`) or a plain
//                character (e.g. the xpub "X"). NULL/"" omits the icon.
//   icon_font  : font for the icon. NULL -> ICON_FONT__SEEDSIGNER (24 px seedsigner icons).
//   icon_color : SEEDSIGNER_ICON_COLOR_DEFAULT -> BODY_FONT_COLOR.
//   label_text : the small gray label above the value. NULL/"" omits the label row.
//   label_font : NULL -> BODY_FONT (locale-aware; 2 px larger than Python's body-2, the
//                same faithful-localizable choice seed_finalize makes).
//   label_color: SEEDSIGNER_ICON_COLOR_DEFAULT -> LABEL_FONT_COLOR (gray).
//   value_text : the value line. Required.
//   value_font : NULL -> BODY_FONT.
//   value_color: SEEDSIGNER_ICON_COLOR_DEFAULT -> BODY_FONT_COLOR.
//   is_text_centered : see the alignment note above (only meaningful with NO icon).
//   icon_width : 0 -> the icon is its own glyph width (default). >0 -> the icon sits in a
//                fixed-width column of this many px, centered within it. Give sibling rows
//                the SAME icon_width so their text columns start at one x (left-aligned
//                labels/values) and icons of differing widths (PUA glyphs vs a plain "X")
//                center on a shared axis.
//
// Returns a content-sized, transparent row parented to `parent`.
typedef struct {
    const char*      icon_glyph;
    const lv_font_t* icon_font;
    uint32_t         icon_color;
    const char*      label_text;
    const lv_font_t* label_font;
    uint32_t         label_color;
    const char*      value_text;
    const lv_font_t* value_font;
    uint32_t         value_color;
    bool             is_text_centered;
    int32_t          icon_width;
} icon_text_line_opts_t;

lv_obj_t* icon_text_line(lv_obj_t* parent, const icon_text_line_opts_t* opts);

// Reclaim the line-height leading LVGL reserves above the caps / below the descenders of
// a single-line label, with negative vertical margins, so stacked labels pack at their
// VISIBLE ink height (PIL/Python measures the tight text bbox; LVGL boxes are taller).
// Measured PER-TEXT and descender-aware: a label whose text has no descender (e.g.
// "Derivation") reclaims its full descent and sits tight to the line below, while text
// with a descender ("Fingerprint" has a 'g') keeps the room its descender needs.
// Call AFTER lv_label_set_text(). Reclaiming empty leading never clips ink.
void reclaim_line_leading(lv_obj_t* label, const lv_font_t* font);

// Like reclaim_line_leading(), but UNIFORM across labels that share one font: it reclaims
// the FONT's leading (measured once from a cap 'X' + a descender 'g'), NOT each label's own
// ink. Use it for a column of homogeneous labels — e.g. the seed-words list — so every line
// gets the SAME box height and baseline and they align consistently, regardless of which
// words happen to carry descenders or ascenders.
void reclaim_line_leading_uniform(lv_obj_t* label, const lv_font_t* font);

lv_obj_t* button(lv_obj_t* lv_parent, const char* text, lv_obj_t* align_to);
lv_obj_t* button_ex(lv_obj_t* lv_parent, const button_opts_t* opts);
lv_obj_t* large_icon_button(lv_obj_t* lv_parent, const char* icon, const char* text, lv_obj_t* align_to);
lv_obj_t* button_list(lv_obj_t* lv_parent, const button_list_item_t *items, size_t item_count, bool is_button_text_centered, button_style_t style);

void button_set_active(lv_obj_t* lv_button, bool active);

// The tagged TEXT label inside a button built by button()/button_ex() (NULL if it
// has none). Exposed so a bespoke screen (e.g. the locale picker) can suppress or
// overlay it: icon/label siblings make child-order lookups unreliable, so this
// resolves the text label by its internal identity tag.
lv_obj_t* button_text_label(lv_obj_t* btn);

// Hardware/joystick focus: promote a focused button's too-wide text label to a
// marquee scroll (true) or clip it back to its start edge (false). Driven from the
// nav layer because body buttons are kept out of the LVGL focus group, so LVGL
// never emits the FOCUSED/DEFOCUSED events that would otherwise drive this.
void button_set_label_marquee(lv_obj_t* lv_button, bool marquee);


#endif // SEEDSIGNER_COMPONENTS_H
