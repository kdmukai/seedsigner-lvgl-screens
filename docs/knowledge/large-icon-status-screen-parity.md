# large_icon_status_screen — LVGL ↔ Python PIL parity notes

Porting `LargeIconStatusScreen` (hero icon + headline + body text + bottom button)
to pixel-parity with the Python/PIL reference surfaced several non-obvious LVGL
behaviors. They generalize to any status/warning-style screen.

## 1. The body is a CLIP container; `OVERFLOW_VISIBLE` does not defeat it
Python anchors the hero icon at `top_nav.height - COMPONENT_PADDING/2` — a small
overlap UP into the top-nav, drawn over it. Replicating that with a negative top
margin in LVGL gets the icon's top **clipped**: the standard body
(`create_standard_body_content`) is a vertical scroll container, and a scroll/clip
container clips its children to its bounds regardless of
`LV_OBJ_FLAG_OVERFLOW_VISIBLE` (that flag governs child objects, not a scroll
container's clip, and a label also clips its own text to its box).

**Fix (architecture):** move the top-nav's *bottom gap* into the BODY. The top-nav
vertically centers its buttons, leaving `(TOP_NAV_HEIGHT - TOP_NAV_BUTTON_SIZE)/2`
of empty space below a button before `TOP_NAV_HEIGHT`. `create_standard_body_content`
now extends the body UP to the button's bottom edge (`body top = TOP_NAV_HEIGHT -
tn_gap`, height `+= tn_gap`) and adds an equal DEFAULT `pad_top = tn_gap`. Net: every
screen's content still begins at `TOP_NAV_HEIGHT` (identical layout), but the body now
*owns* the gap. A screen that needs to nudge an element higher (the status hero icon)
**relaxes** that top buffer (`pad_top = tn_gap - COMPONENT_PADDING/2`) — the element
moves up into the gap entirely WITHIN the body's box, so nothing clips. No overflow
tricks, and the capability is reusable.

## 2. LVGL anchors text by font ascent; PIL anchors the visible glyph top
LVGL places a label's first-line baseline at `box_top + ascent`, where
`ascent = line_height - base_line` *includes the font's leading above the caps*. PIL
anchors the visible glyph top at the requested y. So an LVGL label sits lower than the
equivalent PIL text, and inter-element gaps read ~2× too big (e.g. the icon→headline
gap, which then cascades down and jams the bottom button against the screen edge).

**Fix:** `text_top_leading(font, text) = ascent - max_ink_ascent` (the empty space
above the caps), measured from the actual glyphs via `lv_font_get_glyph_dsc`
(`ofs_y + box_h`). Subtract it from the intended top margin so the VISIBLE text lands
where Python places it: `margin_top = COMPONENT_PADDING/2 - text_top_leading(...)`.

## 3. Same root for line spacing: declared `line_height` is loose
LVGL's declared `lv_font_get_line_height` carries loose leading and varies wildly by
script (Arabic/Farsi fonts over-reserve vertical space for stacked marks), so using it
as the multi-line advance looks far looser than PIL. `tight_line_space(font, text, gap)`
derives the advance from the text's real ink extents (`max ascender + max descender +
small gap`) and returns the `line_space` to apply. Exact for simple LTR scripts (en
matches PIL pitch). **Caveat:** presentation-form *shaped* scripts (fa) render glyphs
that differ from their source codepoints, so per-codepoint ink measurement UNDER-counts
and would collapse the lines — guarded with a floor (never tighten more than ¼ of
`line_height`). The robust long-term fix is to compute each locale's line advance
OFFLINE the way Python/PIL does and bake it per-locale into the pack manifest.

## 4. The icon "clip" was NOT a too-small box
`lv_font_get_glyph_dsc` shows the SeedSigner icon glyphs satisfy `box_h == line_height`
(`base_line == 0`), i.e. the glyph fits its label box exactly. The clipped top was
purely the negative-margin overflow of #1, not a self-clip — so padding the label box
does nothing; the real fix is #1.

## 5. Residual differences are rendering-pipeline, not bugs
- **Text clarity/brightness:** Python supersamples text (`TextArea.supersampling_factor
  = 2`): renders at 2×, downscales LANCZOS, then `ImageFilter.SHARPEN`. SHARPEN's kernel
  sums to 1, so solid glyph INTERIORS are unchanged (true color), but EDGES over/under-
  shoot — crisper, and brighter at the rim. At low res, strokes are ~all edge so the
  whole stroke reads brighter; at high res, solid interiors dominate and stay true. LVGL
  renders native into an **RGB565** framebuffer (coarser AA gradient, slight color
  shift). The color CONSTANTS match (`BODY_FONT_COLOR=0xf8f8f8`, `WARNING_COLOR=0xffd60a`)
  — the difference is the pipeline. A low-cost approximation is per-profile color
  boosting on the small displays (see plan item A8); the accurate fix is supersampling
  text in the LVGL layer (costly on ESP32-class targets).
- **Line breaks:** both PIL and LVGL are GREEDY word-wrappers (no line-length balancing)
  using the same usable width. A different break (e.g. "spends its" vs "spends") comes
  only from a 1–2px font-metric difference at the boundary word between the two
  rasterizers — not the algorithm. LVGL exposes `LV_TXT_BREAK_CHARS` and
  `LV_TXT_LINE_BREAK_LONG_*` but **no balanced-wrap mode**.
