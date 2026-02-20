/*
 * audio_diag.c - HDMI audio beeps for diagnostics
 * Uses GPU mailbox to play simple tones
 */
#include <stdint.h>

extern uint64_t get_mailbox_base(void);

/* Audio format: 48kHz, 16-bit stereo */
#define SAMPLE_RATE 48000
#define TONE_FREQ   800  /* 800 Hz beep */

static volatile uint32_t __attribute__((aligned(16))) audio_msg[32];
static int16_t tone_buffer[4800] __attribute__((aligned(16)));  /* 0.1s of audio */

static inline void mb(void) {
    __asm__ volatile("dsb sy\nisb" ::: "memory");
}

/* Generate a simple sine wave tone */
static void generate_tone(int16_t *buf, int samples, int freq) {
    for (int i = 0; i < samples; i++) {
        /* Simple square wave for debugging - easier than sine */
        int16_t val = ((i * freq * 2) / SAMPLE_RATE) % 2 ? 16000 : -16000;
        buf[i * 2] = val;      /* Left */
        buf[i * 2 + 1] = val;  /* Right */
    }
}

/* Try to play a beep via HDMI audio */
int audio_beep(int count)
{
    uint64_t mbox_base = get_mailbox_base();
    volatile uint32_t *mb_write  = (volatile uint32_t *)(mbox_base + 0x20);
    volatile uint32_t *mb_read   = (volatile uint32_t *)(mbox_base + 0x00);
    volatile uint32_t *mb_status = (volatile uint32_t *)(mbox_base + 0x18);
    
    /* Generate tone buffer */
    generate_tone(tone_buffer, 2400, TONE_FREQ);
    
    for (int beep = 0; beep < count; beep++) {
        /* Build audio enable message */
        int i = 0;
        audio_msg[i++] = 0;              /* Size - fill below */
        audio_msg[i++] = 0x00000000;     /* Request */
        
        /* Tag: Set audio enable */
        audio_msg[i++] = 0x00030001;     /* Audio enable tag */
        audio_msg[i++] = 8;              /* Value size */
        audio_msg[i++] = 0;              /* Request */
        audio_msg[i++] = 1;              /* Enable audio */
        audio_msg[i++] = 0;              /* Reserved */
        
        audio_msg[i++] = 0x00000000;     /* End tag */
        audio_msg[0] = i * 4;
        
        /* Send message */
        uint32_t phys = (uint32_t)(uint64_t)audio_msg;
        uint32_t msg = ((phys & 0x0FFFFFFFU) | 0xC0000000U) | 8;
        
        /* Wait TX, send, wait RX - with short timeout */
        int timeout = 10000;
        while ((*mb_status & 0x80000000U) && --timeout > 0) mb();
        if (timeout > 0) {
            mb();
            *mb_write = msg;
            mb();
            
            timeout = 10000;
            while ((*mb_status & 0x40000000U) && --timeout > 0) mb();
            if (timeout > 0) {
                mb();
                uint32_t r = *mb_read;
                mb();
                if ((r & 0xF) == 8) {
                    /* Success - audio enabled */
                }
            }
        }
        
        /* Delay between beeps */
        for (volatile int d = 0; d < 5000000; d++);
    }
    
    return 0;
}

/* Diagnostic beep codes */
void beep_code(int code) {
    /* Code 1 = mailbox accessible, code 2 = mailbox timeout, etc */
    audio_beep(code);
}
