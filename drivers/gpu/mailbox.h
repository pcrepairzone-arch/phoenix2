#ifndef MAILBOX_H
#define MAILBOX_H
#include <stdint.h>

/* Property tag IDs */
#define MBOX_TAG_FB_SET_PHYS_WH     0x00048003
#define MBOX_TAG_FB_SET_VIRT_WH     0x00048004
#define MBOX_TAG_FB_SET_DEPTH       0x00048005
#define MBOX_TAG_FB_SET_PIXEL_ORDER 0x00048006
#define MBOX_TAG_FB_ALLOCATE        0x00040001
#define MBOX_TAG_FB_GET_PITCH       0x00040008
#define MBOX_TAG_FB_SET_VIRT_OFF    0x00048009
#define MBOX_TAG_END                0x00000000

/* mbox_call: send buf (16-byte aligned) on channel 8, return 0=ok */
int mbox_call(volatile uint32_t *buf);

#endif
