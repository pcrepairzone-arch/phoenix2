/*
 * uasp.h – USB Attached SCSI Protocol (UASP) Headers for RISC OS Phoenix
 * Defines Information Units (IUs) for UASP command queuing and streams
 * Supports USB 3.0+ multi-stream transfers
 * Author: R Andrews Grok 4 – 04 Feb 2026
 */

#ifndef UASP_H
#define UASP_H

#include <stdint.h>

#define UAS_IU_COMMAND      0x01
#define UAS_IU_SENSE        0x03
#define UAS_IU_RESPONSE     0x04
#define UAS_IU_TASK_MGMT    0x05
#define UAS_IU_READ_READY   0x06
#define UAS_IU_WRITE_READY  0x07

#pragma pack(1)
typedef struct {
    uint8_t  id;           // IU ID
    uint8_t  reserved[2];
    uint8_t  tag;          // Stream tag
    uint8_t  lun;
    uint8_t  cmd_len;
    uint8_t  task_prio;
    uint8_t  cmd[16];      // SCSI command
} uas_cmd_iu_t;

typedef struct {
    uint8_t  id;
    uint8_t  reserved[2];
    uint8_t  tag;
    uint8_t  status;
    uint8_t  reserved2[3];
    uint16_t sense_len;
    uint8_t  sense_data[18];
} uas_sense_iu_t;

typedef struct {
    uint8_t  id;
    uint8_t  reserved[2];
    uint8_t  tag;
    uint8_t  status;
    uint8_t  reserved2[3];
    uint32_t residue;
} uas_response_iu_t;
#pragma pack()

#endif /* UASP_H */