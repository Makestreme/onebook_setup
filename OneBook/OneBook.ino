// OneBook firmware: ESP32-S3 EPUB server reachable through a Cloudflare Worker tunnel.
// Target: Arduino-ESP32 core 2.0.18, "ESP32S3 Dev Module", Flash 8MB,
// Partition "8M with spiffs (3MB APP/1.5MB SPIFFS)", PSRAM disabled,
// USB CDC On Boot disabled, Upload Mode UART0, Core Debug Level None.
// Libraries: WiFiManager (tzapu), WebSockets (Markus Sattler), ArduinoJson 7.x.
//
// Tunnel protocol (wss://<host>/device, header "Authorization: Bearer <device secret>"):
//   Worker -> device text   {"t":"req","id":N,"m":"GET|PUT|DELETE","p":"/api/...","len":bodyBytes}
//                           {"t":"abort","id":N}
//   Device -> worker text   {"t":"hello","fw":"x.y.z"}
//                           {"t":"res","id":N,"s":status,"ct":"type","len":bodyBytes}
//                           {"t":"fail","id":N}   (body stream broke after "res")
//   Binary frames, both directions: 4-byte big-endian id + at most 8192 body bytes.
//   One request at a time. The device may send "res" (error) before a request body is complete;
//   the worker must then stop sending that body.
//
// API: GET /api/status | GET /api/books | GET /api/log
//      GET|PUT|DELETE /api/books/<name> | GET|PUT /api/meta/<name> | GET|PUT /api/bookmarks/<name>[?flash=1]

#define ARDUINOJSON_USE_LONG_LONG 1

#include <Arduino.h>
#include <algorithm>
#include <set>
#include <vector>
#include <time.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

// ---------------- Build options ----------------
#define TLS_VERIFY 1  // 0 = encrypt without verifying the Worker certificate

const char* FW_VERSION = "1.0.0";
const char* DEVICE_HOSTNAME = "onebook";
const char* PORTAL_SSID = "OneBook-Setup";
const char* PORTAL_PASS = "onebook-setup";  // WPA2 needs at least 8 characters

// ---------------- Pins ----------------
// ESP32-S3 default SPI pins. Any free GPIO works, but avoid 0, 3, 19, 20, 26-37 and 43-46.
const int PIN_SD_MOSI = 11;
const int PIN_SD_MISO = 13;
const int PIN_SD_SCK = 12;
const int PIN_SD_CS = 10;
const int PIN_LED = 48;  // WS2812 data; the built-in RGB LED on many ESP32-S3 boards (38 on some)

// ---------------- Limits and timing ----------------
const uint32_t SD_FREQ_HZ = 10000000;
const uint32_t WIFI_CONNECT_MS = 60000;
const unsigned long PORTAL_TIMEOUT_S = 300;
const uint32_t WIFI_LOST_RESTART_MS = 300000;
const uint32_t DRD_WINDOW_MS = 3000;
const uint32_t SD_RETRY_MS = 10000;
const uint32_t RECV_TIMEOUT_MS = 30000;
const uint32_t MAX_BOOK_BYTES = 20UL * 1024 * 1024;
const uint64_t MIN_FREE_BYTES = 50ULL * 1024 * 1024;
const uint32_t MAX_META_BYTES = 64UL * 1024;
const uint32_t MAX_BOOKMARK_BYTES = 16UL * 1024;
const size_t MAX_UPLOAD_NAME = 100;
const size_t CHUNK = 8192;
const size_t LOG_SIZE = 4096;
const char* BOOKS_DIR = "/books";
const char* META_DIR = "/.onebook/meta";
const char* BM_DIR = "/bm";

// Roots of every CA Cloudflare uses for free certificates (Google Trust Services, Let's Encrypt, SSL.com).
// Core 2.x TLS does not check certificate dates, so these keep working past their expiry years.
const char CA_BUNDLE[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlrsWNBCUaqxElqjANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjIwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjIwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQDO3v2m++zsFDQ8BwZabFn3GTXd98GdVarTzTukk3LvCvpt
nfbwhYBboUhSnznFt+4orO/LdmgUud+tAWyZH8QiHZ/+cnfgLFuv5AS/T3KgGjSY
6Dlo7JUle3ah5mm5hRm9iYz+re026nO8/4Piy33B0s5Ks40FnotJk9/BW9BuXvAu
MC6C/Pq8tBcKSOWIm8Wba96wyrQD8Nr0kLhlZPdcTK3ofmZemde4wj7I0BOdre7k
RXuJVfeKH2JShBKzwkCX44ofR5GmdFrS+LFjKBC4swm4VndAoiaYecb+3yXuPuWg
f9RhD1FLPD+M2uFwdNjCaKH5wQzpoeJ/u1U8dgbuak7MkogwTZq9TwtImoS1mKPV
+3PBV2HdKFZ1E66HjucMUQkQdYhMvI35ezzUIkgfKtzra7tEscszcTJGr61K8Yzo
dDqs5xoic4DSMPclQsciOzsSrZYuxsN2B6ogtzVJV+mSSeh2FnIxZyuWfoqjx5RW
Ir9qS34BIbIjMt/kmkRtWVtd9QCgHJvGeJeNkP+byKq0rxFROV7Z+2et1VsRnTKa
G73VululycslaVNVJ1zgyjbLiGH7HrfQy+4W+9OmTN6SpdTi3/UGVN4unUu0kzCq
gc7dGtxRcw1PcOnlthYhGXmy5okLdWTK1au8CcEYof/UVKGFPP0UJAOyh9OktwID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQUu//KjiOfT5nK2+JopqUVJxce2Q4wDQYJKoZIhvcNAQEMBQADggIBAB/Kzt3H
vqGf2SdMC9wXmBFqiN495nFWcrKeGk6c1SuYJF2ba3uwM4IJvd8lRuqYnrYb/oM8
0mJhwQTtzuDFycgTE1XnqGOtjHsB/ncw4c5omwX4Eu55MaBBRTUoCnGkJE+M3DyC
B19m3H0Q/gxhswWV7uGugQ+o+MePTagjAiZrHYNSVc61LwDKgEDg4XSsYPWHgJ2u
NmSRXbBoGOqKYcl3qJfEycel/FVL8/B/uWU9J2jQzGv6U53hkRrJXRqWbTKH7QMg
yALOWr7Z6v2yTcQvG99fevX4i8buMTolUVVnjWQye+mew4K6Ki3pHrTgSAai/Gev
HyICc/sgCq+dVEuhzf9gR7A/Xe8bVr2XIZYtCtFenTgCR2y59PYjJbigapordwj6
xLEokCZYCDzifqrXPW+6MYgKBesntaFJ7qBFVHvmJ2WZICGoo7z7GJa7Um8M7YNR
TOlZ4iBgxcJlkoKM8xAfDoqXvneCbT+PHV28SSe9zE8P4c52hgQjxcCMElv924Sg
JPFI/2R80L5cFtHvma3AH/vLrrw4IgYmZNralw4/KBVEqE8AyvCazM90arQ+POuV
7LXTWtiBmelDGDfrs7vRWGJB82bSj6p4lVQgw1oudCvV0b4YacCs1aTPObpRhANl
6WLAYv7YTVWW4tAR+kg0Eeye7QUd5MjWHYbL
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPluILrIPglJ209ZjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjMwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjMwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AAQfTzOHMymKoYTey8chWEGJ6ladK0uFxh1MJ7x/JlFyb+Kf1qPKzEUURout736G
jOyxfi//qXGdGIRFBEFVbivqJn+7kAHjSxm65FSWRQmx1WyRRK2EE46ajA2ADDL2
4CejQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBTB8Sa6oC2uhYHP0/EqEr24Cmf9vDAKBggqhkjOPQQDAwNpADBmAjEA9uEglRR7
VKOQFhG/hMjqb2sXnh5GmCCbn9MN2azTL818+FsuVbu/3ZL3pAzcMeGiAjEA/Jdm
ZuVDFhOD3cffL74UOO0BzrEXGhF16b0DjyZ+hOXJYKaV11RZt+cRLInUue4X
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIF3TCCA8WgAwIBAgIIeyyb0xaAMpkwDQYJKoZIhvcNAQELBQAwfDELMAkGA1UE
BhMCVVMxDjAMBgNVBAgMBVRleGFzMRAwDgYDVQQHDAdIb3VzdG9uMRgwFgYDVQQK
DA9TU0wgQ29ycG9yYXRpb24xMTAvBgNVBAMMKFNTTC5jb20gUm9vdCBDZXJ0aWZp
Y2F0aW9uIEF1dGhvcml0eSBSU0EwHhcNMTYwMjEyMTczOTM5WhcNNDEwMjEyMTcz
OTM5WjB8MQswCQYDVQQGEwJVUzEOMAwGA1UECAwFVGV4YXMxEDAOBgNVBAcMB0hv
dXN0b24xGDAWBgNVBAoMD1NTTCBDb3Jwb3JhdGlvbjExMC8GA1UEAwwoU1NMLmNv
bSBSb290IENlcnRpZmljYXRpb24gQXV0aG9yaXR5IFJTQTCCAiIwDQYJKoZIhvcN
AQEBBQADggIPADCCAgoCggIBAPkP3aMrfcvQKv7sZ4Wm5y4bunfh4/WvpOz6Sl2R
xFdHaxh3a3by/ZPkPQ/CFp4LZsNWlJ4Xg4XOVu/yFv0AYvUiCVToZRdOQbngT0aX
qhvIuG5iXmmxX9sqAn78bMrzQdjt0Oj8P2FI7bADFB0QDksZ4LtO7IZl/zbzXmcC
C52GVWH9ejjt/uIZALdvoVBidXQ8oPrIJZK0bnoix/geoeOy3ZExqysdBP+lSgQ3
6YWkMyv94tZVNHwZpEpox7Ko07fKoZOI68GXvIz5HdkihCR0xwQ9aqkpk8zruFvh
/l8lqjRYyMEjVJ0bmBHDOJx+PYZspQ9AhnwC9FwCTyjLrnGfDzrIM/4RJTXq/LrF
YD3ZfBjVsqnTdXgDciLKOsMf7yzlLqn6niy2UUb9rwPW6mBo6oUWNmuF6R7As93E
JNyAKoFBbZQ+yODJgUEAnl6/f8UImKIYLEJAs/lvOCdLToD0PYFH4Ih86hzOtXVc
US4cK38acijnALXRdMbX5J+tB5O2UzU1/Dfkw/ZdFr4hc96SCvigY2q8lpJqPvi8
ZVWb3vUNiSYE/CUapiVpy8JtynziWV+XrOvvLsi81xtZPCvM8hnIk2snYxnP/Okm
+Mpxm3+T/jRnhE6Z6/yzeAkzcLpmpnbtG3PrGqUNxCITIJRWCk4sbE6x/c+cCbqi
M+2HAgMBAAGjYzBhMB0GA1UdDgQWBBTdBAkHovV6fVJTEpKV7jiAJQ2mWTAPBgNV
HRMBAf8EBTADAQH/MB8GA1UdIwQYMBaAFN0ECQei9Xp9UlMSkpXuOIAlDaZZMA4G
A1UdDwEB/wQEAwIBhjANBgkqhkiG9w0BAQsFAAOCAgEAIBgRlCn7Jp0cHh5wYfGV
cpNxJK1ok1iOMq8bs3AD/CUrdIWQPXhq9LmLpZc7tRiRux6n+UBbkflVma8eEdBc
Hadm47GUBwwyOabqG7B52B2ccETjit3E+ZUfijhDPwGFpUenPUayvOUiaPd7nNgs
PgohyC0zrL/FgZkxdMF1ccW+sfAjRfSda/wZY52jvATGGAslu1OJD7OAUN5F7kR/
q5R4ZJjT9ijdh9hwZXT7DrkT66cPYakylszeu+1jTBi7qUD3oFRuIIhxdRjqerQ0
cuAjJ3dctpDqhiVAq+8zD8ufgr6iIPv2tS0a5sKFsXQP+8hlAqRSAUfdSSLBv9jr
a6x+3uxjMxW3IwiPxg+NQVrdjsW5j+VFP3jbutIbQLH+cU0/4IGiul607BXgk90I
H37hVZkLId6Tngr75qNJvTYw/ud3sqB1l7UtgYgXZSD32pAAn8lSzDLKNXz1PQ/Y
K9f1JmzJBjSWFupwWRoyeXkLtoh/D1JIPb9s2KJELtFOt3JY04kTlf5Eq/jXixtu
nLwsoFvVagCvXzfh1foQC5ichucmj87w7G6KVwuA406ywKBjYZC6VWg3dGq2ktuf
oYYitmUnDuy2n0Jg5GfCtdpBC8TTi2EbvPofkSvXRAdeuims2cXp71NIWuuA8ShY
Ic2wBlX7Jz9TkHCpBB5XJ7k=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICjTCCAhSgAwIBAgIIdebfy8FoW6gwCgYIKoZIzj0EAwIwfDELMAkGA1UEBhMC
VVMxDjAMBgNVBAgMBVRleGFzMRAwDgYDVQQHDAdIb3VzdG9uMRgwFgYDVQQKDA9T
U0wgQ29ycG9yYXRpb24xMTAvBgNVBAMMKFNTTC5jb20gUm9vdCBDZXJ0aWZpY2F0
aW9uIEF1dGhvcml0eSBFQ0MwHhcNMTYwMjEyMTgxNDAzWhcNNDEwMjEyMTgxNDAz
WjB8MQswCQYDVQQGEwJVUzEOMAwGA1UECAwFVGV4YXMxEDAOBgNVBAcMB0hvdXN0
b24xGDAWBgNVBAoMD1NTTCBDb3Jwb3JhdGlvbjExMC8GA1UEAwwoU1NMLmNvbSBS
b290IENlcnRpZmljYXRpb24gQXV0aG9yaXR5IEVDQzB2MBAGByqGSM49AgEGBSuB
BAAiA2IABEVuqVDEpiM2nl8ojRfLliJkP9x6jh3MCLOicSS6jkm5BBtHllirLZXI
7Z4INcgn64mMU1jrYor+8FsPazFSY0E7ic3s7LaNGdM0B9y7xgZ/wkWV7Mt/qCPg
CemB+vNH06NjMGEwHQYDVR0OBBYEFILRhXMw5zUE044CkvvlpNHEIejNMA8GA1Ud
EwEB/wQFMAMBAf8wHwYDVR0jBBgwFoAUgtGFczDnNQTTjgKS++Wk0cQh6M0wDgYD
VR0PAQH/BAQDAgGGMAoGCCqGSM49BAMCA2cAMGQCMG/n61kRpGDPYbCWe+0F+S8T
kdzt5fxQaxFGRrMcIQBiu77D5+jNB5n5DQtdcj7EqgIwH7y6C+IwJPt8bYBVCpk+
gA0z5Wajs6O7pdWLjwkspl1+4vAHCGht0nxpbl/f5Wpl
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFiTCCA3GgAwIBAgIQb77arXO9CEDii02+1PdbkTANBgkqhkiG9w0BAQsFADBO
MQswCQYDVQQGEwJVUzEYMBYGA1UECgwPU1NMIENvcnBvcmF0aW9uMSUwIwYDVQQD
DBxTU0wuY29tIFRMUyBSU0EgUm9vdCBDQSAyMDIyMB4XDTIyMDgyNTE2MzQyMloX
DTQ2MDgxOTE2MzQyMVowTjELMAkGA1UEBhMCVVMxGDAWBgNVBAoMD1NTTCBDb3Jw
b3JhdGlvbjElMCMGA1UEAwwcU1NMLmNvbSBUTFMgUlNBIFJvb3QgQ0EgMjAyMjCC
AiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBANCkCXJPQIgSYT41I57u9nTP
L3tYPc48DRAokC+X94xI2KDYJbFMsBFMF3NQ0CJKY7uB0ylu1bUJPiYYf7ISf5OY
t6/wNr/y7hienDtSxUcZXXTzZGbVXcdotL8bHAajvI9AI7YexoS9UcQbOcGV0ins
S657Lb85/bRi3pZ7QcacoOAGcvvwB5cJOYF0r/c0WRFXCsJbwST0MXMwgsadugL3
PnxEX4MN8/HdIGkWCVDi1FW24IBydm5MR7d1VVm0U3TZlMZBrViKMWYPHqIbKUBO
L9975hYsLfy/7PO0+r4Y9ptJ1O4Fbtk085zx7AGL0SDGD6C1vBdOSHtRwvzpXGk3
R2azaPgVKPC506QVzFpPulJwoxJF3ca6TvvC0PeoUidtbnm1jPx7jMEWTO6Af77w
dr5BUxIzrlo4QqvXDz5BjXYHMtWrifZOZ9mxQnUjbvPNQrL8VfVThxc7wDNY8VLS
+YCk8OjwO4s4zKTGkH8PnP2L0aPP2oOnaclQNtVcBdIKQXTbYxE3waWglksejBYS
d66UNHsef8JmAOSqg+qKkK3ONkRN0VHpvB/zagX9wHQfJRlAUW7qglFA35u5CCoG
AtUjHBPW6dvbxrB6y3snm/vg1UYk7RBLY0ulBY+6uB0rpvqR4pJSvezrZ5dtmi2f
gTIFZzL7SAg/2SW4BCUvAgMBAAGjYzBhMA8GA1UdEwEB/wQFMAMBAf8wHwYDVR0j
BBgwFoAU+y437uOEeicuzRk1sTN8/9REQrkwHQYDVR0OBBYEFPsuN+7jhHonLs0Z
NbEzfP/UREK5MA4GA1UdDwEB/wQEAwIBhjANBgkqhkiG9w0BAQsFAAOCAgEAjYlt
hEUY8U+zoO9opMAdrDC8Z2awms22qyIZZtM7QbUQnRC6cm4pJCAcAZli05bg4vsM
QtfhWsSWTVTNj8pDU/0quOr4ZcoBwq1gaAafORpR2eCNJvkLTqVTJXojpBzOCBvf
R4iyrT7gJ4eLSYwfqUdYe5byiB0YrrPRpgqU+tvT5TgKa3kSM/tKWTcWQA673vWJ
DPFs0/dRa1419dvAJuoSc06pkZCmF8NsLzjUo3KUQyxi4U5cMj29TH0ZR6LDSeeW
P4+a0zvkEdiLA9z2tmBVGKaBUfPhqBVq6+AL8BQx1rmMRTqoENjwuSfr98t67wVy
lrXEj5ZzxOhWc5y8aVFjvO9nHEMaX3cZHxj4HCUp+UmZKbaSPaKDN7EgkaibMOlq
bLQjk2UEqxHzDh1TJElTHaE/nUiSEeJ9DU/1172iWD54nR4fK/4huxoTtrEoZP2w
AgDHbICivRZQIA9ygV/MlP+7mea6kMvq+cYMwq7FGc4zoWtcu358NFcXrfA/rs3q
r5nsLFR+jM4uElZI7xc7P0peYNLcdDa8pUNjyw9bowJWCZ4kLOGGgYz+qxcs+sji
Mho6/4UIyYOf8kpIEFR3N+2ivEC+5BB09+Rbu7nzifmPQdjH5FCQNYA+HLhNkNPU
98OwoX6EyneSMSy4kLGCenROmxMmtNVQZlR4rmA=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICOjCCAcCgAwIBAgIQFAP1q/s3ixdAW+JDsqXRxDAKBggqhkjOPQQDAzBOMQsw
CQYDVQQGEwJVUzEYMBYGA1UECgwPU1NMIENvcnBvcmF0aW9uMSUwIwYDVQQDDBxT
U0wuY29tIFRMUyBFQ0MgUm9vdCBDQSAyMDIyMB4XDTIyMDgyNTE2MzM0OFoXDTQ2
MDgxOTE2MzM0N1owTjELMAkGA1UEBhMCVVMxGDAWBgNVBAoMD1NTTCBDb3Jwb3Jh
dGlvbjElMCMGA1UEAwwcU1NMLmNvbSBUTFMgRUNDIFJvb3QgQ0EgMjAyMjB2MBAG
ByqGSM49AgEGBSuBBAAiA2IABEUpNXP6wrgjzhR9qLFNoFs27iosU8NgCTWyJGYm
acCzldZdkkAZDsalE3D07xJRKF3nzL35PIXBz5SQySvOkkJYWWf9lCcQZIxPBLFN
SeR7T5v15wj4A4j3p8OSSxlUgaNjMGEwDwYDVR0TAQH/BAUwAwEB/zAfBgNVHSME
GDAWgBSJjy+j6CugFFR781a4Jl9nOAuc0DAdBgNVHQ4EFgQUiY8vo+groBRUe/NW
uCZfZzgLnNAwDgYDVR0PAQH/BAQDAgGGMAoGCCqGSM49BAMDA2gAMGUCMFXjIlbp
15IkWE8elDIPDAI2wv2sdDJO4fscgIijzPvX6yv/N33w7deedWo1dlJF4AIxAMeN
b0Igj762TVntd00pxCAgRWSGOlDGxK0tk/UYfXLtqc/ErFc2KAhl3zx5Zn6g6g==
-----END CERTIFICATE-----
)PEM";

const char LOCAL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OneBook</title>
<style>body{font-family:system-ui,sans-serif;margin:1rem;max-width:52rem}td{padding:.15rem .7rem .15rem 0}
pre{background:#111;color:#ddd;padding:.6rem;overflow:auto;max-height:24rem;font-size:.8rem}</style>
</head><body>
<h2>OneBook</h2><div id="s">Loading...</div>
<h3>Books</h3><table id="b"></table>
<h3>Log</h3><pre id="l"></pre>
<script>
const sz=n=>n>=1e9?(n/1e9).toFixed(2)+' GB':n>=1e6?(n/1e6).toFixed(1)+' MB':(n/1e3).toFixed(0)+' KB';
const $=id=>document.getElementById(id);
async function load(){
  const s=await (await fetch('/api/status')).json();
  $('s').textContent=`fw ${s.fw} | up ${s.uptime}s | heap ${s.heap} | rssi ${s.rssi} | tunnel ${s.tunnel?'up':'down'} | SD ${s.sd.ok?sz(s.sd.used)+' used of '+sz(s.sd.total):'not mounted'}`;
  const b=await (await fetch('/api/books')).json();
  const t=$('b');t.replaceChildren();
  for(const x of (b.books||[])){const r=t.insertRow();r.insertCell().textContent=x.n;r.insertCell().textContent=sz(x.s);}
  $('l').textContent=await (await fetch('/api/log')).text();
}
load();
</script></body></html>)HTML";

// ---------------- Types and state ----------------
enum JobKind { JOB_NONE, JOB_SEND, JOB_RECV };
enum RecvKind { RK_BOOK, RK_META, RK_BOOKMARK };

struct Job {
  JobKind kind = JOB_NONE;
  uint32_t id = 0;
  File file;
  String body;
  size_t total = 0;
  size_t done = 0;
  RecvKind rk = RK_BOOK;
  fs::FS* target = nullptr;
  String tmpPath;
  String finalPath;
  uint32_t lastRx = 0;
  bool flash = false;
};

Preferences prefs;
WebServer server(80);
WebSocketsClient ws;
Job job;

String cfgHost;
String cfgSecret;

bool sdOk = false;
bool usageDirty = true;
uint64_t sdTotal = 0;
uint64_t sdUsed = 0;
uint32_t sdLastTry = 0;

bool tunnelConfigured = false;
bool tunnelUp = false;
uint32_t tunnelFails = 0;

bool wifiLost = false;
uint32_t wifiLostAt = 0;
uint32_t drdClearAt = 0;
uint32_t greenUntil = 0;
uint32_t lastLed = 0xFFFFFFFF;

char logRing[LOG_SIZE];
size_t logPos = 0;
bool logWrapped = false;

uint8_t txBuf[WEBSOCKETS_MAX_HEADER_SIZE + 4 + CHUNK];

// ---------------- Log ----------------
void logMsg(const char* fmt, ...) {
  char line[192];
  int n = snprintf(line, sizeof(line), "[%lu] ", (unsigned long)(millis() / 1000));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\n';
  line[len] = '\0';
  for (size_t i = 0; i < len; i++) {
    logRing[logPos++] = line[i];
    if (logPos == LOG_SIZE) {
      logPos = 0;
      logWrapped = true;
    }
  }
  Serial.print(line);
}

String logText() {
  char* buf = (char*)malloc(LOG_SIZE + 1);
  if (!buf) return String();
  size_t n = 0;
  if (logWrapped) {
    size_t i = logPos;
    while (i < LOG_SIZE && logRing[i] != '\n') i++;  // drop the partially overwritten line
    for (i++; i < LOG_SIZE; i++) buf[n++] = logRing[i];
  }
  for (size_t i = 0; i < logPos; i++) buf[n++] = logRing[i];
  buf[n] = '\0';
  String s(buf);
  free(buf);
  return s;
}

// ---------------- LED ----------------
void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (c == lastLed) return;
  lastLed = c;
  neopixelWrite(PIN_LED, r, g, b);
}

void updateLed() {
  uint32_t now = millis();
  if (!sdOk) {
    ledSet(24, 0, 0);
  } else if (WiFi.status() != WL_CONNECTED) {
    if ((now / 500) % 2) ledSet(16, 16, 16);
    else ledSet(0, 0, 0);
  } else if (!tunnelUp) {
    ledSet(12, 0, 16);
  } else if (job.kind != JOB_NONE) {
    ledSet(24, 8, 0);
  } else if ((int32_t)(greenUntil - now) > 0) {
    ledSet(0, 24, 0);
  } else {
    ledSet(0, 0, 0);
  }
}

// ---------------- Double reset detector ----------------
bool drdCheck() {
  bool hit = prefs.getBool("drd", false);
  prefs.putBool("drd", true);
  drdClearAt = millis() + DRD_WINDOW_MS;
  return hit;
}

void drdClear() {
  prefs.putBool("drd", false);
  drdClearAt = 0;
}

void drdTick() {
  if (drdClearAt && (int32_t)(millis() - drdClearAt) > 0) drdClear();
}

// ---------------- Names and paths ----------------
int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

String urlDecode(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '%' && i + 2 < s.length()) {
      int hi = hexVal(s[i + 1]);
      int lo = hexVal(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += (char)(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += c;
  }
  return out;
}

bool hasEpubExt(const String& n) {
  String l = n;
  l.toLowerCase();
  return l.endsWith(".epub");
}

// Accepts any name a PC could have put on the card, but never a path.
bool nameOk(const String& n) {
  if (n.length() < 6 || n.length() > 200 || n[0] == '.') return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if ((uint8_t)c < 0x20 || strchr("/\\:*?\"<>|", c)) return false;
  }
  return hasEpubExt(n);
}

bool nameOkForUpload(const String& n) {
  if (!nameOk(n) || n.length() > MAX_UPLOAD_NAME) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (!isalnum((uint8_t)c) && !strchr(" ._-()[],'&+!", c)) return false;
  }
  return true;
}

// FNV-1a 64 of the lowercased name; FAT is case-insensitive.
String nameHash(const String& name) {
  String l = name;
  l.toLowerCase();
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < l.length(); i++) {
    h ^= (uint8_t)l[i];
    h *= 1099511628211ULL;
  }
  static const char hex[] = "0123456789abcdef";
  char buf[17];
  for (int i = 15; i >= 0; i--) {
    buf[i] = hex[h & 0xF];
    h >>= 4;
  }
  buf[16] = '\0';
  return String(buf);
}

String bookPath(const String& name) { return String(BOOKS_DIR) + "/" + name; }
String metaPath(const String& name) { return String(META_DIR) + "/" + nameHash(name) + ".json"; }
String bmPath(const String& name) { return String(BM_DIR) + "/" + nameHash(name) + ".json"; }

String sanitizeHost(String h) {
  h.trim();
  if (h.startsWith("https://")) h = h.substring(8);
  else if (h.startsWith("http://")) h = h.substring(7);
  int slash = h.indexOf('/');
  if (slash >= 0) h = h.substring(0, slash);
  return h;
}

// ---------------- SD card ----------------
void removeTempFiles(const char* dirPath, const char* suffix) {
  File dir = SD.open(dirPath);
  if (!dir) return;
  std::vector<String> stale;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = f.name();
    if (!f.isDirectory() && n.endsWith(suffix)) stale.push_back(n);
    f.close();
  }
  dir.close();
  for (const String& n : stale) SD.remove(String(dirPath) + "/" + n);
}

// Raw CMD0 at 400 kHz: separates "card never answers" (wiring) from "card answers but mount fails" (format).
int sdProbe() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  pinMode(PIN_SD_CS, OUTPUT);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_SD_CS, HIGH);
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
  digitalWrite(PIN_SD_CS, LOW);
  const uint8_t cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
  for (uint8_t b : cmd0) SPI.transfer(b);
  uint8_t r = 0xFF;
  for (int i = 0; i < 16 && r == 0xFF; i++) r = SPI.transfer(0xFF);
  digitalWrite(PIN_SD_CS, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  return r;
}

void logSdFailure() {
  static int lastReply = -1;
  int r = sdProbe();
  if (r == lastReply) return;
  lastReply = r;
  if (r == 0x01) logMsg("SD: card answers but has no readable FAT32 filesystem - format it FAT32 or try another card");
  else if (r == 0xFF) logMsg("SD: no reply - check MOSI/MISO not swapped, card power, CS/SCK wiring");
  else if (r == 0x00) logMsg("SD: MISO stuck low - check the MISO wire");
  else logMsg("SD: garbled reply 0x%02X - check SCK/MOSI wiring and contacts", r);
}

bool sdMount() {
  const uint32_t speeds[] = {SD_FREQ_HZ, 4000000, 1000000};
  for (uint32_t hz : speeds) {
    SD.end();
    if (!SD.begin(PIN_SD_CS, SPI, hz, "/sd", 5, false) || SD.cardType() == CARD_NONE) continue;
    SD.mkdir(BOOKS_DIR);
    SD.mkdir("/.onebook");
    SD.mkdir(META_DIR);
    removeTempFiles(BOOKS_DIR, ".part");
    removeTempFiles(META_DIR, ".tmp");
    usageDirty = true;
    logMsg("SD: mounted at %lu kHz, card %lu MB", (unsigned long)(hz / 1000), (unsigned long)(SD.cardSize() / (1024ULL * 1024ULL)));
    return true;
  }
  SD.end();
  logSdFailure();
  return false;
}

void sdRetry() {
  if (sdOk || millis() - sdLastTry < SD_RETRY_MS) return;
  sdLastTry = millis();
  sdOk = sdMount();
}

// usedBytes() can take seconds on a large card, so it is only recomputed after changes.
void refreshUsage() {
  if (!sdOk || !usageDirty) return;
  sdTotal = SD.totalBytes();
  sdUsed = SD.usedBytes();
  usageDirty = false;
}

// ---------------- JSON bodies ----------------
String jsonError(const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  String s;
  serializeJson(d, s);
  return s;
}

String buildStatusJson() {
  refreshUsage();
  JsonDocument d;
  d["name"] = "OneBook";
  d["fw"] = FW_VERSION;
  d["uptime"] = (uint32_t)(millis() / 1000);
  d["heap"] = (uint32_t)ESP.getFreeHeap();
  d["rssi"] = (int)WiFi.RSSI();
  d["ip"] = WiFi.localIP().toString();
  d["tunnel"] = tunnelUp;
  JsonObject sd = d["sd"].to<JsonObject>();
  sd["ok"] = sdOk;
  sd["total"] = sdTotal;
  sd["used"] = sdUsed;
  String s;
  serializeJson(d, s);
  return s;
}

String buildBooksJson() {
  std::set<String> metas;
  File md = SD.open(META_DIR);
  if (md) {
    for (File f = md.openNextFile(); f; f = md.openNextFile()) {
      metas.insert(String(f.name()));
      f.close();
    }
    md.close();
  }
  JsonDocument d;
  JsonArray arr = d["books"].to<JsonArray>();
  File dir = SD.open(BOOKS_DIR);
  if (dir) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String n = f.name();
      if (!f.isDirectory() && nameOk(n)) {
        JsonObject o = arr.add<JsonObject>();
        o["n"] = n;
        o["s"] = (uint32_t)f.size();
        o["m"] = metas.count(nameHash(n) + ".json") > 0;
      }
      f.close();
    }
    dir.close();
  }
  String s;
  serializeJson(d, s);
  return s;
}

// ---------------- Tunnel responses ----------------
void sendJsonText(JsonDocument& d) {
  String s;
  serializeJson(d, s);
  ws.sendTXT(s);
}

// Body bytes must already be at txBuf + header reserve + 4.
bool sendFrame(uint32_t id, size_t n) {
  uint8_t* p = txBuf + WEBSOCKETS_MAX_HEADER_SIZE;
  p[0] = (uint8_t)(id >> 24);
  p[1] = (uint8_t)(id >> 16);
  p[2] = (uint8_t)(id >> 8);
  p[3] = (uint8_t)id;
  return ws.sendBIN(txBuf, n + 4, true);
}

void sendHead(uint32_t id, int status, const char* ctype, size_t len) {
  JsonDocument d;
  d["t"] = "res";
  d["id"] = id;
  d["s"] = status;
  d["ct"] = ctype;
  d["len"] = (uint32_t)len;
  sendJsonText(d);
}

void jobReset() {
  if (job.file) job.file.close();
  job.file = File();
  job.body = String();
  job.tmpPath = String();
  job.finalPath = String();
  job.target = nullptr;
  job.flash = false;
  job.kind = JOB_NONE;
}

void jobAbort() {
  if (job.kind == JOB_RECV && job.target) {
    job.file.close();
    job.target->remove(job.tmpPath);
  }
  jobReset();
}

void respondString(uint32_t id, int status, const char* ctype, const String& body) {
  size_t len = body.length();
  sendHead(id, status, ctype, len);
  if (len == 0) return;
  if (len <= CHUNK) {
    memcpy(txBuf + WEBSOCKETS_MAX_HEADER_SIZE + 4, body.c_str(), len);
    sendFrame(id, len);
    return;
  }
  job.kind = JOB_SEND;
  job.id = id;
  job.file = File();
  job.body = body;
  job.total = len;
  job.done = 0;
}

void respondError(uint32_t id, int status, const char* msg) {
  respondString(id, status, "application/json", jsonError(msg));
}

void respondOk(uint32_t id) {
  respondString(id, 200, "application/json", String("{\"ok\":true}"));
}

void respondFile(uint32_t id, File f, const char* ctype) {
  size_t size = f.size();
  sendHead(id, 200, ctype, size);
  if (size == 0) {
    f.close();
    return;
  }
  job.kind = JOB_SEND;
  job.id = id;
  job.file = f;
  job.body = String();
  job.total = size;
  job.done = 0;
}

void pumpSend() {
  if (job.kind != JOB_SEND) return;
  size_t n = std::min(CHUNK, job.total - job.done);
  uint8_t* data = txBuf + WEBSOCKETS_MAX_HEADER_SIZE + 4;
  if (job.file) {
    if (job.file.read(data, n) != n) {
      logMsg("Send: read error");
      JsonDocument d;
      d["t"] = "fail";
      d["id"] = job.id;
      sendJsonText(d);
      jobReset();
      return;
    }
  } else {
    memcpy(data, job.body.c_str() + job.done, n);
  }
  if (!sendFrame(job.id, n)) {
    logMsg("Send: tunnel write failed");
    jobReset();
    return;
  }
  job.done += n;
  if (job.done >= job.total) jobReset();
}

// ---------------- Request bodies ----------------
void startRecv(uint32_t id, RecvKind rk, fs::FS& target, const String& tmp, const String& fin, uint32_t len, bool flash) {
  target.remove(tmp);
  File f = target.open(tmp, FILE_WRITE);
  if (!f) {
    respondError(id, 500, "cannot create file");
    return;
  }
  job.kind = JOB_RECV;
  job.id = id;
  job.file = f;
  job.rk = rk;
  job.target = &target;
  job.tmpPath = tmp;
  job.finalPath = fin;
  job.total = len;
  job.done = 0;
  job.lastRx = millis();
  job.flash = flash;
}

void finishRecv() {
  uint32_t id = job.id;
  RecvKind rk = job.rk;
  fs::FS* target = job.target;
  String tmp = job.tmpPath;
  String fin = job.finalPath;
  size_t total = job.total;
  bool flash = job.flash;
  jobReset();
  // LittleFS renames over an existing file; FAT refuses, so remove and retry.
  if (!target->rename(tmp, fin)) {
    target->remove(fin);
    if (!target->rename(tmp, fin)) {
      target->remove(tmp);
      respondError(id, 500, "rename failed");
      return;
    }
  }
  if (rk == RK_BOOK) {
    usageDirty = true;
    logMsg("Stored %s (%u bytes)", fin.c_str(), (unsigned)total);
  } else if (rk == RK_BOOKMARK && flash) {
    greenUntil = millis() + 800;
  }
  respondOk(id);
}

void onBinary(const uint8_t* payload, size_t length) {
  if (length < 4) return;
  uint32_t id = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                ((uint32_t)payload[2] << 8) | (uint32_t)payload[3];
  if (job.kind != JOB_RECV || id != job.id) return;
  size_t n = length - 4;
  if (job.done + n > job.total) {
    jobAbort();
    respondError(id, 400, "body longer than declared");
    return;
  }
  if (job.file.write(payload + 4, n) != n) {
    jobAbort();
    logMsg("Receive: write failed");
    respondError(id, 500, "write failed");
    return;
  }
  job.done += n;
  job.lastRx = millis();
  if (job.done == job.total) finishRecv();
}

// ---------------- API handlers ----------------
void handleBook(uint32_t id, const String& method, const String& name, uint32_t len) {
  if (!sdOk) {
    respondError(id, 503, "sd card not available");
    return;
  }
  String path = bookPath(name);
  if (method == "GET") {
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
      respondError(id, 404, "book not found");
      return;
    }
    respondFile(id, f, "application/epub+zip");
  } else if (method == "PUT") {
    if (!nameOkForUpload(name)) {
      respondError(id, 400, "invalid name for upload");
      return;
    }
    if (len == 0 || len > MAX_BOOK_BYTES) {
      respondError(id, 413, "invalid size");
      return;
    }
    refreshUsage();
    if (sdTotal < sdUsed + len + MIN_FREE_BYTES) {
      respondError(id, 507, "not enough space");
      return;
    }
    startRecv(id, RK_BOOK, SD, path + ".part", path, len, false);
  } else if (method == "DELETE") {
    if (!SD.exists(path)) {
      respondError(id, 404, "book not found");
      return;
    }
    if (!SD.remove(path)) {
      respondError(id, 500, "delete failed");
      return;
    }
    String meta = metaPath(name);
    if (SD.exists(meta)) SD.remove(meta);
    String bm = bmPath(name);
    if (LittleFS.exists(bm)) LittleFS.remove(bm);
    usageDirty = true;
    logMsg("Deleted %s", name.c_str());
    respondOk(id);
  } else {
    respondError(id, 405, "method not allowed");
  }
}

void handleMeta(uint32_t id, const String& method, const String& name, uint32_t len) {
  if (!sdOk) {
    respondError(id, 503, "sd card not available");
    return;
  }
  String path = metaPath(name);
  if (method == "GET") {
    File f = SD.exists(path) ? SD.open(path, FILE_READ) : File();
    if (!f) {
      respondError(id, 404, "no metadata");
      return;
    }
    respondFile(id, f, "application/json");
  } else if (method == "PUT") {
    if (!SD.exists(bookPath(name))) {
      respondError(id, 404, "book not found");
      return;
    }
    if (len == 0 || len > MAX_META_BYTES) {
      respondError(id, 413, "invalid size");
      return;
    }
    startRecv(id, RK_META, SD, path + ".tmp", path, len, false);
  } else {
    respondError(id, 405, "method not allowed");
  }
}

void handleBookmark(uint32_t id, const String& method, const String& name, uint32_t len, bool flash) {
  String path = bmPath(name);
  if (method == "GET") {
    File f = LittleFS.exists(path) ? LittleFS.open(path, FILE_READ) : File();
    if (!f) {
      respondError(id, 404, "no bookmarks");
      return;
    }
    respondFile(id, f, "application/json");
  } else if (method == "PUT") {
    if (sdOk && !SD.exists(bookPath(name))) {
      respondError(id, 404, "book not found");
      return;
    }
    if (len == 0 || len > MAX_BOOKMARK_BYTES) {
      respondError(id, 413, "invalid size");
      return;
    }
    startRecv(id, RK_BOOKMARK, LittleFS, path + ".tmp", path, len, flash);
  } else {
    respondError(id, 405, "method not allowed");
  }
}

void handleRequest(uint32_t id, const String& method, String path, uint32_t len) {
  int q = path.indexOf('?');
  bool flash = false;
  if (q >= 0) {
    flash = path.indexOf("flash=1", q) >= 0;
    path = path.substring(0, q);
  }
  logMsg("%s %s", method.c_str(), path.c_str());

  if (job.kind != JOB_NONE) {
    respondError(id, 503, "busy");
    return;
  }
  if (method == "GET" && path == "/api/status") {
    respondString(id, 200, "application/json", buildStatusJson());
    return;
  }
  if (method == "GET" && path == "/api/log") {
    respondString(id, 200, "text/plain", logText());
    return;
  }
  if (method == "GET" && path == "/api/books") {
    if (!sdOk) respondError(id, 503, "sd card not available");
    else respondString(id, 200, "application/json", buildBooksJson());
    return;
  }

  String name;
  char area;
  if (path.startsWith("/api/books/")) {
    area = 'b';
    name = urlDecode(path.substring(11));
  } else if (path.startsWith("/api/meta/")) {
    area = 'm';
    name = urlDecode(path.substring(10));
  } else if (path.startsWith("/api/bookmarks/")) {
    area = 'k';
    name = urlDecode(path.substring(15));
  } else {
    respondError(id, 404, "not found");
    return;
  }
  if (!nameOk(name)) {
    respondError(id, 400, "invalid name");
    return;
  }
  if (area == 'b') handleBook(id, method, name, len);
  else if (area == 'm') handleMeta(id, method, name, len);
  else handleBookmark(id, method, name, len, flash);
}

// ---------------- Tunnel ----------------
void onText(const uint8_t* payload, size_t length) {
  JsonDocument d;
  if (deserializeJson(d, (const char*)payload, length)) return;
  const char* t = d["t"] | "";
  uint32_t id = d["id"] | (uint32_t)0;
  if (strcmp(t, "req") == 0) {
    handleRequest(id, String(d["m"] | ""), String(d["p"] | ""), d["len"] | (uint32_t)0);
  } else if (strcmp(t, "abort") == 0 && job.kind != JOB_NONE && job.id == id) {
    logMsg("Request %lu aborted", (unsigned long)id);
    jobAbort();
  }
}

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      tunnelUp = true;
      tunnelFails = 0;
      logMsg("Tunnel: connected");
      JsonDocument d;
      d["t"] = "hello";
      d["fw"] = FW_VERSION;
      sendJsonText(d);
      break;
    }
    case WStype_DISCONNECTED:
      if (tunnelUp) logMsg("Tunnel: disconnected");
      else if (tunnelFails++ % 12 == 0) logMsg("Tunnel: connect failed (attempt %lu)", (unsigned long)tunnelFails);
      tunnelUp = false;
      jobAbort();
      break;
    case WStype_TEXT:
      onText(payload, length);
      break;
    case WStype_BIN:
      onBinary(payload, length);
      break;
    default:
      break;
  }
}

void tunnelBegin() {
  if (cfgHost.isEmpty() || cfgSecret.isEmpty()) {
    logMsg("Tunnel: host or secret not set (double-press RST to configure)");
    return;
  }
#if TLS_VERIFY
  ws.beginSslWithCA(cfgHost.c_str(), 443, "/device", CA_BUNDLE, "");
#else
  ws.beginSslWithCA(cfgHost.c_str(), 443, "/device", nullptr, "");
#endif
  ws.setExtraHeaders(("Authorization: Bearer " + cfgSecret).c_str());
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(20000, 8000, 2);
  tunnelConfigured = true;
  logMsg("Tunnel: connecting to %s", cfgHost.c_str());
}

// ---------------- Wi-Fi ----------------
bool hasSavedWiFi() {
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return false;
  return conf.sta.ssid[0] != 0;
}

bool waitForWiFi(uint32_t timeoutMs) {
  logMsg("WiFi: connecting");
  WiFi.begin();
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    drdTick();
    updateLed();
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

void runPortal() {
  drdClear();
  WiFiManagerParameter pHost("host", "Worker host, e.g. onebook.you.workers.dev", cfgHost.c_str(), 100);
  WiFiManagerParameter pSecret("secret", "Device secret (blank = keep current)", "", 64, "type='password'");
  WiFiManager wm;
  wm.setDebugOutput(false);
  std::vector<const char*> menu = {"wifi", "info", "exit"};
  wm.setMenu(menu);
  wm.addParameter(&pHost);
  wm.addParameter(&pSecret);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setBreakAfterConfig(true);
  wm.setAPCallback([](WiFiManager*) { ledSet(0, 0, 24); });
  logMsg("Portal: open as %s", PORTAL_SSID);
  wm.startConfigPortal(PORTAL_SSID, PORTAL_PASS);

  String host = sanitizeHost(String(pHost.getValue()));
  if (host != cfgHost) {
    cfgHost = host;
    prefs.putString("host", cfgHost);
    logMsg("Config: host set to %s", cfgHost.c_str());
  }
  String secret = String(pSecret.getValue());
  secret.trim();
  if (secret.length()) {
    cfgSecret = secret;
    prefs.putString("secret", cfgSecret);
    logMsg("Config: device secret updated");
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
}

void wifiWatch() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiLost) logMsg("WiFi: reconnected");
    wifiLost = false;
    return;
  }
  if (!wifiLost) {
    wifiLost = true;
    wifiLostAt = millis();
    logMsg("WiFi: lost");
  } else if (millis() - wifiLostAt > WIFI_LOST_RESTART_MS) {
    logMsg("WiFi: lost too long, restarting");
    delay(100);
    ESP.restart();
  }
}

// Cheap insurance in case the TLS stack checks certificate dates.
void waitForTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < 10000) delay(100);
}

// ---------------- Local status page (LAN only, read-only) ----------------
void serverBegin() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", LOCAL_PAGE); });
  server.on("/api/status", HTTP_GET, []() { server.send(200, "application/json", buildStatusJson()); });
  server.on("/api/log", HTTP_GET, []() { server.send(200, "text/plain", logText()); });
  server.on("/api/books", HTTP_GET, []() {
    if (!sdOk) server.send(503, "application/json", jsonError("sd card not available"));
    else server.send(200, "application/json", buildBooksJson());
  });
  server.begin();
  MDNS.begin(DEVICE_HOSTNAME);
  MDNS.addService("http", "tcp", 80);
}

// ---------------- Arduino entry points ----------------
void setup() {
  Serial.begin(115200);
  ledSet(0, 0, 0);
  logMsg("OneBook %s booting", FW_VERSION);

  prefs.begin("onebook", false);
  cfgHost = prefs.getString("host", "");
  cfgSecret = prefs.getString("secret", "");
  bool forcePortal = drdCheck();

  if (LittleFS.begin(true)) LittleFS.mkdir(BM_DIR);
  else logMsg("LittleFS: mount failed");

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = sdMount();
  sdLastTry = millis();

  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (forcePortal) logMsg("Double reset: opening portal");
  while (WiFi.status() != WL_CONNECTED) {
    if (!forcePortal && hasSavedWiFi() && waitForWiFi(WIFI_CONNECT_MS)) break;
    forcePortal = false;
    runPortal();
  }
  logMsg("WiFi: connected to %s, IP %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  serverBegin();
  waitForTime();
  tunnelBegin();
}

void loop() {
  drdTick();
  wifiWatch();
  server.handleClient();
  if (tunnelConfigured) ws.loop();
  pumpSend();
  if (job.kind == JOB_RECV && millis() - job.lastRx > RECV_TIMEOUT_MS) {
    logMsg("Receive: timed out");
    jobAbort();
  }
  sdRetry();
  updateLed();
}
