#include "seedsigner.h"
#include "screen_scaffold.h"
#include "screen_helpers.h"
#include "components.h"
#include "gui_constants.h"
#include "navigation.h"

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <stdexcept>

using json = nlohmann::json;

// PSBTAddressDetailsScreen: one recipient's amount over its full (wrapped) address,
// vertically centered between the top nav and the action button. The View supplies the
// title ("Verify Send Address" etc.) and the button label.
//
// cfg:
//   top_nav.title            — screen title (default "Address").
//   btc_amount { ... }       — the send amount (btc_amount_from_cfg contract).
//   address (string, req.)   — the full destination address.
//   button_list (array)      — action buttons (default ["Next"]).
void psbt_address_details_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("psbt_address_details_screen requires an \"address\" string");
    }
    std::string address = cfg["address"].get<std::string>();

    // Bottom-pinned button-list shape (Python is_bottom_list = True).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Address";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Next" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // Center the [amount / address] block vertically in the gap above the button, the
    // same way seed_finalize centers its fingerprint readout: grow upper_body to claim
    // the gap, center on both axes, collapse the scaffold spacer. The COMPONENT_PADDING
    // row gap matches Python's amount->address spacing.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object()) {
        btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);
    }

    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    formatted_address_opts_t fo = {};
    fo.address      = address.c_str();
    fo.width        = W - 2 * EDGE_PADDING;
    fo.max_lines    = 0;                                        // wrap to as many lines as needed
    fo.accent_color = network_color(resolve_address_network(cfg, address));   // head/tail = network color
    fo.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;
    formatted_address(screen.upper_body, &fo);

    bind_screen_navigation(cfg, screen, 0);
    load_screen_and_cleanup_previous(screen.screen);
}
