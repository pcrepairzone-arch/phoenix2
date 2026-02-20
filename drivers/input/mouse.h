/*
 * mouse.h - Three-button mouse for Phoenix RISC OS
 * SELECT (left), MENU (middle), ADJUST (right)
 */

#ifndef DRIVERS_INPUT_MOUSE_H
#define DRIVERS_INPUT_MOUSE_H

#include <stdint.h>

/* RISC OS button bits */
#define BUTTON_SELECT  0x04  /* Left (bit 2) */
#define BUTTON_MENU    0x02  /* Middle (bit 1) */
#define BUTTON_ADJUST  0x01  /* Right (bit 0) */

typedef struct {
    int16_t x, y;
    int16_t dx, dy;
    uint8_t buttons;
    int8_t  wheel;
} mouse_event_t;

int mouse_init(void);
int mouse_poll(mouse_event_t *event);
void mouse_set_bounds(int16_t width, int16_t height);

#endif
