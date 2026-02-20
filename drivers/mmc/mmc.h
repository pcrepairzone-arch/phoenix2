/*
 * mmc.h - MMC/SD Driver Header for Phoenix RISC OS
 */

#ifndef DRIVERS_MMC_H
#define DRIVERS_MMC_H

#include <stdint.h>

/* MMC Host Controller State */
typedef struct {
    uint64_t base;           /* MMIO base address */
    uint32_t rca;            /* Relative Card Address */
    uint64_t capacity;       /* Capacity in 512-byte blocks */
    uint32_t block_size;     /* Block size (typically 512) */
    uint8_t  high_capacity;  /* 1 = SDHC/SDXC, 0 = SDSC */
    uint8_t  version;        /* 1 = SD 1.x, 2 = SD 2.0+ */
} mmc_host_t;

/* Card information structure */
typedef struct {
    uint64_t capacity_mb;
    uint32_t block_size;
    uint8_t  high_capacity;
    uint8_t  version;
} mmc_card_info_t;

/* MMC Functions */
int mmc_init(void);
int mmc_read_blocks(uint32_t start_block, uint32_t num_blocks, void *buffer);
int mmc_write_blocks(uint32_t start_block, uint32_t num_blocks, const void *buffer);
void mmc_get_info(mmc_card_info_t *info);

#endif /* DRIVERS_MMC_H */
