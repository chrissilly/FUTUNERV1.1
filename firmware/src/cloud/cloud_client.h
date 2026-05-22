#ifndef CLOUD_CLIENT_H
#define CLOUD_CLIENT_H

#include "esp_err.h"
#include "esp_http_client.h"

/*
 * cloud_client — single factory for every dongle → SRM cloud HTTPS
 * client. Centralizes TLS config so future knobs (cert pinning, IP
 * allowlist, retry policy, custom UA) land in exactly one place
 * instead of drifting across 3+ modules.
 *
 * Per CLAUDE.md Rule 3 (no magic numbers) and P-49: each caller used
 * to build its own esp_http_client_config_t with .crt_bundle_attach
 * inlined. P-46 was a long-latent regression because nothing
 * structurally prevented one of those sites from forgetting the
 * attach. With this factory, the only place `.crt_bundle_attach`
 * lives is cloud_client.c.
 *
 * The factory is intentionally thin: it takes the fully-resolved URL
 * + HTTP method + timeout and returns an esp_http_client_handle_t.
 * URL resolution stays in the calling module (host constant +
 * static path, both already in *_config.h). Headers (Authorization,
 * Content-Type) and request body are set by the caller after init.
 *
 * The caller owns the returned handle and is responsible for
 * esp_http_client_cleanup() — same lifecycle the raw IDF API has.
 */

/*
 * Build an HTTPS client with the canonical TLS config attached.
 *
 * Returns NULL on init failure (matches esp_http_client_init's
 * contract; callers already handle the NULL case).
 */
esp_http_client_handle_t cloud_client_https_init(const char                *url,
                                                  esp_http_client_method_t   method,
                                                  int                        timeout_ms);

#endif /* CLOUD_CLIENT_H */
