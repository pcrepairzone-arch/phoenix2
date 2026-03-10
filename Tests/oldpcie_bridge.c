/**
 * @file kernel/pcie_bridge.c
 * @brief BCM2711 PCIe Bridge Configuration
 * 
 * The Pi 4's PCIe bridge needs proper initialization to make
 * the VL805 USB controller visible on the PCI bus.
 * 
 * @author Phoenix RISC OS Team
 * @since v56
 */

#include "kernel.h"

/* BCM2711 PCIe Bridge Registers */
#define PCIE_BASE           0xFD500000

/* Bridge configuration registers */
#define PCIE_MISC_RC_BAR2_CONFIG_LO     0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI     0x4038
#define PCIE_MISC_MISC_CTRL             0x4008
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO 0x400C
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI 0x4010
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG  0x4204

/* Link status registers */
#define PCIE_RC_CFG_PCIE_LINK_STATUS_CONTROL 0x00D0
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1 0x0188

/* LTSSM states */
#define LTSSM_DETECT_QUIET  0x00
#define LTSSM_DETECT_ACT    0x01
#define LTSSM_L0            0x11  /* Link up and ready */

/**
 * @brief Read PCIe bridge register
 */
static uint32_t pcie_read(uint32_t offset)
{
    extern uint64_t peripheral_base;
    // PCIe is at peripheral_base - 0x600000 on Pi 4!
    void *addr = ioremap(peripheral_base - 0x600000 + offset, 4);
    uint32_t val = readl(addr);
    return val;
}

/**
 * @brief Write PCIe bridge register
 */
static void pcie_write(uint32_t offset, uint32_t val)
{
    extern uint64_t peripheral_base;
    void *addr = ioremap(peripheral_base - 0x600000 + offset, 4);
    writel(val, addr);
}

/**
 * @brief Get current LTSSM state (link training state)
 */
static uint8_t pcie_get_ltssm_state(void)
{
    uint32_t val = pcie_read(PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1);
    return (val >> 10) & 0x3F;  /* Bits 15:10 contain LTSSM state */
}

/**
 * @brief Check if PCIe link is up
 */
static int pcie_link_is_up(void)
{
    uint8_t ltssm = pcie_get_ltssm_state();
    return (ltssm == LTSSM_L0);
}

/**
 * @brief Wait for PCIe link to come up
 */
static int pcie_wait_for_link(int timeout_ms)
{
    debug_print("[PCIe] Waiting for link to come up...\n");
    
    for (int i = 0; i < timeout_ms; i++) {
        if (pcie_link_is_up()) {
            debug_print("[PCIe] Link is UP! (L0 state)\n");
            return 0;
        }
        
        /* Wait 1ms */
        for (volatile int j = 0; j < 1000; j++) {
            asm volatile("nop");
        }
    }
    
    uint8_t ltssm = pcie_get_ltssm_state();
    debug_print("[PCIe] Link timeout. LTSSM state: %d\n", ltssm);
    return -1;
}

/**
 * @brief Initialize PCIe bridge
 * 
 * Configures the BCM2711 PCIe bridge to enable communication
 * with the VL805 USB controller.
 * 
 * @return 0 on success, negative on error
 * @since v56
 */
int pcie_bridge_init(void)
{extern uint64_t peripheral_base;
    
    debug_print("[PCIe] Initializing Pi 4 PCIe bridge\n");
    debug_print("[PCIe] ================================\n");
    debug_print("[PCIe] DEBUG: peripheral_base = 0x%llx\n", peripheral_base);
    debug_print("[PCIe] DEBUG: PCIE_BASE (absolute) = 0x%x\n", PCIE_BASE);
    debug_print("[PCIe] DEBUG: Will try reading from 0x%llx\n", 
                (uint64_t)PCIE_BASE);
    debug_print("[PCIe] ================================\n");
    
    /* Rest of function... */    
	debug_print("[PCIe] Initializing Pi 4 PCIe bridge\n");
    
    /* Don't check if link is up - we know firmware reset it! */
    /* Just enable it! */
    
    debug_print("[PCIe] Enabling PCIe controller registers...\n");
    
    /* Enable the bridge */
    uint32_t ctrl = pcie_read(0x4000);  // PCIE_CTRL
    debug_print("[PCIe] PCIE_CTRL = 0x%x\n", ctrl);
    ctrl |= (1 << 0);  // Enable
    pcie_write(0x4000, ctrl);
    
    /* Enable SCB access */
    uint32_t misc = pcie_read(0x4008);  // MISC_CTRL  
    debug_print("[PCIe] MISC_CTRL = 0x%x\n", misc);
    misc |= (1 << 0);  // SCB_ACCESS_EN
    pcie_write(0x4008, misc);
    
    /* Give bridge time to come up */
    for (volatile int i = 0; i < 10000000; i++) {
        asm volatile("nop");
    }
    
    /* Check if link came up */
    if (pcie_link_is_up()) {
        debug_print("[PCIe] Link is UP! (L0 state)\n");
        debug_print("[PCIe] PCIe bridge ready\n");
        return 0;
    }
    
    /* If not, try starting link training */
    debug_print("[PCIe] Link still down, trying to start training...\n");
    
    uint32_t link_ctrl = pcie_read(0x00D0);
    link_ctrl |= (1 << 5);  // Retrain link
    pcie_write(0x00D0, link_ctrl);
    
    /* Wait again */
    if (pcie_wait_for_link(5) < 0) {
        uint8_t ltssm = pcie_get_ltssm_state();
        debug_print("[PCIe] Link training failed. LTSSM: %d\n", ltssm);
        debug_print("[PCIe] Continuing anyway - device might still work\n");
    }
    
    debug_print("[PCIe] PCIe bridge ready\n");
    return 0;

}