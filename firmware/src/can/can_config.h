#ifndef CAN_CONFIG_H
#define CAN_CONFIG_H

/*
 * CAN Pin Configuration
 * =====================
 * CRITICAL: These pins are board-specific. The CAN transceiver TXD/RXD
 * pins are routed to different ESP32-S3 GPIOs on each PCB. Using wrong
 * pins = CAN TX timeout (ESP_ERR_TIMEOUT) with zero bus errors — no
 * signal reaches the transceiver.
 *
 * The board IDs below are CHRIS'S NUMBERING for the dongles he has on
 * hand, NOT external naming. Trust ONLY the pin numbers verified by
 * reverse-engineering the firmware that actually worked on each board.
 *
 *
 * BOARD_V10 — "v1.0" SEFI dongle (Sean's original ScorpionEFI/SEFI hardware)
 *   ESP32-S3, 16 MB flash, 8 MB PSRAM, MAC 30:ed:a0:b6:35:40
 *   TX = GPIO 21, RX = GPIO 14
 *   *** VERIFIED 2026-05-03 by extracting twai_general_config_t from
 *   the working v1.5 firmware binary (the firmware that successfully
 *   discovered the ECU on this hardware). Pattern found at DROM offset
 *   0x6cc0 (DROM segment of app0.bin): {mode=0, tx_io=21, rx_io=14,
 *   clk=-1, busoff=-1, tx_q=5, rx_q=5}.
 *
 * BOARD_REV2 — newer FUTUNER PCB (Chris's design, ~2026)
 *   16 MB flash, 8 MB PSRAM
 *   TX = GPIO 5, RX = GPIO 16
 *   Per Sean (hardware designer). NOT yet binary-verified.
 *
 * BOARD_REV1 — older 32 MB FUTUNER prototype (Chris's earlier build)
 *   32 MB flash, 16 MB PSRAM
 *   TX = GPIO 21, RX = GPIO 14
 *   (Same pins as BOARD_V10 by coincidence. The 32 MB / 16 MB part lets
 *   you tell them apart at flash_id time even though pins match.)
 *
 *
 * IMPORTANT: the previous comment block in this file claimed
 * "16 MB flash + 8 MB PSRAM = REV2 (GPIO 5/16)" — that was wrong for
 * the v1.0 SEFI dongle. The v1.0 SEFI dongle has 16 MB flash but uses
 * GPIO 21/14. NEVER guess pins from flash size alone. Verify against
 * the firmware that proved-out on the board, or against the PCB itself.
 *
 * The HW reference doc (SEFI-ECU-Flasher-Project-Reference-v3.md)
 * incorrectly states GPIO 17/18 for some board — that's neither v1.0
 * nor REV2. Ignore that doc for pin assignment.
 */

/* === SELECT BOARD === */
/* Uncomment ONE of these: */
#define BOARD_V10      /* SEFI v1.0 (Sean's dongle, MAC 30:ed:a0:b6:35:40), GPIO 21/14 */
// #define BOARD_REV1  /* Old 32MB FUTUNER prototype, GPIO 21/14 */
// #define BOARD_REV2  /* New FUTUNER PCB, GPIO 5/16 */

#if defined(BOARD_V10)
#define CAN_TX_PIN 21
#define CAN_RX_PIN 14
#elif defined(BOARD_REV1)
#define CAN_TX_PIN 21
#define CAN_RX_PIN 14
#elif defined(BOARD_REV2)
#define CAN_TX_PIN 5
#define CAN_RX_PIN 16
#else
#error "No board selected! Define BOARD_V10, BOARD_REV1, or BOARD_REV2 in can_config.h"
#endif

#define CAN_BAUDRATE_500KBPS 1

/* UDS CAN IDs — standard VAG MDG1 diagnostic addressing */
#define ECU_PHYSICAL_TX_ID 0x7E0   /* Tester → ECU */
#define ECU_PHYSICAL_RX_ID 0x7E8   /* ECU → Tester */
#define ECU_FUNCTIONAL_TX_ID 0x7DF /* Functional broadcast */

/* ISO-TP buffer size — must hold largest UDS message (firmware transfer blocks) */
#define ISOTP_BUFFER_SIZE 4096

#endif // CAN_CONFIG_H
