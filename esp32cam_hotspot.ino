// ESP32-CAM: creates its own Wi-Fi hotspot and streams live video
// Board: "AI Thinker ESP32-CAM" (Tools > Board > esp32 > AI Thinker ESP32-CAM)
// Connect your phone to Wi-Fi "ESP32-CAM" (password 12345678), open http://192.168.4.1

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

const char* AP_SSID = "ESP32-CAM";
const char* AP_PASS = "12345678";   // min 8 characters

// ---- AI-Thinker ESP32-CAM pins ----
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22
#define FLASH_LED_PIN    4

httpd_handle_t web_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

#define BOUNDARY "frame"
static const char* STREAM_TYPE = "multipart/x-mixed-replace;boundary=" BOUNDARY;
static const char* STREAM_SEP  = "\r\n--" BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM Live</title>
<style>
body{margin:0;background:#0b0f17;color:#e6edf3;font-family:system-ui,sans-serif;text-align:center}
h1{font-size:20px;margin:16px 0}
img{width:100%;max-width:800px;border-radius:12px;background:#000}
.row{margin:14px;display:flex;gap:10px;justify-content:center;flex-wrap:wrap}
button,select{background:#1f6feb;color:#fff;border:0;border-radius:8px;padding:10px 16px;font-size:15px}
select{background:#21262d}
</style></head><body>
<h1>&#128247; ESP32-CAM Live</h1>
<img id="cam" alt="stream">
<div class="row">
  <button onclick="fetch('/flash?on=1')">Light ON</button>
  <button onclick="fetch('/flash?on=0')">Light OFF</button>
  <select onchange="fetch('/size?v='+this.value)">
    <option value="5">QVGA 320x240</option>
    <option value="8" selected>VGA 640x480</option>
    <option value="9">SVGA 800x600</option>
    <option value="10">XGA 1024x768</option>
  </select>
</div>
<script>document.getElementById('cam').src='http://'+location.hostname+':81/stream';</script>
</body></html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static int get_query_int(httpd_req_t* req, const char* key, int def) {
  char buf[64], val[16];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK &&
      httpd_query_key_value(buf, key, val, sizeof(val)) == ESP_OK) return atoi(val);
  return def;
}

static esp_err_t flash_handler(httpd_req_t* req) {
  digitalWrite(FLASH_LED_PIN, get_query_int(req, "on", 0) ? HIGH : LOW);
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t size_handler(httpd_req_t* req) {
  sensor_t* s = esp_camera_sensor_get();
  if (s) s->set_framesize(s, (framesize_t)get_query_int(req, "v", FRAMESIZE_VGA));
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t stream_handler(httpd_req_t* req) {
  char part[64];
  esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }
    size_t hlen = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;   // viewer disconnected
  }
  return res;
}

void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  httpd_uri_t index_uri = { "/",      HTTP_GET, index_handler, NULL };
  httpd_uri_t flash_uri = { "/flash", HTTP_GET, flash_handler, NULL };
  httpd_uri_t size_uri  = { "/size",  HTTP_GET, size_handler,  NULL };
  if (httpd_start(&web_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(web_httpd, &index_uri);
    httpd_register_uri_handler(web_httpd, &flash_uri);
    httpd_register_uri_handler(web_httpd, &size_uri);
  }
  // Stream on its own server (port 81) so it doesn't block the page
  config.server_port = 81;
  config.ctrl_port += 1;
  httpd_uri_t stream_uri = { "/stream", HTTP_GET, stream_handler, NULL };
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_LATEST;
  if (psramFound()) {
    c.frame_size = FRAMESIZE_VGA; c.jpeg_quality = 12; c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.frame_size = FRAMESIZE_QVGA; c.jpeg_quality = 15; c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }

  if (esp_camera_init(&c) != ESP_OK) {
    Serial.println("Camera init FAILED - check camera ribbon cable");
    delay(3000);
    ESP.restart();
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Hotspot ready. Open http://");
  Serial.println(WiFi.softAPIP());   // 192.168.4.1

  startServers();
}

void loop() {
  delay(1000);
}
