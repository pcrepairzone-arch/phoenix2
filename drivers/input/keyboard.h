/*
 * keyboard.h - Keyboard driver for Phoenix RISC OS
 * RISC OS internal key numbers (INKEY codes)
 */

#ifndef DRIVERS_INPUT_KEYBOARD_H
#define DRIVERS_INPUT_KEYBOARD_H

#include <stdint.h>

/* Function keys (RISC OS standard) */
#define KEY_F1    17
#define KEY_F2    18
#define KEY_F3    19
#define KEY_F4    20
#define KEY_F5    21
#define KEY_F6    22
#define KEY_F7    23
#define KEY_F8    24
#define KEY_F9    25
#define KEY_F10   26
#define KEY_F11   27
#define KEY_F12   28

/* Modifiers */
#define MOD_SHIFT   0x01
#define MOD_CTRL    0x02
#define MOD_ALT     0x04

typedef struct {
    uint8_t key_code;
    uint8_t key_char;
    uint8_t modifiers;
} keyboard_event_t;

int keyboard_init(void);
int keyboard_poll(keyboard_event_t *event);

#endif
