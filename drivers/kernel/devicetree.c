/*
 * devicetree.c - Basic Device Tree Parser
 * Extracts essential hardware info from DTB
 */

#include "kernel.h"
#include <stdint.h>

/* FDT (Flattened Device Tree) header */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_MAGIC 0xD00DFEED
#define FDT_BEGIN_NODE 0x01
#define FDT_END_NODE 0x02
#define FDT_PROP 0x03
#define FDT_NOP 0x04
#define FDT_END 0x09

static uint32_t fdt32_to_cpu(uint32_t x) {
    return __builtin_bswap32(x);
}

/* Parse memory node to get RAM size */
static void parse_memory_node(const char *dtb, uint64_t *mem_start, uint64_t *mem_size) {
    struct fdt_header *hdr = (struct fdt_header *)dtb;
    
    if (fdt32_to_cpu(hdr->magic) != FDT_MAGIC) {
        debug_print("ERROR: Invalid device tree magic\n");
        *mem_start = 0;
        *mem_size = 1024 * 1024 * 1024; // Default 1GB
        return;
    }
    
    // Simplified: assume Pi has memory at 0, size from boot
    // Real parser would walk the tree
    *mem_start = 0;
    *mem_size = 1024 * 1024 * 1024; // 1GB default for Pi 4
    
    debug_print("DeviceTree: Memory: start=0x%llx, size=%lld MB\n", 
               *mem_start, *mem_size / (1024*1024));
}

/* Main device tree parser */
void device_tree_parse(uint64_t dtb_ptr) {
    if (dtb_ptr == 0) {
        debug_print("WARNING: No device tree provided\n");
        return;
    }
    
    debug_print("Parsing device tree at 0x%llx\n", dtb_ptr);
    
    const char *dtb = (const char *)dtb_ptr;
    struct fdt_header *hdr = (struct fdt_header *)dtb;
    
    if (fdt32_to_cpu(hdr->magic) != FDT_MAGIC) {
        debug_print("ERROR: Invalid DTB magic: 0x%x\n", fdt32_to_cpu(hdr->magic));
        return;
    }
    
    uint32_t totalsize = fdt32_to_cpu(hdr->totalsize);
    uint32_t version = fdt32_to_cpu(hdr->version);
    
    debug_print("Device Tree: version=%d, size=%d bytes\n", version, totalsize);
    
    uint64_t mem_start, mem_size;
    parse_memory_node(dtb, &mem_start, &mem_size);
    
    debug_print("Device Tree parsing complete\n");
}

/* Detect number of CPUs */
int detect_nr_cpus(void) {
    // Pi 4 has 4 cores, Pi 5 has 4 cores
    // For now, return 4 (can be refined by parsing /cpus node in DT)
    return 4;
}
