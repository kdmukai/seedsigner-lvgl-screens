# REPL flow test: multi-step button list navigation
# Usage on device REPL:
# >>> exec(open('repl_test_button_list_flow.py').read())

import time
import display_manager
import seedsigner_lvgl


def wait_for_selection(prompt='Waiting for selection...'):
    print(prompt)
    while True:
        ev = seedsigner_lvgl.poll_for_result()
        if ev is None:
            time.sleep_ms(50)
            continue

        # Expected event tuple: ("button_selected", index, label)
        if isinstance(ev, tuple) and len(ev) == 3 and ev[0] == 'button_selected':
            idx = ev[1]
            label = ev[2]
            print('Selected:', idx, label)
            return idx, label

        print('Ignoring unexpected event:', ev)


print('Initializing display manager...')
display_manager.init()

while True:
    # ---------- Screen 1 ----------
    seedsigner_lvgl.clear_result_queue()
    seedsigner_lvgl.button_list_screen({
        'top_nav': {
            'title': 'Favorite Type of Pet',
            'show_back_button': False,
            'show_power_button': False,
        },
        'button_list': [
            ('Dog', 'Dog'),
            ('Cat', 'Cat'),
            ('Fish', 'Fish'),
        ]
    })

    first_idx, first_label = wait_for_selection('Step 1: choose dog/cat/fish')

    # ---------- Screen 2 ----------
    if first_idx == 0:  # dog
        second_options = [
            ('Labrador', 'Labrador'),
            ('Golden Retriever', 'Golden Retriever'),
            ('German Shepherd', 'German Shepherd'),
            ('Chihuahua / Rat Terrier', 'Chihuahua / Rat Terrier'),
        ]
    elif first_idx == 1:  # cat
        second_options = [
            ('Siamese', 'Siamese'),
            ('Maine Coon', 'Maine Coon'),
            ('Persian', 'Persian'),
            ('Ragdoll', 'Ragdoll'),
        ]
    else:  # fish
        second_options = [
            ('Betta', 'Betta'),
            ('Goldfish', 'Goldfish'),
            ('Guppy', 'Guppy'),
            ('Angelfish', 'Angelfish'),
        ]

    seedsigner_lvgl.clear_result_queue()
    seedsigner_lvgl.button_list_screen({
        'top_nav': {
            'title': 'Favorite {} Breed'.format(first_label),
            'show_back_button': True,
            'show_power_button': False,
        },
        'button_list': second_options,
    })

    second_idx, second_label = wait_for_selection('Step 2: choose favorite breed')

    # ---------- Screen 3 ----------
    # Single restart button; selecting it returns to Screen 1.
    seedsigner_lvgl.clear_result_queue()
    seedsigner_lvgl.button_list_screen({
        'top_nav': {
            'title': '{} (Final)'.format(second_label),
            'show_back_button': True,
            'show_power_button': False,
        },
        'button_list': [
            ('Restart', None),
        ]
    })

    wait_for_selection('Step 3: press Restart to return to Step 1')
    print('Restarting flow...')
