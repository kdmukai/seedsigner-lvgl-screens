# REPL test: seedsigner_lvgl.button_list_screen
# Usage on device REPL:
# >>> exec(open('repl_test_button_list_screen.py').read())

import seedsigner_lvgl

print('[1/3] demo_screen sanity')
seedsigner_lvgl.demo_screen()

print('[2/3] button_list_screen default top_nav + custom buttons')
seedsigner_lvgl.button_list_screen({
    'button_list': [
        ('Language', 'lang'),
        ('Persistent Settings', {'kind': 'settings'}),
        ('Scan QR', 123),
        ('Continue', True),
    ]
})

print('[3/3] button_list_screen with top_nav override')
seedsigner_lvgl.button_list_screen({
    'top_nav': {
        'title': 'Button List Test',
        'show_back_button': True,
        'show_power_button': False,
    },
    'button_list': [
        ('Option A', None),
        ('Option B', 2),
        ('Option C', ('nested', 3)),
    ]
})

print('OK: button_list_screen REPL test executed')
