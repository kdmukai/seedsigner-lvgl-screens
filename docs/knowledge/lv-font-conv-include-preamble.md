# lv_font_conv 1.5.3 emits an include preamble the repo's build can't resolve

## Symptom
After baking a new icon/text font with `lv_font_conv` (e.g. the 26px top-nav icon
family `seedsigner_icons_26_4bpp{,_133x,_200x}.c`) and adding it to a desktop tool's
CMake source list, the build fails to compile the new `.c` with:

```
fatal error: lvgl/lvgl.h: No such file or directory
```

…even though every *existing* baked font in `components/seedsigner/fonts/` compiles
fine in the same build.

## Root cause
The generated font `.c` starts by deciding how to include LVGL. **Older** lv_font_conv
output (all the fonts already in the repo) auto-detects the header:

```c
#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif
```

**lv_font_conv 1.5.3** drops the `__has_include` auto-detection and emits only:

```c
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
```

Our desktop CMake puts `lvgl/` on the include path as `lvgl.h` (not `lvgl/lvgl.h`),
and does **not** predefine `LV_LVGL_H_INCLUDE_SIMPLE`. So the older fonts self-define
the macro via `__has_include("lvgl.h")` and take the `#include "lvgl.h"` branch, while
the 1.5.3 fonts fall through to `#include "lvgl/lvgl.h"` — which doesn't exist.

## Fix
After baking, replace the 1.5.3 preamble with the `__has_include` form (copy it from any
existing font in `components/seedsigner/fonts/`). This makes the new fonts self-contained
and consistent with the rest of the repo, with no CMake/define changes. The bitmap +
`lv_font_t` payload below the preamble is unaffected.

(Alternative: define `LV_LVGL_H_INCLUDE_SIMPLE` globally for every font consumer — but
patching the preamble keeps the fix local to the generated artifact and matches what the
existing fonts already do.)

## Reference
- Bake command used for the 26px family (note: 1.5.3 rejects the older `--stride`/`--align`
  flags that appear in some pre-existing fonts' `Opts:` headers — omit them):
  `lv_font_conv --bpp 4 --size <26|35|52> --no-compress --font seedsigner-icons.otf --range 0xE900-0xE923 --format lvgl --lv-font-name <name> -o <name>.c`
- Sizes follow the profile multipliers: 26 (×100), 35 (×133), 52 (×200).
