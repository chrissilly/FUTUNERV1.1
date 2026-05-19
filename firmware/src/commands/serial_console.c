/*
 * Serial console — minimal command interface over USB UART.
 *
 * Lets you configure WiFi STA / view status / reboot without needing
 * to be on the dongle's AP first.
 *
 * Commands (one per line, terminated with \n):
 *   help                                — list commands
 *   status                              — connection summary
 *   wifi_connect <ssid> <password>      — join external WiFi (saves to NVS)
 *   wifi_disconnect                     — drop STA, clear saved creds
 *   wifi_status                         — STA connected? IP?
 *   reboot                              — soft restart
 *
 * Output is plain text on the same UART the ESP_LOG macros use.
 *
 * Performance / risk: dedicated task, 4KB stack, priority 3 (below CAN task).
 * Reads non-blocking with 100 ms idle delay so it doesn't starve other tasks.
 */
#include "serial_console.h"
#include "wifi/wifi_ap.h"
#include "state_machine/connection_manager.h"
#include "phase2_hil_preflight_commands.h"
#include "wifi_commands.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
/* USJ primary (BOARD_V10 / customer-stock: USB-CDC carries both stdin
 * and stdout). The USJ driver must be installed before stdin works
 * non-blocking. IDF v5.5: the non-deprecated VFS helpers live in
 * `driver/usb_serial_jtag_vfs.h`. */
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
/* UART0 primary (legacy / dev-board builds with physical RX wired). */
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

static const char *TAG = "SERIAL_CON";

#define CON_LINE_MAX 128

static void trim(char *s) {
    /* Strip trailing CR/LF/space */
    int n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) {
        s[--n] = '\0';
    }
    /* Strip leading space */
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static void cmd_help(void) {
    printf("\n[FUTUNER serial console]\n");
    printf("  help\n");
    printf("  status\n");
    printf("  wifi_connect <ssid> <password>   — LEGACY: saves + joins external WiFi\n");
    printf("  wifi_disconnect                  — LEGACY: drops STA + wipes saved creds\n");
    printf("  wifi_sta_set <ssid> <password>   — store STA creds without changing radio\n");
    printf("  wifi_mode ap|sta                 — toggle WiFi mode intent (AP always up)\n");
    printf("  wifi_clear                       — forget STA creds + force AP intent\n");
    printf("  wifi_status                      — WiFi mode + STA association snapshot\n");
    printf("  logger_start                     — begin polling ECU vars\n");
    printf("  logger_stop                      — pause polling\n");
    printf("  reboot\n\n");
}

static void cmd_status(void) {
    connection_state_t st = connection_manager_get_state();
    bool patched = connection_manager_is_patched();
    bool paired  = connection_manager_is_paired();
    const char *ssid = wifi_ap_get_ssid();
    const char *sta_ip = wifi_client_get_ip();
    bool sta_up = wifi_client_is_connected();

    printf("\n[status]\n");
    printf("  ECU state    : %s\n", connection_manager_get_state_name(st));
    printf("  patched      : %s\n", patched ? "yes" : "no");
    printf("  paired       : %s\n", paired ? "yes" : "no");
    printf("  AP SSID      : %s\n", ssid ? ssid : "(?)");
    printf("  AP IP        : 192.168.10.1\n");
    printf("  STA          : %s\n", sta_up ? "up" : "down");
    if (sta_up && sta_ip && sta_ip[0]) printf("  STA IP       : %s\n", sta_ip);
    printf("\n");
}

static void cmd_wifi_status(void) {
    /* Delegate to the WS command's handler so serial sees the same
     * spec response shape (mode/ssid/ip/sta_connected/sta_creds_stored). */
    static char resp[256];
    resp[0] = '\0';
    cmd_wifi_status2(0, NULL, resp, sizeof(resp));
    printf("%s\n", resp);
}

static void cmd_wifi_connect(char *args) {
    /* Parse: <ssid> <password>  (ssid: no spaces) */
    char *space = strchr(args, ' ');
    if (!space) { printf("Usage: wifi_connect <ssid> <password>\n"); return; }
    *space = '\0';
    char *ssid = args;
    char *pass = space + 1;
    while (*pass == ' ') pass++;
    if (!*ssid || !*pass) { printf("Usage: wifi_connect <ssid> <password>\n"); return; }

    printf("Connecting to '%s'...\n", ssid);
    esp_err_t r = wifi_client_connect(ssid, pass);
    if (r != ESP_OK) {
        printf("connect failed: %s\n", esp_err_to_name(r));
    } else {
        printf("connect requested. Watch logs for IP.\n");
    }
}

static void cmd_wifi_disconnect(void) {
    esp_err_t r = wifi_client_disconnect();
    printf("disconnect: %s\n", esp_err_to_name(r));
}

static void handle_line(char *line) {
    trim(line);
    if (!*line) return;

    /* split first word */
    char *space = strchr(line, ' ');
    char *args = "";
    if (space) {
        *space = '\0';
        args = space + 1;
        while (*args == ' ') args++;
    }

    if      (strcmp(line, "help") == 0)             cmd_help();
    else if (strcmp(line, "status") == 0)           cmd_status();
    else if (strcmp(line, "wifi_status") == 0)      cmd_wifi_status();
    else if (strcmp(line, "wifi_connect") == 0)     cmd_wifi_connect(args);
    else if (strcmp(line, "wifi_disconnect") == 0)  cmd_wifi_disconnect();
    else if (strcmp(line, "wifi_sta_set") == 0) {
        static char r[256]; r[0] = '\0';
        cmd_wifi_sta_set(0, args, r, sizeof(r));
        printf("%s\n", r);
    }
    else if (strcmp(line, "wifi_mode") == 0) {
        static char r[256]; r[0] = '\0';
        cmd_wifi_mode(0, args, r, sizeof(r));
        printf("%s\n", r);
    }
    else if (strcmp(line, "wifi_clear") == 0) {
        static char r[256]; r[0] = '\0';
        cmd_wifi_clear(0, args, r, sizeof(r));
        printf("%s\n", r);
    }
    else if (strcmp(line, "logger_start") == 0) {
        connection_manager_logger_start();
        printf("logger started\n");
    }
    else if (strcmp(line, "logger_stop") == 0) {
        connection_manager_logger_stop();
        printf("logger stopped\n");
    }
    else if (strcmp(line, "reboot") == 0) {
        printf("Rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    else if (strcmp(line, "phase2_hil_preflight") == 0) {
        /* Local USB-UART surface for the HIL preflight shadow dry-run.
         * Authenticated by physical access (USB cable); no password check.
         * Args: "" / "shadow" (only mode supported pre-go-HIL). */
        static char preflight_resp[2048];
        preflight_resp[0] = '\0';
        cmd_phase2_hil_preflight(0, args, preflight_resp, sizeof(preflight_resp));
        printf("%s\n", preflight_resp);
    }
    else if (strcmp(line, "phase2_hil_preflight_arm") == 0) {
        /* Local USB-UART surface for arming the NVS one-shot autostart.
         * Reboot the dongle after this to trigger the preflight run.
         * `args` is the bare string the user typed after the command
         * name — e.g. "prod" / "shadow" / "" (defaults to shadow). */
        static char arm_resp[512];
        arm_resp[0] = '\0';
        cmd_phase2_hil_preflight_arm(0, args, arm_resp, sizeof(arm_resp));
        printf("%s\n", arm_resp);
    }
    else {
        printf("Unknown: '%s'  (type 'help')\n", line);
    }
}

static void serial_console_task(void *arg) {
    char buf[CON_LINE_MAX];
    int pos = 0;

    /* Install the right driver for whichever console kconfig picked as
     * primary. After this, stdin/stdout flow through the chosen path
     * and the dispatcher's read(STDIN_FILENO, ...) below is config-
     * agnostic. CR-on-RX → LF-on-read so the line splitter sees '\n'
     * regardless of whether the host sent CR, LF, or CRLF. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&usj_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed; "
                      "stdin will not work over USB-CDC");
    }
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
#else
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
#endif

    /* Make stdin non-blocking-ish so we can poll */
    setvbuf(stdin, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "Serial console ready. Type 'help'.");
    printf("\n[FUTUNER serial console — type 'help']\n> ");
    fflush(stdout);

    /* Non-blocking stdin */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    while (1) {
        char c;
        int n = read(STDIN_FILENO, &c, 1);
        if (n != 1) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c == '\r' || c == '\n') {
            putchar('\n');
            buf[pos] = '\0';
            handle_line(buf);
            pos = 0;
            printf("> ");
            fflush(stdout);
        } else if (c == 0x7f || c == 0x08) {
            if (pos > 0) { pos--; printf("\b \b"); fflush(stdout); }
        } else if (pos < CON_LINE_MAX - 1 && c >= 0x20 && c < 0x7f) {
            buf[pos++] = c;
            putchar(c);
            fflush(stdout);
        }
    }
}

void serial_console_start(void) {
    xTaskCreate(serial_console_task, "serial_con", 4096, NULL, 3, NULL);
}
