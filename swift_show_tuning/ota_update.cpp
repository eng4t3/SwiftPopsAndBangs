// =========================================================================================
// FIRMWARE UPDATE (OTA) - see ota_update.h
// =========================================================================================
// Every path ends in the Update library writing the inactive OTA slot (default.csv:
// 2 x 1,310,720-byte app slots). While bytes go to flash the engine is inhibited (never
// cuts spark) and a failed / aborted / cancelled update always releases the inhibit.
//
//  1. Browser / laptop upload   POST /update[?md5=<32 hex>] (multipart field "update").
//     Used by the dashboard's manual file picker, by the dashboard's "one-tap" update
//     (the phone downloads firmware.bin from GitHub over its own mobile data first) and by
//     `pio run -e esp32dev_wifi -t upload` (curl).
//  2. ESP-direct online update  POST /api/ota/check | /api/ota/install
//     The ESP joins a Wi-Fi with internet (home Wi-Fi or a phone hotspot) in AP+STA mode,
//     reads version.json and streams firmware.bin from raw.githubusercontent.com over TLS
//     with real certificate validation (embedded root CAs below), then reboots. Runs in a
//     FreeRTOS task on core 0 so loop() (engine, dashboard, WebSocket) keeps running.
//
// ---- HTTP API (JSON endpoints answer HTTP 200, check "ok" / "state"; 403 = cross-site) ---
//  GET  /api/info
//       {"fw":"2.0.0","code":20000,"built":"Sep 24 2026 20:12:00","heap":123456,
//        "repo":"eng4t3/SwiftPopsAndBangs","branch":"main","staSsid":"HomeWifi",
//        "autoCheck":false,"rpm":0,"uptime":12345}  (uptime = ms since boot, uint32; the
//                                                    Wi-Fi password is never returned)
//  GET  /api/ota/status
//       {"state":"idle|connecting|checking|available|uptodate|downloading|flashing|done|error",
//        "progress":0-100,"msg":"<Hungarian>","latest":"2.1.0","latestCode":20100,
//        "current":"2.0.0","staIp":"192.168.1.50"}   (latest "" / latestCode 0 = unknown)
//  POST /api/ota/config   x-www-form-urlencoded, every field optional (omitted = unchanged):
//       ssid (<=32, empty = forget the network), pass (empty = open network, else 8..63
//       chars or 64 hex), repo ("owner/name", empty = default), branch (empty = default),
//       autoCheck (1/0)                                 -> {"ok":true} | {"ok":false,"msg":".."}
//  POST /api/ota/check    connect + read the manifest   -> {"ok":true} | {"ok":false,"msg":".."}
//  POST /api/ota/install  connect + manifest + download + flash + reboot (refused above
//                         OTA_MAX_INSTALL_RPM or if the manifest is not newer)
//  POST /api/ota/cancel   abort a running check/install unless already in "flashing";
//                         with nothing running it resets a finished state to "idle".
//  POST /update           200 "SIKERES FRISSITES! ..." then reboot; 400 bad md5 / bad or
//                         corrupt image / no file; 409 engine above OTA_MAX_INSTALL_RPM (at the
//                         start or during the transfer) or an online update running; 500 flash
//                         error. Plain text, Hungarian.
//
// Engine safety: the engine is inhibited (never cuts spark, so no rev limiter either) only
// while bytes are actually going to flash: from Update.begin() of an ACCEPTED upload /
// download until it fails or the device reboots. Rejected requests never inhibit. Above
// OTA_MAX_INSTALL_RPM an upload / download is aborted before its point of no return, and no
// check / install / boot auto-check starts (they would also take the AP off-channel).
//
// Cross-site / DNS-rebinding protection (same rule as the WebSocket handshake in the .ino):
// every state-changing route (POST /update, POST /api/ota/*) answers 403 unless
//   - the Host header's host is an IPv4 literal or a ".local" name, and
//   - if an Origin header is present: it is "http://<host>" with the same host.
// No Origin (curl, laptop upload: Host 192.168.4.1) is allowed. DEPENDENCY: WebServer only
// keeps headers named in collectHeaders(); the glue must list "Origin" there (it does).
//
// Rollback: CONFIG_APP_ROLLBACK_ENABLE + the prebuilt bootloader support it. A freshly
// installed image boots in PENDING_VERIFY; verifyRollbackLater() below stops the core from
// marking it valid before setup(), and otaLoop() / GET /api/info mark it valid once it has
// run OTA_MARK_VALID_MS with the AP up. If it crashes or reboots before that, the bootloader
// returns to the previous firmware on the next boot.
//
// Flash writes and the engine: while the flash is erased / written (otadata when an image is
// confirmed, NVS in /api/ota/config, the OTA slot itself) the engine ISRs are masked, so the
// clamp keeps its current state for those few ms. During an OTA write the engine is
// inhibited (clamp off); for the otadata write we wait for a moment without an active cut
// (bounded); NVS writes only happen when the user saves settings. Worst case: one cut is
// held a few ms longer than planned.
//
// Manifest (repo root, next to firmware.bin, generated by scripts/post_build.py):
//   {"version":"2.0.0","code":20000,"size":880000,"md5":"<32 hex>","built":"<ISO UTC>"}
//   "code" is compared against FW_VERSION_CODE (version.h).
// =========================================================================================
#include "ota_update.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "engine_control.h"
#include "version.h"

// ---- Tunables -----------------------------------------------------------------------------
#define OTA_MAX_INSTALL_RPM     1500    // above this the engine is being revved / driven
#define OTA_CONNECT_TIMEOUT_MS  20000   // manual check / install: join the Wi-Fi
#define OTA_AUTO_CONNECT_MS     12000   // boot-time auto check: keep the AP disturbance short
#define OTA_AUTO_DELAY_MS       4000    // boot-time auto check starts this long after boot
#define OTA_HTTP_TIMEOUT_MS     15000   // TCP connect / read timeout
#define OTA_STALL_TIMEOUT_MS    20000   // no bytes received during the download
#define OTA_REBOOT_DELAY_MS     2500    // lets the dashboard poll "done" before the reboot
#define OTA_TASK_STACK          12288   // TLS handshake runs on this stack
#define OTA_MANIFEST_MAX        1024
#define OTA_DL_CHUNK            4096
#define OTA_MARK_VALID_MS       10000   // healthy uptime (AP up) before a new image is kept
#define OTA_VALID_DEFER_MS      3000    // max. wait for a moment without spark cut (otaLoop)
#define OTA_VALID_WAIT_MS       300     // same, blocking, before a new update starts
#define OTA_AUTO_GIVEUP_MS      60000   // boot auto-check: give up if the engine stays revved

#define RAW_HOST_PREFIX "https://raw.githubusercontent.com/"
#define OTA_USER_AGENT  "SwiftPopsAndBangs-OTA/" FW_VERSION

static const char BUILD_STAMP[] = __DATE__ " " __TIME__;

// ---- Trusted root CAs for raw.githubusercontent.com ---------------------------------------
// Chain served in 2026-09: *.github.io (Let's Encrypt YR1) <- ISRG "Root YR" (cross-signed by
// ISRG Root X1). mbedtls stops at the first trusted certificate, so either X1 or the
// self-signed Root YR anchors it. The other roots are successors / a CA switch hedge so a
// rotation on GitHub's side does not break updates. The device does not check certificate
// dates (CONFIG_MBEDTLS_HAVE_TIME_DATE is off in Arduino-ESP32 2.0.x), so no NTP is needed
// and root expiry cannot brick updates; a new root KEY would. Manual upload always works.
// Verify / refresh:  openssl s_client -connect raw.githubusercontent.com:443 -showcerts
static const char GITHUB_ROOT_CA[] =
// ISRG Root X1 (Let's Encrypt, RSA) - anchors today's raw.githubusercontent.com chain via the Root YR cross-sign
//   SHA-256 96BCEC06264976F37460779ACF28C5A7CFE8A3C0AAE11A8FFCEE05C0BDDF08C6, valid until Jun 4 11:04:38 2035 GMT
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n"
// ISRG Root YR (Let's Encrypt Gen Y, RSA) - successor root; the YR1 intermediate chains to it directly
//   SHA-256 E57B7E6F150C419102E8D5C055729FF967B9D1A829BF00CEC89CA604EBF4A86F, valid until Sep 2 23:59:59 2045 GMT
"-----BEGIN CERTIFICATE-----\n"
"MIIFKTCCAxGgAwIBAgIRAOxGNJNgz0sP+KmC2Tqpyj0wDQYJKoZIhvcNAQELBQAw\n"
"LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n"
"HhcNMjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzEN\n"
"MAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEB\n"
"BQADggIPADCCAgoCggIBANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9\n"
"sGNiB0BD1fcOxbSUQCJIM1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGg\n"
"YbSQ4OpzI+DG8SGuTlcE873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBN\n"
"JAY+OKfX/FUvYKuhjT+no49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBzn\n"
"ZqvbNPLMXMLFxCb3WTfrJBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904\n"
"M+faKx8hnLCpJ15ZqaEgcNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawT\n"
"vSZuVvlbRrAlLxIB6pwMBjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEa\n"
"tnMdmDT5BqnKC92bd0EhM1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lz\n"
"Yal+9zTg7C5DALyVOeG/CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU\n"
"6H1qGg3DgTOuskf8eahTMiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9\n"
"IWhH4YZKh3WnJEIt+oQvlYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/B\n"
"AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud\n"
"DgQWBBTe51tg0CJtQCh9Pw0B/qS1UrRRlDANBgkqhkiG9w0BAQsFAAOCAgEAWHnf\n"
"713Bdkq7t5yN2dNIgQakUb94X9WuyhMEHHkgx4oDpSUlnG0w4g94MoqaEUE31ZjR\n"
"LU7L5LD1g9ujFHTQu8AD215AHMVQFbm6j8hQxdXHAzDajFNQnOlDJrLjzIx176oy\n"
"AjvUtejZx2NNmdb5fd0WGVGsCdoAJ3N8ozo7ajE8t6vfxStZb4BQ9WYJGHUDrv2N\n"
"i5tJF6CNiPnlzs3BUfECRbE4JSk+jvy8+VoGiFE8qsH/j78x2fjgQhAQFV7P7Zxy\n"
"dBTZ1wEkNpZNW2qnaK1SKBLa+xf6E06YRIq5uaI+HWH8SY1y5VbRgzq40EKg3yxP\n"
"06fz+uYAUIFJoLNfhwRCc3Q6pQVuMX3yAjHAes4gk4moGcLQ5p7HAh39yeylZc1J\n"
"41sx/jKwLIkPE6Rr1Nf4pxdsxf9SA4yOEiAkDgq04DVxn8hgYFdUtBCuiuVC2heA\n"
"EiqVEa+8QZjuw8Gj0EbHXcRd1nInvGqRS1o9Is7YBdQN57X1AYveGBNNqjICSb7c\n"
"awuw1EawTDrs13VUlJVEsbQ0/O/1aaV73mCdOQ8azqL2KTv1Ewu1xbquE2S+kdQU\n"
"To9TUwat3wUA6cwXh1EfpS/3fJ0aGah5hdpRyoCLDlsSn8tkrjMfFFX0viC+GxHc\n"
"sI1ANRYvqSFC2X1VRZfDg+wD6E21BccmifG4yWc=\n"
"-----END CERTIFICATE-----\n"
// ISRG Root X2 (Let's Encrypt, ECDSA) - covers a switch to ECDSA certificates
//   SHA-256 69729B8E15A86EFC177A57AFB7171DFC64ADD28C2FCA8CF1507E34453CCB1470, valid until Sep 17 16:00:00 2040 GMT
"-----BEGIN CERTIFICATE-----\n"
"MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
"CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
"R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
"MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
"ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
"EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
"+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
"ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
"AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
"zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
"tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
"/q4AaOeMSQ+2b1tbFfLn\n"
"-----END CERTIFICATE-----\n"
// ISRG Root YE (Let's Encrypt Gen Y, ECDSA) - successor of X2
//   SHA-256 E14FFCAD5B0025731006CAA43A121A22D8E9700F4FB9CF852F02A708AA5D5666, valid until Sep 2 23:59:59 2045 GMT
"-----BEGIN CERTIFICATE-----\n"
"MIIB2TCCAWCgAwIBAgIRAKQCa6LvbHwg1AR+XmWmk4AwCgYIKoZIzj0EAwMwLjEL\n"
"MAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWUUwHhcN\n"
"MjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsG\n"
"A1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAi\n"
"A2IABDwS/6vhrcVqcbBo+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7X\n"
"yFjWsPYhIPt/wzOqxTd2b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW8\n"
"7j77GaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O\n"
"BBYEFKPIJlqOoUzQNWP8myPIOq5W809WMAoGCCqGSM49BAMDA2cAMGQCMHhMr8N9\n"
"LdL1VQKs9BdV81r76eXRB6mtjuNjzk6/lBsPNToWLTDzGYgtQKO1jl63uAIwGV7m\n"
"onyF377c+MM1oqVNs17sgu7F9YKZwgLmVbeOMDbKAXHtKMDLbiGllCcs8f47\n"
"-----END CERTIFICATE-----\n"
// USERTrust RSA Certification Authority (Sectigo) - GitHub's other CA (github.com), hedge against a CA switch
//   SHA-256 E793C9B02FD8AA13E21C31228ACCB08119643B749C898964B1746D46C3D4CBD2, valid until Jan 18 23:59:59 2038 GMT
"-----BEGIN CERTIFICATE-----\n"
"MIIF3jCCA8agAwIBAgIQAf1tMPyjylGoG7xkDjUDLTANBgkqhkiG9w0BAQwFADCB\n"
"iDELMAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0pl\n"
"cnNleSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNV\n"
"BAMTJVVTRVJUcnVzdCBSU0EgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAw\n"
"MjAxMDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNV\n"
"BAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVU\n"
"aGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBSU0EgQ2Vy\n"
"dGlmaWNhdGlvbiBBdXRob3JpdHkwggIiMA0GCSqGSIb3DQEBAQUAA4ICDwAwggIK\n"
"AoICAQCAEmUXNg7D2wiz0KxXDXbtzSfTTK1Qg2HiqiBNCS1kCdzOiZ/MPans9s/B\n"
"3PHTsdZ7NygRK0faOca8Ohm0X6a9fZ2jY0K2dvKpOyuR+OJv0OwWIJAJPuLodMkY\n"
"tJHUYmTbf6MG8YgYapAiPLz+E/CHFHv25B+O1ORRxhFnRghRy4YUVD+8M/5+bJz/\n"
"Fp0YvVGONaanZshyZ9shZrHUm3gDwFA66Mzw3LyeTP6vBZY1H1dat//O+T23LLb2\n"
"VN3I5xI6Ta5MirdcmrS3ID3KfyI0rn47aGYBROcBTkZTmzNg95S+UzeQc0PzMsNT\n"
"79uq/nROacdrjGCT3sTHDN/hMq7MkztReJVni+49Vv4M0GkPGw/zJSZrM233bkf6\n"
"c0Plfg6lZrEpfDKEY1WJxA3Bk1QwGROs0303p+tdOmw1XNtB1xLaqUkL39iAigmT\n"
"Yo61Zs8liM2EuLE/pDkP2QKe6xJMlXzzawWpXhaDzLhn4ugTncxbgtNMs+1b/97l\n"
"c6wjOy0AvzVVdAlJ2ElYGn+SNuZRkg7zJn0cTRe8yexDJtC/QV9AqURE9JnnV4ee\n"
"UB9XVKg+/XRjL7FQZQnmWEIuQxpMtPAlR1n6BB6T1CZGSlCBst6+eLf8ZxXhyVeE\n"
"Hg9j1uliutZfVS7qXMYoCAQlObgOK6nyTJccBz8NUvXt7y+CDwIDAQABo0IwQDAd\n"
"BgNVHQ4EFgQUU3m/WqorSs9UgOHYm8Cd8rIDZsswDgYDVR0PAQH/BAQDAgEGMA8G\n"
"A1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEMBQADggIBAFzUfA3P9wF9QZllDHPF\n"
"Up/L+M+ZBn8b2kMVn54CVVeWFPFSPCeHlCjtHzoBN6J2/FNQwISbxmtOuowhT6KO\n"
"VWKR82kV2LyI48SqC/3vqOlLVSoGIG1VeCkZ7l8wXEskEVX/JJpuXior7gtNn3/3\n"
"ATiUFJVDBwn7YKnuHKsSjKCaXqeYalltiz8I+8jRRa8YFWSQEg9zKC7F4iRO/Fjs\n"
"8PRF/iKz6y+O0tlFYQXBl2+odnKPi4w2r78NBc5xjeambx9spnFixdjQg3IM8WcR\n"
"iQycE0xyNN+81XHfqnHd4blsjDwSXWXavVcStkNr/+XeTWYRUc+ZruwXtuhxkYze\n"
"Sf7dNXGiFSeUHM9h4ya7b6NnJSFd5t0dCy5oGzuCr+yDZ4XUmFF0sbmZgIn/f3gZ\n"
"XHlKYC6SQK5MNyosycdiyA5d9zZbyuAlJQG03RoHnHcAP9Dc1ew91Pq7P8yF1m9/\n"
"qS3fuQL39ZeatTXaw2ewh0qpKJ4jjv9cJ2vhsE/zB+4ALtRZh8tSQZXq9EfX7mRB\n"
"VXyNWQKV3WKdwrnuWih0hKWbt5DHDAff9Yk2dDLWKMGwsAvgnEzDHNb842m1R0aB\n"
"L6KCq9NjRHDEjf8tM7qtj3u1cIiuPhnPQCjY/MiQu12ZIvVS5ljFH4gxQ+6IHdfG\n"
"jjxDah2nGN59PRbxYvnKkKj9\n"
"-----END CERTIFICATE-----\n"
// USERTrust ECC Certification Authority (Sectigo) - ECDSA variant of the above
//   SHA-256 4FF460D54B9C86DABFBCFC5712E0400D2BED3FBC4D4FBDAA86E06ADCD2A9AD7A, valid until Jan 18 23:59:59 2038 GMT
"-----BEGIN CERTIFICATE-----\n"
"MIICjzCCAhWgAwIBAgIQXIuZxVqUxdJxVt7NiYDMJjAKBggqhkjOPQQDAzCBiDEL\n"
"MAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNl\n"
"eSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMT\n"
"JVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAwMjAx\n"
"MDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNVBAgT\n"
"Ck5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVUaGUg\n"
"VVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlm\n"
"aWNhdGlvbiBBdXRob3JpdHkwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAQarFRaqflo\n"
"I+d61SRvU8Za2EurxtW20eZzca7dnNYMYf3boIkDuAUU7FfO7l0/4iGzzvfUinng\n"
"o4N+LZfQYcTxmdwlkWOrfzCjtHDix6EznPO/LlxTsV+zfTJ/ijTjeXmjQjBAMB0G\n"
"A1UdDgQWBBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAOBgNVHQ8BAf8EBAMCAQYwDwYD\n"
"VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNoADBlAjA2Z6EWCNzklwBBHU6+4WMB\n"
"zzuqQhFkoJ2UOQIReVx7Hfpkue4WQrO/isIJxOzksU0CMQDpKmFHjFJKS04YcPbW\n"
"RNZu9YO6bVi9JNlWSOrvxKJGgYhqOkbRqZtNyWHa0V1Xahg=\n"
"-----END CERTIFICATE-----\n";

// ---- Shared state ---------------------------------------------------------------------------
enum OtaState : uint8_t {
  ST_IDLE, ST_CONNECTING, ST_CHECKING, ST_AVAILABLE, ST_UPTODATE,
  ST_DOWNLOADING, ST_FLASHING, ST_DONE, ST_ERROR
};
static const char* const STATE_NAMES[] = {
  "idle", "connecting", "checking", "available", "uptodate",
  "downloading", "flashing", "done", "error"
};

struct OtaStatus {
  OtaState state;
  uint8_t  progress;
  char     msg[256];
  char     latest[16];
  long     latestCode;
  char     staIp[16];
};

struct OtaConfig {
  char ssid[33];
  char pass[65];
  char repo[101];
  char branch[101];
  bool autoCheck;
};

struct Manifest {
  char version[16];
  long code;
  long size;
  char md5[33];
};

// Everything below that both the loop task (HTTP handlers) and the OTA task touch is
// guarded by `mux`. Critical sections only copy small structs / flags.
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static OtaStatus st = { ST_IDLE, 0, "Nincs folyamatban frissítés", "", 0, "" };
static OtaConfig cfg;
static bool jobRunning = false;     // OTA task alive
static bool jobInstall = false;     // parameters of the running job
static bool jobAuto = false;
static volatile bool cancelReq = false;
static bool uploadActive = false;   // a browser upload owns Update
static volatile bool holdUpload = false;  // engine inhibit requested by POST /update
static volatile bool holdJob = false;     // engine inhibit requested by the online download
static bool autoPending = false;

static WebServer* srv = nullptr;

bool otaIsBusy() { return holdUpload || holdJob; }

static void applyInhibit() { engineSetInhibit(holdUpload || holdJob); }

// ---- Rollback protection --------------------------------------------------------------------
#ifdef CONFIG_APP_ROLLBACK_ENABLE
// Overrides the weak default in cores/esp32/esp32-hal-misc.c (C linkage there). Returning
// true stops initArduino() from marking a freshly installed image valid before setup();
// requestAppValid() / confirmAppNow() do it once the image has proven itself.
extern "C" bool verifyRollbackLater() { return true; }
#endif

// All of this runs in the loop task only.
static bool appPending = false;          // running image is PENDING_VERIFY (read once at boot)
static bool appValidDone = false;        // confirmed, or nothing to confirm
static const char* appValidWhy = nullptr;  // != nullptr: confirmation requested (reason)
static uint32_t appValidDeferSince = 0;    // first time the write was postponed (active cut)

static void readAppState() {
#ifdef CONFIG_APP_ROLLBACK_ENABLE
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  appPending = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
               state == ESP_OTA_IMG_PENDING_VERIFY;
  if (appPending) Serial.println("[OTA] New firmware on trial: rollback armed until confirmed");
#endif
  appValidDone = !appPending;
}

static void writeAppValid(const char* why) {
  appValidDone = true;
#ifdef CONFIG_APP_ROLLBACK_ENABLE
  // One otadata sector erase + write: engine ISRs are masked for those few ms (see top).
  esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
  Serial.printf("[OTA] Firmware %s confirmed (%s), rollback cancelled: %s\n", FW_VERSION, why,
                esp_err_to_name(e));
#else
  (void)why;
#endif
}

// Non-blocking: remembers the request; otaLoop() writes at a moment without an active cut
// (at most OTA_VALID_DEFER_MS later).
static void requestAppValid(const char* why) {
  if (!appValidDone && !appValidWhy) appValidWhy = why;
}

static void serviceAppValid() {
  if (appValidDone || !appValidWhy) return;
  if (engineGetTelemetry().cutActive) {
    if (!appValidDeferSince) appValidDeferSince = millis() | 1;
    if (millis() - appValidDeferSince < OTA_VALID_DEFER_MS) return;
  }
  writeAppValid(appValidWhy);
}

// Blocking (short): before a new update replaces an unconfirmed image.
static void confirmAppNow(const char* why) {
  if (appValidDone) return;
  uint32_t t0 = millis();
  while (engineGetTelemetry().cutActive && millis() - t0 < OTA_VALID_WAIT_MS) delay(2);
  writeAppValid(why);
}

// ---- Cross-site / DNS-rebinding request check -----------------------------------------------
// Same rule as the WebSocket handshake in the .ino (originAllowed there).
static void hostPart(const char* s, size_t n, const char*& h, size_t& hn) {
  h = s;
  hn = 0;
  while (hn < n && s[hn] != ':' && s[hn] != '/') hn++;
}

static bool isIPv4Literal(const char* h, size_t n) {
  int dots = 0, digits = 0;
  for (size_t i = 0; i < n; i++) {
    if (h[i] == '.') {
      if (!digits) return false;
      dots++;
      digits = 0;
    } else if (isdigit((unsigned char)h[i])) {
      if (++digits > 3) return false;
    } else {
      return false;
    }
  }
  return dots == 3 && digits > 0;
}

static bool isLocalHost(const char* h, size_t n) {   // IPv4 literal or "<name>.local"
  return isIPv4Literal(h, n) || (n > 6 && strncasecmp(h + n - 6, ".local", 6) == 0);
}

// true = this state-changing request may proceed. Needs "Origin" in the glue's
// server.collectHeaders() list (see the header comment).
static bool requestAllowed() {
  String hostHdr = srv->hostHeader();
  const char* hh;
  size_t hhn;
  hostPart(hostHdr.c_str(), hostHdr.length(), hh, hhn);
  bool ok = hhn > 0 && isLocalHost(hh, hhn);   // a rebinding domain name never qualifies

  String origin = srv->header("Origin");     // "" = absent (curl, laptop upload)
  if (ok && origin.length()) {
    static const char kHttp[] = "http://";
    const size_t kLen = sizeof(kHttp) - 1;
    if (origin.length() < kLen || strncasecmp(origin.c_str(), kHttp, kLen) != 0) {
      ok = false;                                // includes "Origin: null"
    } else {
      const char* oh;
      size_t ohn;
      hostPart(origin.c_str() + kLen, origin.length() - kLen, oh, ohn);
      ok = ohn == hhn && strncasecmp(oh, hh, ohn) == 0;
    }
  }
  if (!ok) Serial.printf("[OTA] Request refused: Host \"%s\", Origin \"%s\"\n", hostHdr.c_str(), origin.c_str());
  return ok;
}

bool otaRequestAllowed() { return srv != nullptr && requestAllowed(); }

static const char CROSS_SITE_MSG[] = "Elutasítva: a kérés egy másik weboldalról érkezett";

// ---- Small helpers ----------------------------------------------------------------------------
// Drops a UTF-8 sequence cut in half by a truncating snprintf (Hungarian text has accents).
static void utf8Trim(char* s) {
  size_t n = strlen(s), i = n;
  while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
  if (i == 0) return;
  unsigned char lead = (unsigned char)s[i - 1];
  size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (n - (i - 1) < need) s[i - 1] = 0;
}

static void setStatusV(OtaState s, int progress, bool log, const char* fmt, va_list ap) {
  char buf[sizeof(st.msg)];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  utf8Trim(buf);
  portENTER_CRITICAL(&mux);
  st.state = s;
  if (progress >= 0) st.progress = (uint8_t)progress;
  memcpy(st.msg, buf, sizeof(buf));
  portEXIT_CRITICAL(&mux);
  if (log) Serial.printf("[OTA] %s %d%%: %s\n", STATE_NAMES[s], progress, buf);
}

static void setStatus(OtaState s, int progress, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  setStatusV(s, progress, true, fmt, ap);
  va_end(ap);
}

static void setProgress(OtaState s, int progress, const char* fmt, ...) {  // no serial spam
  va_list ap; va_start(ap, fmt);
  setStatusV(s, progress, (progress % 10) == 0, fmt, ap);
  va_end(ap);
}

static OtaState currentState() {
  portENTER_CRITICAL(&mux);
  OtaState s = st.state;
  portEXIT_CRITICAL(&mux);
  return s;
}

static OtaConfig copyConfig() {
  OtaConfig c;
  portENTER_CRITICAL(&mux);
  c = cfg;
  portEXIT_CRITICAL(&mux);
  return c;
}

static void setStaIp(const char* ip) {
  portENTER_CRITICAL(&mux);
  strlcpy(st.staIp, ip, sizeof(st.staIp));
  portEXIT_CRITICAL(&mux);
}

// JSON string escaping into a bounded buffer (never splits an escape sequence).
static void jsonEsc(char* out, size_t cap, const char* in) {
  size_t o = 0;
  for (; *in; ++in) {
    unsigned char c = (unsigned char)*in;
    char tmp[8];
    const char* rep = nullptr;
    if (c == '"') rep = "\\\"";
    else if (c == '\\') rep = "\\\\";
    else if (c == '\n') rep = "\\n";
    else if (c == '\r') rep = "\\r";
    else if (c == '\t') rep = "\\t";
    else if (c < 0x20 || c == 0x7f) { snprintf(tmp, sizeof(tmp), "\\u%04x", c); rep = tmp; }
    if (rep) {
      size_t n = strlen(rep);
      if (o + n >= cap) break;
      memcpy(out + o, rep, n);
      o += n;
    } else {
      if (o + 1 >= cap) break;
      out[o++] = (char)c;
    }
  }
  out[o] = 0;
  utf8Trim(out);
}

static void sendJson(const char* body, int code = 200) {
  srv->sendHeader("Cache-Control", "no-store");
  srv->send(code, "application/json; charset=utf-8", body);
}

static void sendOk() { sendJson("{\"ok\":true}"); }

static void sendFail(const char* msg, int code = 200) {
  char esc[480];
  jsonEsc(esc, sizeof(esc), msg);
  char body[520];
  snprintf(body, sizeof(body), "{\"ok\":false,\"msg\":\"%s\"}", esc);
  sendJson(body, code);
}

// For the POST /api/ota/* handlers: answers 403 and returns true for a cross-site request.
static bool rejectCrossSite() {
  if (requestAllowed()) return false;
  sendFail(CROSS_SITE_MSG, 403);
  return true;
}

static void sendText(int code, const char* msg) {
  srv->sendHeader("Connection", "close");
  srv->send(code, "text/plain; charset=utf-8", msg);
}

static bool isHexStr(const char* s, size_t len) {
  if (strlen(s) != len) return false;
  for (size_t i = 0; i < len; i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

static void lowerStr(char* s) { for (; *s; ++s) *s = (char)tolower((unsigned char)*s); }

// GitHub "owner/name": letters, digits, '-', '_', '.' (owner: letters, digits, '-').
static bool validRepo(const char* s) {
  size_t n = strlen(s);
  if (n < 3 || n > 100) return false;
  const char* slash = strchr(s, '/');
  if (!slash || slash == s || !slash[1] || strchr(slash + 1, '/')) return false;
  for (const char* p = s; *p; ++p) {
    char c = *p;
    bool ok = isalnum((unsigned char)c) || c == '-' || (p > slash && (c == '_' || c == '.')) || p == slash;
    if (!ok) return false;
  }
  return strcmp(slash + 1, ".") != 0 && strcmp(slash + 1, "..") != 0;
}

// Branch names we accept: URL-safe subset of git ref names.
static bool validBranch(const char* s) {
  size_t n = strlen(s);
  if (n < 1 || n > 100) return false;
  if (s[0] == '/' || s[0] == '-' || s[0] == '.' || s[n - 1] == '/' || s[n - 1] == '.') return false;
  if (strstr(s, "..") || strstr(s, "//")) return false;
  for (const char* p = s; *p; ++p) {
    char c = *p;
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '/')) return false;
  }
  return true;
}

// Hungarian description of the Update library's last error.
static void updateErrorText(char* out, size_t cap) {
  const char* hu;
  switch (Update.getError()) {
    case UPDATE_ERROR_MD5:          hu = "Az MD5 ellenőrzőösszeg nem egyezik (sérült vagy más fájl)"; break;
    case UPDATE_ERROR_MAGIC_BYTE:   hu = "Ez nem ESP32 firmware (.bin) fájl"; break;
    case UPDATE_ERROR_SIZE:
    case UPDATE_ERROR_SPACE:        hu = "A firmware nem fér el az OTA partíción"; break;
    case UPDATE_ERROR_WRITE:
    case UPDATE_ERROR_ERASE:
    case UPDATE_ERROR_READ:         hu = "Flash írási hiba"; break;
    case UPDATE_ERROR_ACTIVATE:     hu = "Az új firmware nem aktiválható (hibás kép)"; break;
    case UPDATE_ERROR_NO_PARTITION: hu = "Nincs OTA partíció"; break;
    case UPDATE_ERROR_ABORT:        hu = "Hiányos vagy megszakított írás"; break;
    default:                        hu = "Frissítési hiba"; break;
  }
  snprintf(out, cap, "%s [%s]", hu, Update.errorString());
}

// ---- Persistent settings (NVS namespace "ota") ------------------------------------------------
static void loadConfig() {
  OtaConfig c;
  memset(&c, 0, sizeof(c));
  Preferences p;
  if (p.begin("ota", false)) {
    if (p.isKey("ssid"))   p.getString("ssid", c.ssid, sizeof(c.ssid));
    if (p.isKey("pass"))   p.getString("pass", c.pass, sizeof(c.pass));
    if (p.isKey("repo"))   p.getString("repo", c.repo, sizeof(c.repo));
    if (p.isKey("branch")) p.getString("branch", c.branch, sizeof(c.branch));
    c.autoCheck = p.getBool("auto", false);
    p.end();
  }
  if (!validRepo(c.repo)) strlcpy(c.repo, FW_REPO_DEFAULT, sizeof(c.repo));
  if (!validBranch(c.branch)) strlcpy(c.branch, FW_BRANCH_DEFAULT, sizeof(c.branch));
  portENTER_CRITICAL(&mux);
  cfg = c;
  portEXIT_CRITICAL(&mux);
}

static void handleConfig() {
  if (rejectCrossSite()) return;
  OtaConfig c = copyConfig();
  bool chSsid = false, chPass = false, chRepo = false, chBranch = false, chAuto = false;

  if (srv->hasArg("ssid")) {
    String v = srv->arg("ssid");
    if (v.length() > 32) return sendFail("Az SSID legfeljebb 32 karakter lehet");
    strlcpy(c.ssid, v.c_str(), sizeof(c.ssid));
    chSsid = true;
    if (!c.ssid[0]) { c.pass[0] = 0; chPass = true; }  // forget the network completely
  }
  if (srv->hasArg("pass")) {
    String v = srv->arg("pass");
    size_t n = v.length();
    if (n > 64 || (n > 0 && n < 8) || (n == 64 && !isHexStr(v.c_str(), 64)))
      return sendFail("A Wi-Fi jelszó 8-63 karakter legyen (nyílt hálózatnál üres)");
    strlcpy(c.pass, v.c_str(), sizeof(c.pass));
    chPass = true;
  }
  if (srv->hasArg("repo")) {
    String v = srv->arg("repo");
    v.trim();
    if (v.length() == 0) v = FW_REPO_DEFAULT;
    if (v.length() > 100 || !validRepo(v.c_str()))
      return sendFail("Érvénytelen repo. Formátum: tulajdonos/név (pl. " FW_REPO_DEFAULT ")");
    strlcpy(c.repo, v.c_str(), sizeof(c.repo));
    chRepo = true;
  }
  if (srv->hasArg("branch")) {
    String v = srv->arg("branch");
    v.trim();
    if (v.length() == 0) v = FW_BRANCH_DEFAULT;
    if (v.length() > 100 || !validBranch(v.c_str()))
      return sendFail("Érvénytelen ág (branch) név");
    strlcpy(c.branch, v.c_str(), sizeof(c.branch));
    chBranch = true;
  }
  if (srv->hasArg("autoCheck")) {
    String v = srv->arg("autoCheck");
    v.trim();
    v.toLowerCase();
    if (v == "1" || v == "true" || v == "on") c.autoCheck = true;
    else if (v == "0" || v == "false" || v == "off" || v == "") c.autoCheck = false;
    else return sendFail("autoCheck értéke 1 vagy 0 lehet");
    chAuto = true;
  }

  // NVS flash write: engine ISRs are masked for a few ms, the clamp keeps its state meanwhile
  // (benign, see the top of the file). Only happens when the user saves update settings.
  Preferences p;
  if (!p.begin("ota", false)) return sendFail("NVS hiba: a beállítás nem menthető");
  bool ok = true;
  // putString returns the number of bytes written (0 for an empty string is not an error)
  if (chSsid)   ok &= (p.putString("ssid", c.ssid) == strlen(c.ssid));
  if (chPass)   ok &= (p.putString("pass", c.pass) == strlen(c.pass));
  if (chRepo)   ok &= (p.putString("repo", c.repo) == strlen(c.repo));
  if (chBranch) ok &= (p.putString("branch", c.branch) == strlen(c.branch));
  if (chAuto)   ok &= (p.putBool("auto", c.autoCheck) == 1);
  p.end();

  portENTER_CRITICAL(&mux);
  cfg = c;   // takes effect for the next check / install even if NVS failed
  portEXIT_CRITICAL(&mux);

  if (!ok) return sendFail("NVS hiba: a beállítás csak újraindulásig marad meg");
  sendOk();
}

// ---- Info / status ------------------------------------------------------------------------------
static void handleInfo() {
  requestAppValid("/api/info served");   // the dashboard reached us: the new image works
  OtaConfig c = copyConfig();
  char ssidE[200], repoE[120], branchE[120];
  jsonEsc(ssidE, sizeof(ssidE), c.ssid);
  jsonEsc(repoE, sizeof(repoE), c.repo);
  jsonEsc(branchE, sizeof(branchE), c.branch);
  char body[660];
  snprintf(body, sizeof(body),
           "{\"fw\":\"%s\",\"code\":%d,\"built\":\"%s\",\"heap\":%u,\"repo\":\"%s\","
           "\"branch\":\"%s\",\"staSsid\":\"%s\",\"autoCheck\":%s,\"rpm\":%d,\"uptime\":%lu}",
           FW_VERSION, FW_VERSION_CODE, BUILD_STAMP, (unsigned)ESP.getFreeHeap(), repoE,
           branchE, ssidE, c.autoCheck ? "true" : "false", engineGetRpm(),
           (unsigned long)(uint32_t)millis());
  sendJson(body);
}

static void handleStatus() {
  OtaStatus s;
  portENTER_CRITICAL(&mux);
  s = st;
  portEXIT_CRITICAL(&mux);
  char msgE[480], latestE[40], ipE[24];
  jsonEsc(msgE, sizeof(msgE), s.msg);
  jsonEsc(latestE, sizeof(latestE), s.latest);
  jsonEsc(ipE, sizeof(ipE), s.staIp);
  char body[720];
  snprintf(body, sizeof(body),
           "{\"state\":\"%s\",\"progress\":%u,\"msg\":\"%s\",\"latest\":\"%s\",\"latestCode\":%ld,"
           "\"current\":\"%s\",\"staIp\":\"%s\"}",
           STATE_NAMES[s.state], (unsigned)s.progress, msgE, latestE, s.latestCode, FW_VERSION, ipE);
  sendJson(body);
}

// ---- Engine speed gate --------------------------------------------------------------------------
// true (and a Hungarian reason in err) when the engine is above OTA_MAX_INSTALL_RPM.
static bool rpmTooHigh(char* err, size_t errLen, bool running) {
  int rpm = engineGetRpm();
  if (rpm <= OTA_MAX_INSTALL_RPM) return false;
  if (running)
    snprintf(err, errLen, "Megszakítva: a motor fordulatszáma túl magas (%d RPM, max. %d)", rpm,
             OTA_MAX_INSTALL_RPM);
  else
    snprintf(err, errLen, "A motor jár (%d RPM). Frissíteni / ellenőrizni csak alapjáraton vagy "
             "álló motorral lehet (max. %d RPM).", rpm, OTA_MAX_INSTALL_RPM);
  return true;
}

// ---- Station (internet) connection --------------------------------------------------------------
static bool staConnect(const OtaConfig& c, uint32_t timeoutMs, char* err, size_t errLen) {
  setStatus(ST_CONNECTING, 0, "Csatlakozás a(z) „%s” Wi-Fi hálózathoz...", c.ssid);
  // AP stays up. Note: the AP follows the STA channel, phones may drop for a moment.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(c.ssid, c.pass[0] ? c.pass : nullptr);

  uint32_t t0 = millis();
  wl_status_t s;
  while ((s = WiFi.status()) != WL_CONNECTED) {
    if (cancelReq) { strlcpy(err, "Megszakítva", errLen); return false; }
    if (rpmTooHigh(err, errLen, true)) return false;   // don't keep the AP off-channel while driving
    uint32_t el = millis() - t0;
    if (s == WL_CONNECT_FAILED && el > 3000) {
      snprintf(err, errLen, "Nem sikerült csatlakozni a(z) „%s” hálózathoz: hibás jelszó?", c.ssid);
      return false;
    }
    if (el > timeoutMs) {
      if (s == WL_NO_SSID_AVAIL)
        snprintf(err, errLen, "A(z) „%s” Wi-Fi nem található. Be van kapcsolva a hotspot / hatótávon belül vagy?", c.ssid);
      else
        snprintf(err, errLen, "Időtúllépés (%u mp): nem sikerült csatlakozni a(z) „%s” hálózathoz (jelszó? térerő?)",
                 (unsigned)(timeoutMs / 1000), c.ssid);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  IPAddress ip = WiFi.localIP();
  if (ip[0] == 192 && ip[1] == 168 && ip[2] == 4) {
    snprintf(err, errLen, "IP ütközés: a(z) „%s” hálózat is a 192.168.4.x tartományt használja", c.ssid);
    return false;
  }
  setStaIp(ip.toString().c_str());
  Serial.printf("[OTA] STA connected, IP %s, channel %d\n", ip.toString().c_str(), WiFi.channel());
  return true;
}

static void staDisconnect() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);   // false: keep the radio (and the AP) on
  WiFi.mode(WIFI_AP);
  setStaIp("");
}

// ---- HTTPS (raw.githubusercontent.com) ----------------------------------------------------------
static bool httpOpen(HTTPClient& http, WiFiClientSecure& tls, const char* url, const char* what,
                     char* err, size_t errLen) {
  tls.setCACert(GITHUB_ROOT_CA);       // certificate chain + host name are verified
  tls.setHandshakeTimeout(OTA_HTTP_TIMEOUT_MS / 1000);
  http.setReuse(false);
  http.useHTTP10(true);                // no chunked transfer encoding, plain Content-Length
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setUserAgent(OTA_USER_AGENT);
  if (!http.begin(tls, url)) {
    snprintf(err, errLen, "Érvénytelen URL: %s", url);
    return false;
  }
  Serial.printf("[OTA] GET %s (heap %u, max block %u)\n", url, (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  int code = http.GET();
  if (code == HTTP_CODE_OK) return true;

  if (code < 0) {
    char tlsErr[96] = "";
    tls.lastError(tlsErr, sizeof(tlsErr));
    snprintf(err, errLen, "Kapcsolódási hiba (%s): %s%s%s", what, HTTPClient::errorToString(code).c_str(),
             tlsErr[0] ? " / " : "", tlsErr);
  } else if (code == HTTP_CODE_NOT_FOUND) {
    snprintf(err, errLen, "%s nem található a GitHubon (HTTP 404). Jó a repo / ág? Fel van töltve?", what);
  } else if (code == HTTP_CODE_FORBIDDEN || code == HTTP_CODE_TOO_MANY_REQUESTS) {
    snprintf(err, errLen, "A GitHub átmenetileg korlátozza a letöltést (HTTP %d), próbáld később", code);
  } else {
    snprintf(err, errLen, "HTTP hiba %d (%s)", code, what);
  }
  http.end();
  return false;
}

// Minimal JSON lookups for the flat manifest object.
static const char* jsonValue(const char* j, const char* key) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(j, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p != ':') return nullptr;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

static bool jsonString(const char* j, const char* key, char* out, size_t cap) {
  const char* p = jsonValue(j, key);
  if (!p || *p != '"') return false;
  p++;
  size_t n = 0;
  while (*p && *p != '"') {
    if (*p == '\\' || n + 1 >= cap) return false;
    out[n++] = *p++;
  }
  if (*p != '"') return false;
  out[n] = 0;
  return true;
}

static bool jsonLong(const char* j, const char* key, long& v) {
  const char* p = jsonValue(j, key);
  if (!p) return false;
  char* e;
  long x = strtol(p, &e, 10);
  if (e == p) return false;
  v = x;
  return true;
}

static bool fetchManifest(const OtaConfig& c, Manifest& m, char* err, size_t errLen) {
  char url[300];
  snprintf(url, sizeof(url), RAW_HOST_PREFIX "%s/%s/version.json", c.repo, c.branch);
  WiFiClientSecure tls;
  HTTPClient http;
  if (!httpOpen(http, tls, url, "version.json", err, errLen)) return false;

  int len = http.getSize();
  if (len > OTA_MANIFEST_MAX) {
    http.end();
    snprintf(err, errLen, "A version.json túl nagy (%d bájt)", len);
    return false;
  }
  char buf[OTA_MANIFEST_MAX + 1];
  size_t n = 0;
  WiFiClient* s = http.getStreamPtr();
  uint32_t last = millis();
  while (n < OTA_MANIFEST_MAX && (len < 0 || (int)n < len)) {
    if (cancelReq) { http.end(); strlcpy(err, "Megszakítva", errLen); return false; }
    int a = s->available();
    if (a > 0) {
      int r = s->read((uint8_t*)buf + n, min((size_t)a, (size_t)OTA_MANIFEST_MAX - n));
      if (r > 0) { n += r; last = millis(); }
    } else if (!s->connected()) {
      break;
    } else if (millis() - last > OTA_HTTP_TIMEOUT_MS) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  http.end();
  buf[n] = 0;
  if (len >= 0 && (int)n != len) {
    snprintf(err, errLen, "A version.json letöltése hiányos (%u / %d bájt)", (unsigned)n, len);
    return false;
  }

  memset(&m, 0, sizeof(m));
  bool ok = jsonString(buf, "version", m.version, sizeof(m.version)) &&
            jsonLong(buf, "code", m.code) && jsonLong(buf, "size", m.size) &&
            jsonString(buf, "md5", m.md5, sizeof(m.md5));
  if (ok) {
    lowerStr(m.md5);
    ok = m.version[0] && m.code > 0 && m.size > 0 && isHexStr(m.md5, 32);
  }
  if (!ok) {
    strlcpy(err, "Hibás version.json (version / code / size / md5 hiányzik vagy érvénytelen)", errLen);
    return false;
  }
  Serial.printf("[OTA] Manifest: %s (code %ld), %ld bytes, md5 %s\n", m.version, m.code, m.size, m.md5);
  return true;
}

// Streams firmware.bin straight into the inactive OTA slot. The engine is inhibited from the
// first flash write until reboot; on any failure the inhibit is released.
static bool downloadAndFlash(const OtaConfig& c, const Manifest& m, char* err, size_t errLen) {
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part) { strlcpy(err, "Nincs OTA partíció", errLen); return false; }
  if ((uint32_t)m.size > part->size) {
    snprintf(err, errLen, "A firmware (%ld bájt) nagyobb, mint az OTA partíció (%u bájt)",
             m.size, (unsigned)part->size);
    return false;
  }

  char url[300];
  snprintf(url, sizeof(url), RAW_HOST_PREFIX "%s/%s/firmware.bin", c.repo, c.branch);
  setStatus(ST_DOWNLOADING, 0, "Kapcsolódás: firmware.bin (%s)...", m.version);
  WiFiClientSecure tls;
  HTTPClient http;
  if (!httpOpen(http, tls, url, "firmware.bin", err, errLen)) return false;

  int len = http.getSize();
  if (len >= 0 && len != m.size) {
    http.end();
    snprintf(err, errLen, "A firmware.bin mérete (%d) eltér a version.json-tól (%ld). "
             "Friss feltöltés után a GitHub ~5 percig a régit adhatja: próbáld újra később.", len, m.size);
    return false;
  }
  uint8_t* buf = (uint8_t*)malloc(OTA_DL_CHUNK);
  if (!buf) { http.end(); strlcpy(err, "Kevés a szabad memória", errLen); return false; }

  // From here on every byte goes to flash: never cut spark.
  holdJob = true;
  applyInhibit();

  bool ok = Update.begin((size_t)m.size);
  if (ok) {
    Update.setMD5(m.md5);
  } else {
    updateErrorText(err, errLen);
  }

  size_t got = 0;
  uint32_t last = millis();
  int lastPct = -1;
  WiFiClient* s = http.getStreamPtr();
  while (ok && got < (size_t)m.size) {
    if (cancelReq) { strlcpy(err, "Megszakítva", errLen); ok = false; break; }
    // Driving off while downloading: the limiter is off (inhibit) -> abort, keep the old image.
    if (rpmTooHigh(err, errLen, true)) { ok = false; break; }
    int a = s->available();
    if (a > 0) {
      size_t want = min((size_t)a, min((size_t)OTA_DL_CHUNK, (size_t)m.size - got));
      int r = s->read(buf, want);
      if (r > 0) {
        if (Update.write(buf, (size_t)r) != (size_t)r) { updateErrorText(err, errLen); ok = false; break; }
        got += (size_t)r;
        last = millis();
        int pct = (int)((uint64_t)got * 100 / (uint64_t)m.size);
        if (pct != lastPct) {
          lastPct = pct;
          setProgress(ST_DOWNLOADING, pct, "Letöltés és írás: %d%% (%u / %ld kB)", pct,
                      (unsigned)(got / 1024), m.size / 1024);
        }
      }
      vTaskDelay(1);   // let IDLE0 run (task watchdog)
    } else if (!s->connected()) {
      snprintf(err, errLen, "A kapcsolat megszakadt letöltés közben (%u / %ld bájt)", (unsigned)got, m.size);
      ok = false;
    } else if (millis() - last > OTA_STALL_TIMEOUT_MS) {
      snprintf(err, errLen, "A letöltés elakadt (%u / %ld bájt)", (unsigned)got, m.size);
      ok = false;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  free(buf);
  http.end();

  if (ok && rpmTooHigh(err, errLen, true)) ok = false;   // last chance before "flashing"
  if (ok) {
    // Point of no return: from "flashing" on /api/ota/cancel is refused. Checked atomically
    // with the cancel handler so an accepted cancel is never ignored.
    portENTER_CRITICAL(&mux);
    bool cancelled = cancelReq;
    if (!cancelled) { st.state = ST_FLASHING; st.progress = 100; strlcpy(st.msg, "Ellenőrzés (MD5) és aktiválás...", sizeof(st.msg)); }
    portEXIT_CRITICAL(&mux);
    if (cancelled) { strlcpy(err, "Megszakítva", errLen); ok = false; }
  }
  if (ok && !Update.end()) {   // verifies MD5 + image, then selects the new boot partition
    updateErrorText(err, errLen);
    if (Update.getError() == UPDATE_ERROR_MD5)
      strlcat(err, ". Friss feltöltés után várj ~5 percet (GitHub gyorsítótár).", errLen);
    ok = false;
  }
  if (!ok) {
    if (Update.isRunning()) Update.abort();
    holdJob = false;
    applyInhibit();
  }
  return ok;
}

// ---- Background job (check / install) -----------------------------------------------------------
static void otaTask(void*) {
  OtaConfig c = copyConfig();
  bool install, automatic;
  portENTER_CRITICAL(&mux);
  install = jobInstall;
  automatic = jobAuto;
  portEXIT_CRITICAL(&mux);

  char err[sizeof(st.msg)] = "";
  bool ok = false;
  OtaState endState = ST_ERROR;
  char endMsg[sizeof(st.msg)] = "";
  Manifest m;

  do {
    if (!staConnect(c, automatic ? OTA_AUTO_CONNECT_MS : OTA_CONNECT_TIMEOUT_MS, err, sizeof(err))) break;

    setStatus(ST_CHECKING, 0, "Verzióinformáció letöltése (%s, %s ág)...", c.repo, c.branch);
    if (!fetchManifest(c, m, err, sizeof(err))) break;
    portENTER_CRITICAL(&mux);
    strlcpy(st.latest, m.version, sizeof(st.latest));
    st.latestCode = m.code;
    portEXIT_CRITICAL(&mux);

    bool newer = m.code > FW_VERSION_CODE;
    if (!newer) {
      ok = true;
      endState = ST_UPTODATE;
      if (m.code == FW_VERSION_CODE)
        snprintf(endMsg, sizeof(endMsg), "A legfrissebb verzió fut (%s)", FW_VERSION);
      else
        snprintf(endMsg, sizeof(endMsg), "A GitHubon régebbi verzió van (%s), a jelenlegi %s marad",
                 m.version, FW_VERSION);
      break;
    }
    if (!install) {
      ok = true;
      endState = ST_AVAILABLE;
      snprintf(endMsg, sizeof(endMsg), "Új verzió elérhető: %s (jelenleg: %s)", m.version, FW_VERSION);
      break;
    }

    if (rpmTooHigh(err, sizeof(err), true)) break;
    if (!downloadAndFlash(c, m, err, sizeof(err))) break;

    // Success: the new image is the boot partition. Engine stays inhibited until the reboot.
    setStatus(ST_DONE, 100, "Sikeres frissítés: %s. Újraindulás...", m.version);
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    ESP.restart();
  } while (false);

  staDisconnect();

  if (ok) {
    setStatus(endState, 0, "%s", endMsg);
  } else if (cancelReq) {
    setStatus(ST_IDLE, 0, "Megszakítva");
  } else if (automatic) {
    setStatus(ST_IDLE, 0, "Automatikus ellenőrzés nem sikerült: %s", err);
  } else {
    setStatus(ST_ERROR, 0, "%s", err);
  }

  Serial.printf("[OTA] task done, stack headroom %u bytes, free heap %u\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)ESP.getFreeHeap());
  portENTER_CRITICAL(&mux);
  jobRunning = false;
  cancelReq = false;
  portEXIT_CRITICAL(&mux);
  vTaskDelete(nullptr);
}

static bool startJob(bool install, bool automatic, char* why, size_t whyLen) {
  OtaConfig c = copyConfig();
  if (!c.ssid[0]) {
    strlcpy(why, "Nincs megadva internetes Wi-Fi (otthoni Wi-Fi vagy telefonos hotspot). Add meg a beállításoknál.", whyLen);
    return false;
  }
  // Check, install and the boot auto-check all take the AP off-channel: never while driving.
  if (rpmTooHigh(why, whyLen, false)) return false;
  portENTER_CRITICAL(&mux);
  bool busy = jobRunning || uploadActive;
  if (!busy) {
    jobRunning = true;
    jobInstall = install;
    jobAuto = automatic;
    cancelReq = false;
    st.state = ST_CONNECTING;
    st.progress = 0;
    strlcpy(st.msg, "Indítás...", sizeof(st.msg));
  }
  portEXIT_CRITICAL(&mux);
  if (busy) {
    strlcpy(why, "Már folyamatban van egy frissítési művelet", whyLen);
    return false;
  }
  // Core 0 (with the Wi-Fi stack): loop() and the engine run on core 1 undisturbed.
  if (xTaskCreatePinnedToCore(otaTask, "ota", OTA_TASK_STACK, nullptr, 1, nullptr, 0) != pdPASS) {
    portENTER_CRITICAL(&mux);
    jobRunning = false;
    portEXIT_CRITICAL(&mux);
    setStatus(ST_ERROR, 0, "Nem sikerült elindítani a frissítést (kevés memória)");
    strlcpy(why, "Nem sikerült elindítani a frissítést (kevés memória)", whyLen);
    return false;
  }
  return true;
}

static void handleCheck() {
  if (rejectCrossSite()) return;
  char why[200];
  if (startJob(false, false, why, sizeof(why))) sendOk(); else sendFail(why);
}

static void handleInstall() {
  if (rejectCrossSite()) return;
  char why[200];
  if (!startJob(true, false, why, sizeof(why))) return sendFail(why);
  // Never replace an unconfirmed image (the download starts seconds later, after Wi-Fi + TLS).
  confirmAppNow("install requested");
  sendOk();
}

static void handleCancel() {
  if (rejectCrossSite()) return;
  bool running, refused = false;
  portENTER_CRITICAL(&mux);
  running = jobRunning;
  if (running) {
    if (st.state == ST_FLASHING || st.state == ST_DONE) refused = true;
    else cancelReq = true;
  }
  portEXIT_CRITICAL(&mux);
  if (refused) return sendFail("A firmware véglegesítése már folyamatban, nem szakítható meg");
  if (!running && currentState() != ST_DONE) setStatus(ST_IDLE, 0, "Nincs folyamatban frissítés");
  sendOk();
}

// ---- Browser / laptop upload (POST /update) -----------------------------------------------------
// WebServer handles the whole multipart body inside one handleClient() call, so loop() is
// blocked meanwhile; the engine (v2) runs from ISRs and keeps working. The engine is
// inhibited only from Update.begin() of an ACCEPTED upload until it fails / the reboot. A
// rejected upload (403 / 409 / 400) just drains the body with the engine fully active, and
// an accepted one is aborted (inhibit released) if the RPM rises above the limit.
static int  upHttp = 0;          // != 0: request rejected / failed with this HTTP status
static char upMsg[256];
static bool upSeen = false;      // UPLOAD_FILE_START happened for this request
static bool upOk = false;        // Update.end() succeeded
static int  upLastPct = -1;

static void upRelease() {
  if (uploadActive) {
    if (Update.isRunning()) Update.abort();
    portENTER_CRITICAL(&mux);
    uploadActive = false;
    portEXIT_CRITICAL(&mux);
    digitalWrite(PIN_STATUS_LED, LOW);
  }
  holdUpload = false;
  applyInhibit();
}

static void upReject(int http, const char* msg) {   // status of a running online job untouched
  if (upHttp == 0) { upHttp = http; strlcpy(upMsg, msg, sizeof(upMsg)); utf8Trim(upMsg); }
  Serial.printf("[OTA] Upload rejected (%d): %s\n", http, msg);
}

static void upFail(int http, const char* msg) {   // accepted upload failed: release at once
  upReject(http, msg);
  if (uploadActive) setStatus(ST_ERROR, 0, "%s", upMsg);
  upRelease();
}

static void handleUploadChunk() {
  HTTPUpload& up = srv->upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      upSeen = true;
      upOk = false;
      upLastPct = -1;
      upHttp = 0;
      upMsg[0] = 0;

      // Rejections below never touch the engine: the body is drained with the limiter active.
      if (!requestAllowed()) { upReject(403, CROSS_SITE_MSG); return; }
      char m[200];
      if (rpmTooHigh(m, sizeof(m), false)) { upReject(409, m); return; }
      char md5[40] = "";
      String md5Arg = srv->arg("md5");   // "" when absent or empty = no MD5 check
      md5Arg.trim();
      if (md5Arg.length()) {
        strlcpy(md5, md5Arg.c_str(), sizeof(md5));
        lowerStr(md5);
        if (md5Arg.length() != 32 || !isHexStr(md5, 32)) {
          upReject(400, "Érvénytelen md5 paraméter (32 hexadecimális karakter kell)");
          return;
        }
      }
      portENTER_CRITICAL(&mux);
      bool busy = jobRunning || uploadActive;
      if (!busy) uploadActive = true;
      portEXIT_CRITICAL(&mux);
      if (busy) { upReject(409, "Már folyamatban van egy online frissítés. Várd meg vagy szakítsd meg."); return; }

      confirmAppNow("upload accepted");     // never replace an unconfirmed image
      // Accepted: from here on bytes go to flash -> never cut spark until failure / reboot.
      holdUpload = true;
      applyInhibit();
      digitalWrite(PIN_STATUS_LED, HIGH);   // solid LED while flashing (loop is blocked)
      Serial.printf("[OTA] Upload started: %s%s%s\n", up.filename.c_str(), md5[0] ? ", md5 " : "", md5);
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        updateErrorText(m, sizeof(m));
        upFail(500, m);
        return;
      }
      if (md5[0]) Update.setMD5(md5);
      setStatus(ST_FLASHING, 0, "Feltöltés és írás: %s", up.filename.c_str());
      break;
    }
    case UPLOAD_FILE_WRITE: {
      if (upHttp || !uploadActive) return;   // rejected: just drain the body
      {
        char m[200];
        if (rpmTooHigh(m, sizeof(m), true)) { upFail(409, m); return; }   // car driven off
      }
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        char m[200];
        updateErrorText(m, sizeof(m));
        upFail(Update.getError() == UPDATE_ERROR_MAGIC_BYTE ? 400 : 500, m);
        return;
      }
      int total = srv->clientContentLength();
      if (total > 0) {
        int pct = (int)((uint64_t)up.totalSize * 100 / (uint64_t)total);
        if (pct > 99) pct = 99;
        if (pct != upLastPct) {
          upLastPct = pct;
          setProgress(ST_FLASHING, pct, "Feltöltés és írás: %u kB", (unsigned)(up.totalSize / 1024));
        }
      }
      break;
    }
    case UPLOAD_FILE_END: {
      if (upHttp || !uploadActive) return;
      if (up.totalSize == 0) { upFail(400, "Üres fájl"); return; }
      if (!Update.end(true)) {
        char m[200];
        updateErrorText(m, sizeof(m));
        uint8_t e = Update.getError();
        upFail((e == UPDATE_ERROR_MD5 || e == UPDATE_ERROR_MAGIC_BYTE || e == UPDATE_ERROR_ACTIVATE) ? 400 : 500, m);
        return;
      }
      upOk = true;
      setStatus(ST_DONE, 100, "Sikeres feltöltés (%u bájt). Újraindulás...", (unsigned)up.totalSize);
      break;
    }
    case UPLOAD_FILE_ABORTED: {
      // Client went away; the final handler is not called for this request.
      Serial.println("[OTA] Upload aborted by the client");
      if (uploadActive) setStatus(ST_ERROR, 0, "A feltöltés megszakadt");
      upRelease();
      upSeen = false;
      upHttp = 0;
      break;
    }
  }
}

static void handleUploadDone() {
  if (!upSeen) {
    holdUpload = false;
    applyInhibit();
    if (!requestAllowed()) return sendText(403, CROSS_SITE_MSG);
    return sendText(400, "Nincs feltöltött fájl (multipart/form-data, mező neve: update)");
  }
  upSeen = false;
  if (upHttp || !upOk) {
    int code = upHttp ? upHttp : 500;
    upHttp = 0;
    upRelease();
    return sendText(code, upMsg[0] ? upMsg : "Hiányos feltöltés");
  }
  sendText(200, "SIKERES FRISSITES! A vezerlo ujraindul...");
  delay(500);
  ESP.restart();
}

// ---- Setup / loop -------------------------------------------------------------------------------
void otaLoop() {
  // Keep a freshly installed image once it has run for a while with the AP up (loop() is
  // alive, the dashboard is reachable). Done early on purpose: the ignition may be switched
  // off soon after an update, and a later spurious rollback would be confusing.
  if (!appValidDone && millis() > OTA_MARK_VALID_MS && (WiFi.getMode() & WIFI_MODE_AP)) {
    requestAppValid("uptime with AP up");
  }
  serviceAppValid();   // performs a requested confirmation outside an active spark cut

  if (autoPending && millis() > OTA_AUTO_DELAY_MS) {
    char why[200];
    if (rpmTooHigh(why, sizeof(why), false)) {
      // Engine revved right after power-up: try again later, give up after a minute.
      if (millis() > OTA_AUTO_GIVEUP_MS) {
        autoPending = false;
        Serial.printf("[OTA] Auto check skipped: %s\n", why);
      }
    } else {
      autoPending = false;
      if (!startJob(false, true, why, sizeof(why))) Serial.printf("[OTA] Auto check skipped: %s\n", why);
    }
  }
}

void otaBegin(WebServer& server) {
  srv = &server;
  readAppState();
  loadConfig();
  {
    OtaConfig c = copyConfig();
    autoPending = c.autoCheck && c.ssid[0];
  }

  // Fallback direct browser upload form at /update (works without JavaScript)
  server.on("/update", HTTP_GET, []() {
    srv->send(200, "text/html; charset=utf-8",
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Swift OTA Frissítés</title>"
      "<style>body{background:#0a0e17;color:#00f0ff;font-family:sans-serif;padding:24px;text-align:center;}"
      "input,button{padding:12px;margin:10px;border-radius:8px;border:none;font-size:16px;}"
      "input[type=file]{background:#162032;color:#fff;}"
      "input[type=submit]{background:#00ff88;color:#000;font-weight:bold;cursor:pointer;}"
      "</style></head><body>"
      "<h2>📡 Swift Show Tuning - Firmware OTA</h2>"
      "<p>Jelenlegi verzió: " FW_VERSION "</p>"
      "<p>Válaszd ki a lefordított .bin fájlt a frissítéshez:</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update' accept='.bin'><br>"
      "<input type='submit' value='Feltöltés és Telepítés'>"
      "</form></body></html>");
  });

  server.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);

  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/ota/status", HTTP_GET, handleStatus);
  server.on("/api/ota/config", HTTP_POST, handleConfig);
  server.on("/api/ota/check", HTTP_POST, handleCheck);
  server.on("/api/ota/install", HTTP_POST, handleInstall);
  server.on("/api/ota/cancel", HTTP_POST, handleCancel);
}
