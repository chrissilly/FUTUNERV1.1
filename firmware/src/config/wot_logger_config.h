#ifndef WOT_LOGGER_CONFIG_H
#define WOT_LOGGER_CONFIG_H

/*
 * wot_logger_config.h — central tunables for the WOT logger feature
 * (recorder + uploader pipeline).
 *
 * Per FUTV1.1/CLAUDE.md "no magic numbers" rule, every numeric and
 * string constant the WOT logger uses lives in this header. The .c
 * files (wot_logger.c, wot_recorder.c, wot_uploader.c,
 * wot_log_commands.c) MUST NOT contain integer literals (other than
 * 0/1) or behavioral string literals.
 *
 * All defaults below are Locked 2026-05-05 (signed off by Sean).
 */

/* ------------------------------------------------------------------ */
/* Trigger / recording behavior                                        */
/* ------------------------------------------------------------------ */

/*
 * Logger variable name whose value drives WOT detection. Today this
 * is hard-coded to the canonical "wdkba" (relative throttle position
 * on Bosch MG1/MDG1/MED17). Variable name is looked up in the active
 * logger profile; missing variable disables triggering.
 *
 * TODO: Phase A migrates this to the per-variant manifest defined in
 * docs/SCALE_ARCHITECTURE_PROPOSAL.md §2.2. Do not lock as a global
 * default — different ECU families may surface throttle under
 * different names.
 *
 * Locked 2026-05-05.
 */
#define WOT_TRIGGER_VARIABLE_NAME           "wdkba"

/*
 * Throttle threshold (percent, integer) at or above which a recording
 * begins. 80% is the conventional WOT entry point and is well clear
 * of the part-throttle band typical drivers operate in.
 *
 * Locked 2026-05-05.
 */
#define WOT_TRIGGER_THRESHOLD_PERCENT       80

/*
 * Cooldown window: throttle must remain BELOW the threshold for at
 * least this many milliseconds before a recording is considered
 * complete. Bridges across momentary lifts mid-pull (gear changes).
 *
 * Locked 2026-05-05.
 */
#define WOT_TRIGGER_COOLDOWN_MS             500

/*
 * Hard cap on a single recording duration (ms). Per MISSION_SPEC §1.3
 * the maximum log length is 60 seconds. Recording is force-ended at
 * this many ms regardless of throttle state.
 *
 * Locked 2026-05-05.
 */
#define WOT_MAX_RECORD_DURATION_MS          60000

/* ------------------------------------------------------------------ */
/* Recorder buffer sizing                                              */
/* ------------------------------------------------------------------ */

/*
 * Maximum number of logger variables per sample row. Mirrors
 * LOGGER_MAX_VALUES in logger_manager.h so the recorder can never
 * receive more values than the producer can emit.
 *
 * Locked 2026-05-05.
 */
#define WOT_RECORDER_MAX_VARS_PER_SAMPLE    32

/*
 * Maximum samples retained for one recording. Sized to comfortably
 * cover WOT_MAX_RECORD_DURATION_MS at a generous nominal sample rate
 * (the actual rate is whatever the producer feeds — the gauge stream
 * delivers ~12.4 Hz today; we pad for headroom in case a future
 * variant runs faster).
 *
 * Locked 2026-05-05.
 */
#define WOT_RECORDER_MAX_SAMPLES            2048

/* ------------------------------------------------------------------ */
/* Uploader behavior                                                   */
/* ------------------------------------------------------------------ */

/*
 * Time between upload retry attempts when the queue has at least one
 * pending log and Wi-Fi STA is connected. 60 s avoids hammering the
 * server during a brief outage and keeps uploads fresh on recovery.
 *
 * Locked 2026-05-05.
 */
#define WOT_UPLOAD_RETRY_INTERVAL_MS        60000

/*
 * Per-dongle ceiling on bytes of queued (un-uploaded) log files on
 * flash. When a new log push would exceed this, the oldest queued log
 * is deleted FIFO until the new one fits. Mirrors the §7a server-side
 * 10 MB-per-VIN policy at a smaller scale appropriate for dongle
 * flash partition.
 *
 * Locked 2026-05-05.
 */
#define WOT_UPLOAD_MAX_QUEUE_BYTES          1048576

/*
 * URL path component appended to the configured cloud host when
 * uploading. The full URL is host + path. Host is configured at
 * runtime via NVS (WOT_UPLOAD_HOST_NVS_KEY) so the same firmware
 * build can target staging vs. production.
 *
 * Locked 2026-05-05.
 */
#define WOT_UPLOAD_ENDPOINT_PATH            "/api/v1/telemetry/log"

/*
 * Default cloud host used when no NVS override is set. Matches the
 * production endpoint configured in CLAUDE.md / cloud/.
 *
 * Locked 2026-05-05.
 */
#define WOT_UPLOAD_DEFAULT_HOST             "https://sillyrabbitmotorsport.com/fut"

/*
 * HTTP status code lower/upper bounds that count as a successful
 * upload (file gets deleted from flash queue). Anything outside this
 * range causes the file to be retained for the next retry.
 *
 * Locked 2026-05-05.
 */
#define WOT_UPLOAD_HTTP_OK_MIN              200
#define WOT_UPLOAD_HTTP_OK_MAX              299

/*
 * HTTP request timeout (ms). Long enough for a 4 KB body over a slow
 * cellular hotspot, short enough that a stuck server doesn't stall
 * the rest of the dongle.
 *
 * Locked 2026-05-05.
 */
#define WOT_UPLOAD_HTTP_TIMEOUT_MS          15000

/* ------------------------------------------------------------------ */
/* Filesystem locations                                                */
/* ------------------------------------------------------------------ */

/*
 * Directory on the storage partition where queued WOT logs live.
 * Files inside are gzipped CSV, named "wot_<unix_ts>.csv.gz".
 *
 * Locked 2026-05-05.
 */
#define WOT_QUEUE_DIR_PATH                  "/storage/wot"

/*
 * NVS namespace and keys for runtime overrides.
 *
 * Locked 2026-05-05.
 */
#define WOT_NVS_NAMESPACE                   "wot"
#define WOT_UPLOAD_HOST_NVS_KEY             "upload_host"

/*
 * Maximum length of a queued-log filename (basename only, no
 * directory prefix). Used for iteration callbacks that return
 * relative names.
 *
 * Locked 2026-05-05.
 */
#define WOT_QUEUE_FILENAME_MAX              96

/*
 * Maximum length of a full file path (queue dir + slash + filename).
 * Sized to comfortably hold WOT_QUEUE_DIR_PATH + "/" + a max-length
 * basename without snprintf truncation.
 *
 * Locked 2026-05-05.
 */
#define WOT_QUEUE_FULL_PATH_MAX             192

/*
 * Maximum length of a configured upload URL (host + path).
 *
 * Locked 2026-05-05.
 */
#define WOT_UPLOAD_URL_MAX                  256

/* ------------------------------------------------------------------ */
/* Internal sizing (RFC 1952 / RFC 1951 / mechanical buffer math).     */
/* These describe physical formats and CSV layout — not tunables       */
/* anyone is expected to change at runtime — but per the project       */
/* "no magic numbers" rule they live as named constants here so         */
/* feature_manager_config.h's pattern is preserved.                    */
/* Each is annotated Locked 2026-05-05.                               */
/*                                                                     */
/* ------------------------------------------------------------------ */

/* Per-column-name budget when sizing the CSV header buffer. 32-char
 * variable names plus comma separator. Locked 2026-05-05. */
#define WOT_CSV_NAME_FIELD_MAX              33

/* Padding added to the CSV header capacity estimate to absorb the
 * "timestamp_ms," prefix and trailing newline. Locked 2026-05-05. */
#define WOT_CSV_HEADER_PADDING_BYTES        16

/* Extra bytes added to the timestamp scratch buffer beyond
 * WOT_REC_PER_TS_MAX (defined in wot_recorder.c) so snprintf has
 * a NUL terminator slot. Locked 2026-05-05. */
#define WOT_CSV_TS_BUF_PAD_BYTES            2

/* RFC 1951 stored DEFLATE block max payload (0xFFFF). Locked 2026-05-05. */
#define WOT_DEFLATE_STORED_BLOCK_MAX_LEN    0xFFFF

/* RFC 1951 stored DEFLATE block framing bytes (BFINAL/BTYPE byte +
 * LEN(2) + NLEN(2) = 5). Locked 2026-05-05. */
#define WOT_DEFLATE_STORED_BLOCK_HEADER     5

/* RFC 1952 gzip footer length (CRC32 + ISIZE = 8 bytes). Locked 2026-05-05. */
#define WOT_GZIP_FOOTER_BYTES               8

/* zlib stream overhead vs. raw DEFLATE: 2 header bytes + 4 adler32
 * trailer bytes that mz_compress2 emits and we strip when wrapping
 * gzip ourselves. Locked 2026-05-05. */
#define WOT_ZLIB_FRAME_OVERHEAD_BYTES       6

/* zlib stream header size in bytes (the 2-byte CMF/FLG header
 * mz_compress2 emits at the start). Locked 2026-05-05. */
#define WOT_ZLIB_HEADER_BYTES               2

/* Default deflate compression level used by the on-target gzip
 * path (mz_compress2). Range 1–9; 6 is miniz's default balance.
 * Locked 2026-05-05. */
#define WOT_DEFLATE_COMPRESS_LEVEL          6

/* mkdir() mode bits used to create the WOT queue directory on first
 * write. 0777 lets the umask narrow it as desired (target umask is
 * typically 022). Locked 2026-05-05. */
#define WOT_QUEUE_DIR_MODE                  0777

#endif /* WOT_LOGGER_CONFIG_H */
