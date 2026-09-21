#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"

extern "C" {
#include "esp_heap_caps.h"
}

// --- Wi-Fi Configuration ---
const char *ssid     = "ESP32-CAM-Robot";
const char *password = "12345678Level";  // at least 8 characters

httpd_handle_t cmd_httpd    = NULL;
httpd_handle_t stream_httpd = NULL;

// True while a stream handler is actively pushing frames.
static volatile bool stream_active = false;

// --- AI-Thinker camera pin map ---
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
  "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
  "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ------------------------------------------------------------------------- UI --------
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><title>ESP32-CAM Robot Controller</title>
<meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>body{font-family:sans-serif;text-align:center;background:#222;color:#fff;margin:0;padding:20px;}
.container{max-width:600px;margin:0 auto;}
.stream-window{width:100%;max-width:400px;border-radius:8px;border:3px solid #444;background:#000;min-height:200px;}
.btn-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;max-width:360px;margin:20px auto;}
.btn{background:#007bff;color:white;border:none;padding:20px;font-size:18px;font-weight:bold;border-radius:8px;cursor:pointer;}
.btn:active{background:#0056b3;}.btn.stop{background:#dc3545;}.spacer{visibility:hidden;}
#state{font-size:13px;color:#8a8;}</style></head>
<body><div class='container'><h2>ESP32-CAM Control Panel</h2>
<img class='stream-window' id='video-feed'><div id='state'>connecting...</div>
<div class='btn-grid'><div class='spacer'></div><button class='btn' onclick="send('FW')">FW</button><div class='spacer'></div>
<button class='btn' onclick="send('LEFT')">LEFT</button><button class='btn stop' onclick="send('STOP')">STOP</button><button class='btn' onclick="send('RIGHT')">RIGHT</button>
<div class='spacer'></div><button class='btn' onclick="send('REV')">REV</button><div class='spacer'></div></div></div>
<script>
var view = document.getElementById('video-feed');
var state = document.getElementById('state');
var lastRestart = 0;

function startStream(){
  var now = Date.now();
  if (now - lastRestart < 1500) return;  // debounce
  lastRestart = now;
  state.textContent = 'reconnecting...';
  view.src = 'http://' + window.location.hostname + ':81/stream?t=' + now;
}

view.onerror = function(){ setTimeout(startStream, 1200); };
view.onload  = function(){ state.textContent = 'streaming'; };

// Watchdog: an MJPEG stream that ends does not raise onerror, so ask the
// board whether its stream handler is still alive and restart if it is not.
setInterval(function(){
  fetch('/status', {cache:'no-store'})
    .then(function(r){ return r.text(); })
    .then(function(t){
      if (t.trim() == '0') { state.textContent = 'stream closed'; startStream(); }
    })
    .catch(function(){ state.textContent = 'board unreachable'; });
}, 2000);

function send(action){ fetch('/cmd?action=' + action); }
startStream();
</script></body></html>
)rawliteral";

// --------------------------------------------------------------------- handlers -------
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, stream_active ? "1" : "0", 1);
}

static esp_err_t cmd_handler(httpd_req_t *req) {
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char action[12];
        if (httpd_query_key_value(buf, "action", action, sizeof(action)) == ESP_OK) {
            String command = String(action);
            if      (command == "FW")    Serial.println("[COMMAND] Move Forward");
            else if (command == "REV")   Serial.println("[COMMAND] Move Reverse");
            else if (command == "LEFT")  Serial.println("[COMMAND] Turn Left");
            else if (command == "RIGHT") Serial.println("[COMMAND] Turn Right");
            else if (command == "STOP")  Serial.println("[COMMAND] Brake / Stop");
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[80];
    uint32_t frames = 0;
    int64_t t0 = esp_timer_get_time();

    // Refuse a second concurrent stream: the old handler owns the httpd task.
    if (stream_active) {
        Serial.println("[SYSTEM] Stream already active - rejecting duplicate client.");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream busy");
        return ESP_FAIL;
    }
    stream_active = true;

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) { stream_active = false; return res; }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "25");

    Serial.println("[SYSTEM] Video streaming channel open.");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[WARNING] Camera frame capture failed");
            vTaskDelay(20 / portTICK_PERIOD_MS);
            continue;
        }

        if (res == ESP_OK) res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

        esp_camera_fb_return(fb);
        fb = NULL;

        if (res != ESP_OK) {
            Serial.printf("[SYSTEM] Send failed (err 0x%x) after %u frames.\n", res, frames);
            break;
        }

        if (++frames % 100 == 0) {
            int64_t dt = esp_timer_get_time() - t0;
            Serial.printf("[STATS] %u frames, %.1f fps, free heap %u, free PSRAM %u\n",
                          frames, 100.0f / (dt / 1000000.0f),
                          heap_caps_get_free_size(MALLOC_CAP_8BIT),
                          heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            t0 = esp_timer_get_time();
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);  // yield to Wi-Fi + watchdog
    }

    stream_active = false;
    Serial.println("[SYSTEM] Video streaming link closed.");
    return res;
}

// ----------------------------------------------------------------------- setup --------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- ESP32-CAM streaming server ---");
    Serial.printf("Free heap at boot : %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    Serial.printf("Free PSRAM at boot: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // IMPORTANT: zero-initialise. Uninitialised members (grab_mode, fb_location,
    // sccb_i2c_port) are the most common cause of "works then dies" streaming.
    camera_config_t config = {};

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;     config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;     config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;     config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;     config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;   config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;  config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;   config.pin_reset = RESET_GPIO_NUM;

    config.xclk_freq_hz = 20000000;        // 20 MHz is the tested value for OV2640
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST; // drop stale frames instead of stalling
    config.fb_location  = CAMERA_FB_IN_PSRAM;

    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;  // drop to CIF/QVGA if your supply is marginal
        config.jpeg_quality = 12;             // 10-15 is a good range
        config.fb_count     = 2;
    } else {
        config.frame_size   = FRAMESIZE_QQVGA;
        config.jpeg_quality = 20;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return;
    }

    Serial.println("Camera initialised.");

    // Optional sensor tweaks - helps with washed-out / dark frames
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_framesize(s, config.frame_size);
        s->set_brightness(s, 0);
        s->set_saturation(s, 0);
    }

    // --- Access point ---
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password, 1, 0, 1);
    WiFi.setSleep(false);                 // no Wi-Fi power save
    // Lower TX power reduces current spikes on weak 5V supplies:
    // WiFi.setTxPower(WIFI_POWER_15dBm);
    Serial.print("Control dashboard: http://");
    Serial.println(WiFi.softAPIP());

    // --- Command server (port 80) ---
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = 80;
    server_config.ctrl_port   = 32768;
    server_config.max_open_sockets = 4;
    server_config.lru_purge_enable = true;

    if (httpd_start(&cmd_httpd, &server_config) == ESP_OK) {
        httpd_uri_t index_uri  = { .uri = "/",        .method = HTTP_GET, .handler = index_handler,  .user_ctx = NULL };
        httpd_uri_t cmd_uri    = { .uri = "/cmd",     .method = HTTP_GET, .handler = cmd_handler,    .user_ctx = NULL };
        httpd_uri_t status_uri = { .uri = "/status",  .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
        httpd_register_uri_handler(cmd_httpd, &index_uri);
        httpd_register_uri_handler(cmd_httpd, &cmd_uri);
        httpd_register_uri_handler(cmd_httpd, &status_uri);
        Serial.println("Command server on port 80.");
    }

    // --- Stream server (port 81) ---
    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port = 81;
    stream_config.ctrl_port   = 32769;
    stream_config.max_open_sockets = 2;
    stream_config.backlog_conn = 2;
    stream_config.lru_purge_enable = true;
    stream_config.recv_wait_timeout = 15;
    stream_config.send_wait_timeout = 15;

    if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
        httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        Serial.println("Stream server on port 81.");
    }
}

void loop() {
    delay(1000);
}
