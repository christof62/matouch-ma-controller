#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Arduino_GFX_Library.h>
#include <ESP32Encoder.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>

#include "config.h"
#include "cst816.h"
#include "ma_client.h"

// ------------------------------------------------------------------
// Hardware
// ------------------------------------------------------------------
Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;
CST816 touch;
ESP32Encoder encoder;

// ------------------------------------------------------------------
// Music Assistant
// ------------------------------------------------------------------
MusicAssistantClient ma;

char ma_host[64] = "";
uint16_t ma_port = MA_WS_PORT_DEFAULT;
char ma_token[512] = "";  // MA tokens are long JWTs (~370 chars)
char ma_port_str[8];

// ------------------------------------------------------------------
// UI state
// ------------------------------------------------------------------
enum Screen { SCR_PLAYER, SCR_VOLUME, SCR_PLAYERLIST, SCR_MENU };
Screen screen = SCR_PLAYER;
int menuIndex = 0;
int volumeTarget = 50;
volatile bool uiDirty = true;
uint32_t volumeShownAt = 0;

// ------------------------------------------------------------------
// Colors / theme (RGB565)
// ------------------------------------------------------------------
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COL_BG      RGB565(0x00, 0x00, 0x00)
#define COL_FG      RGB565(0xFF, 0xFF, 0xFF)
#define COL_ACCENT  RGB565(0x37, 0xC6, 0xF5)  // cyan
#define COL_ACCENT2 RGB565(0x2E, 0x7C, 0xF6)  // blue
#define COL_DIM     RGB565(0x80, 0x80, 0x80)
#define COL_PLAY    RGB565(0x35, 0xC0, 0x5A)  // green
#define COL_PAUSE   RGB565(0xF5, 0xB9, 0x3C)  // amber
#define COL_STOP    RGB565(0xF5, 0x53, 0x3C)  // red
#define COL_RING_OFF RGB565(0x18, 0x18, 0x18)
#define TWO_PI_F 6.2831853f

// ------------------------------------------------------------------
// Forward decls
// ------------------------------------------------------------------
void drawScreen();
void drawPlayerScreen();
void drawVolumeScreen();
void updateVolumeScreen(bool force);
void drawPlayerList();
void drawMenu();
void drawMessage(const char *msg);
void handleInput();
void saveConfig();
void loadConfig();
void saveSelectedPlayer();
void restoreSelectedPlayer();
void markDirty() { uiDirty = true; }
void drawVolumeOverlay();
void fetchArtwork();
bool drawArtwork();          // returns true when artwork is on screen
bool decodeArtworkToBuffer();  // decode jpeg into the circular buffer (once)

JPEGDEC jpeg;
uint8_t *artworkJpeg = nullptr;
size_t artworkJpegLen = 0;
String artworkLoadedFor = "";
uint16_t *artworkBuf = nullptr;   // decoded, circular-clipped RGB565 (PSRAM)
bool artworkBufValid = false;
bool takeDirty() {
  if (uiDirty) { uiDirty = false; return true; }
  return false;
}

// ------------------------------------------------------------------
// Arc drawing (thick arc via line segments, no exotic GFX API)
// ------------------------------------------------------------------
void drawThickArc(int cx, int cy, int r, float a0, float a1, int thick, uint16_t color) {
  const float step = 0.0349f;  // ~2 degrees in radians
  for (int t = 0; t < thick; t++) {
    int rr = r - t;
    float prevx = cx + rr * cosf(a0);
    float prevy = cy + rr * sinf(a0);
    for (float a = a0 + step; a <= a1 + 0.0001f; a += step) {
      float x = cx + rr * cosf(a);
      float y = cy + rr * sinf(a);
      gfx->drawLine((int)prevx, (int)prevy, (int)x, (int)y, color);
      prevx = x;
      prevy = y;
    }
  }
}

void drawVolumeArc(int cx, int cy, int r, int vol, int thick, uint16_t on, uint16_t off) {
  float aOn = vol * 0.0628f;  // 3.6 deg per percent in radians
  if (aOn > 0.01f) drawThickArc(cx, cy, r, -1.5708f, -1.5708f + aOn, thick, on);
  if (aOn < TWO_PI_F - 0.01f)
    drawThickArc(cx, cy, r, -1.5708f + aOn, 4.7124f, thick, off);
}

// ------------------------------------------------------------------
// Setup
// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(GC9A01_BL, OUTPUT);
  digitalWrite(GC9A01_BL, HIGH);

  bus = new Arduino_ESP32SPI(GC9A01_DC, GC9A01_CS, GC9A01_SCLK, GC9A01_MOSI, GC9A01_MISO);
  gfx = new Arduino_GC9A01(bus, GC9A01_RES, 0 /* rotation */, true /* IPS */);
  gfx->begin(GC9A01_SPI_FREQ);
  gfx->fillScreen(BLACK);

  touch.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

  pinMode(ENC_SW, INPUT_PULLUP);
  // FullQuad counts only complete detent steps -> robust against
  // contact bounce that made the count jump/fall back to 0
  encoder.attachFullQuad(ENC_DT, ENC_CLK);
  encoder.setCount(0);
  encoder.setFilter(1023);  // enable hardware filter/debounce if available

  loadConfig();
  strlcpy(ma_port_str, String(ma_port).c_str(), sizeof(ma_port_str));
  Serial.printf("[CFG] loaded host='%s' port=%u token len=%d\n",
                ma_host, ma_port, (int)strlen(ma_token));

  {
    // show config incl. token length on the display (no serial needed)
    gfx->fillScreen(BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(40, 85);
    gfx->println("Config:");
    gfx->setTextColor(COL_FG);
    gfx->setTextSize(1);
    gfx->setCursor(30, 115);
    gfx->println(String("host: ") + ma_host);
    gfx->setCursor(30, 130);
    gfx->println(String("port: ") + ma_port);
    gfx->setCursor(30, 145);
    gfx->println(String("token len: ") + (int)strlen(ma_token));
    delay(3000);
  }

  WiFiManager wm;
  WiFiManagerParameter p_host("ma_host", "MA Server (IP/Hostname)", ma_host, 64);
  WiFiManagerParameter p_port("ma_port", "MA Port (default 8095)", ma_port_str, 8);
  WiFiManagerParameter p_token("ma_token", "MA Token", ma_token, 512);
  wm.addParameter(&p_host);
  wm.addParameter(&p_port);
  wm.addParameter(&p_token);

  drawMessage("Connecting WiFi...");

  bool portalWasUsed = false;
  bool connected;
  if (strlen(ma_host) == 0 || strlen(ma_token) < 100) {
    // host missing OR token too short (form truncates long JWTs):
    // force the config portal; the token can then be set via
    // http://192.168.4.1/set?token=<full JWT> (bypasses the HTML form)
    // no MA server configured yet: force the config portal
    Serial.println("[CFG] host empty or token too short -> starting config portal");
    drawMessage("Setup portal...");
    // setWebServerCallback runs right after the portal webserver is created
    wm.setWebServerCallback([&]() {
      wm.server->on("/set", HTTP_GET, [&]() {
        String token = wm.server->arg("token");
        String host = wm.server->arg("host");
        String port = wm.server->arg("port");
        String html = "<html><body><h2>";
        if (token.length() > 0) {
          strlcpy(ma_token, token.c_str(), sizeof(ma_token));
          html += "Token saved! Len: " + String(token.length());
        } else {
          html += "ERROR: no token param";
        }
        if (host.length() > 0) strlcpy(ma_host, host.c_str(), sizeof(ma_host));
        if (port.length() > 0) ma_port = (uint16_t)port.toInt();
        saveConfig();
        html += "<br>host: " + String(ma_host);
        html += "<br>port: " + String(ma_port);
        html += "<br><b>Saved. Reset the device (RST) to apply.</b></h2></body></html>";
        wm.server->send(200, "text/html", html);
      });
    });
    connected = wm.startConfigPortal("MA-Controller");
    portalWasUsed = true;
  } else {
    connected = wm.autoConnect("MA-Controller");
  }

  // ALWAYS save the parameters right after the portal/autoconnect returns,
  // BEFORE any reboot decision - otherwise user input is lost when WiFi
  // connect fails and the device restarts.
  if (portalWasUsed) {
    strlcpy(ma_host, p_host.getValue(), sizeof(ma_host));
    // trim whitespace
    char *h = ma_host; while (*h == ' ') memmove(h, h + 1, strlen(h));
    while (strlen(ma_host) && ma_host[strlen(ma_host) - 1] == ' ') ma_host[strlen(ma_host) - 1] = '\0';
    ma_port = (uint16_t)atoi(p_port.getValue());
    if (ma_port == 0) ma_port = MA_WS_PORT_DEFAULT;
    strlcpy(ma_token, p_token.getValue(), sizeof(ma_token));
    saveConfig();
    Serial.printf("[CFG] saved host='%s' port=%u token=%s len=%d\n",
                  ma_host, ma_port, strlen(ma_token) > 0 ? "<set>" : "<empty>",
                  (int)strlen(ma_token));
  }

  if (!connected) {
    // WiFi failed, but config is saved now -> retry booting quietly
    Serial.println("[CFG] WiFi connect failed, saved config, rebooting");
    drawMessage("WiFi retry...");
    delay(2000);
    ESP.restart();
  }

  if (strlen(ma_host) == 0) {
    Serial.println("[CFG] ERROR: host still empty after portal! restarting portal");
    drawMessage("No host! retry");
    delay(2000);
    ESP.restart();
  }

  drawMessage("Connecting MA...");

  ma.begin(ma_host, ma_port, ma_token, []() { markDirty(); });
  ma.setPlayersLoadedCallback([]() { restoreSelectedPlayer(); });

}

// ------------------------------------------------------------------
// Loop
// ------------------------------------------------------------------
void loop() {
  ma.loop();
  handleInput();

  static uint32_t lastPoll = 0;
  if (ma.isConnected() && millis() - lastPoll > 60000) {
    lastPoll = millis();
    ma.fetchPlayers();
  }

  // auto-hide the volume overlay after inactivity: full player redraw
  if (screen == SCR_VOLUME && volumeShownAt != 0 && millis() - volumeShownAt > 2500) {
    volumeShownAt = 0;
    screen = SCR_PLAYER;
    markDirty();  // triggers a FULL player redraw -> overlay is gone completely
  }

  // fetch artwork for the active player when needed (every 10s max)
  static uint32_t lastArtTry = 0;
  if (millis() - lastArtTry > 10000) {
    lastArtTry = millis();
    fetchArtwork();
  }

  static uint32_t lastDraw = 0;
  bool dirty = takeDirty();
  if (screen == SCR_VOLUME && dirty) {
    if (millis() - lastDraw > 80) {  // max ~12 fps for volume updates
      lastDraw = millis();
      updateVolumeScreen(false);
    }
  } else if (dirty) {
    drawScreen();
  }
}

// ------------------------------------------------------------------
// Input: encoder rotation + push, touch
// ------------------------------------------------------------------
void applyVolume() {
  static uint32_t lastSent = 0;
  static int lastSentVal = -1;
  if (ma.players.empty()) return;
  // send at most every 150ms while turning; final value is sent
  // when the value settles (last value always gets through)
  if (volumeTarget != lastSentVal && millis() - lastSent > 150) {
    lastSent = millis();
    lastSentVal = volumeTarget;
    ma.cmdVolumeSet(ma.players[ma.activePlayerIndex].player_id, volumeTarget);
  }
}

void handleInput() {
  // ---- encoder rotation ----
  static int64_t lastCount = 0;
  int64_t now = encoder.getCount();
  int delta = (int)(now - lastCount);
  // clamp spikes from contact noise: ignore impossible jumps
  if (delta > 4 || delta < -4) {
    delta = (delta > 0) ? 2 : -2;  // treat as a single detent step
  }
  if (delta != 0) {
    lastCount = now;
    switch (screen) {
      case SCR_PLAYER:
      case SCR_VOLUME:
        volumeTarget = constrain(volumeTarget + delta * 2, 0, 100);
        // apply immediately - no push needed anymore
        applyVolume();
        screen = SCR_VOLUME;
        volumeShownAt = millis();
        markDirty();
        break;
      case SCR_PLAYERLIST: {
        int n = (int)ma.players.size();
        if (n > 0) {
          ma.activePlayerIndex = ((ma.activePlayerIndex + delta) % n + n) % n;
          saveSelectedPlayer();
          markDirty();
        }
        break;
      }
      case SCR_MENU: {
        int n = 5;
        menuIndex = ((menuIndex + delta) % n + n) % n;
        markDirty();
        break;
      }
    }
  }

  // ---- encoder push (short + long) ----
  static uint32_t pressStart = 0;
  static bool longPressFired = false;
  bool sw = digitalRead(ENC_SW) == LOW;

  if (sw && pressStart == 0) {
    pressStart = millis();
    longPressFired = false;
  } else if (sw && pressStart != 0 && !longPressFired &&
             millis() - pressStart > 700) {
    // LONG PRESS -> open menu (touch replacement)
    longPressFired = true;
    if (screen != SCR_MENU && screen != SCR_PLAYERLIST) {
      screen = SCR_MENU;
      menuIndex = 0;
      markDirty();
    }
  } else if (!sw && pressStart != 0) {
    uint32_t dur = millis() - pressStart;
    pressStart = 0;
    if (!longPressFired && dur > 30 && dur < 400) {
      // SHORT PRESS
      uint32_t lastPush = 0;  // reuse debounce var semantics
    switch (screen) {
      case SCR_VOLUME:
        // volume is applied live; short press just returns to the player
        volumeShownAt = 0;
        screen = SCR_PLAYER;
        markDirty();
        break;
      case SCR_PLAYER:
        if (!ma.players.empty())
          ma.cmdPlayPause(ma.players[ma.activePlayerIndex].player_id);
        break;
      case SCR_PLAYERLIST:
        screen = SCR_PLAYER;
        markDirty();
        break;
      case SCR_MENU: {
        if (!ma.players.empty()) {
          const String &pid = ma.players[ma.activePlayerIndex].player_id;
          switch (menuIndex) {
            case 0: screen = SCR_PLAYER; break;
            case 1: screen = SCR_VOLUME; volumeShownAt = millis(); break;
            case 2: screen = SCR_PLAYERLIST; break;
            case 3:
              ma.cmdPower(pid, !ma.players[ma.activePlayerIndex].powered);
              screen = SCR_PLAYER;
              break;
            case 4: {
              WiFiManager wm;
              drawMessage("WiFi portal 180s");
              wm.startConfigPortal("MA-Controller");
              ESP.restart();
              break;
            }
          }
        } else if (menuIndex == 4) {
          WiFiManager wm;
          wm.startConfigPortal("MA-Controller");
          ESP.restart();
        }
        markDirty();
        break;
      }
    }
    }
  }

  // ---- touch ----
  uint8_t tx, ty;
  if (!touch.readTouch(tx, ty)) return;

  int dx = tx - SCREEN_W / 2;
  int dy = ty - SCREEN_H / 2;
  int r = (int)sqrtf((float)(dx * dx + dy * dy));

  switch (screen) {
    case SCR_PLAYER:
    case SCR_VOLUME: {
      if (r < 45) {
        if (!ma.players.empty())
          ma.cmdPlayPause(ma.players[ma.activePlayerIndex].player_id);
      } else if (dx > 40) {
        if (!ma.players.empty()) ma.cmdNext(ma.players[ma.activePlayerIndex].player_id);
      } else if (dx < -40) {
        if (!ma.players.empty()) ma.cmdPrevious(ma.players[ma.activePlayerIndex].player_id);
      } else if (dy < -30) {
        screen = SCR_PLAYERLIST;
        markDirty();
      } else if (dy > 30) {
        screen = SCR_MENU;
        markDirty();
      }
      break;
    }
    case SCR_PLAYERLIST: {
      int n = (int)ma.players.size();
      if (n > 0) {
        if (dy < -40) ma.activePlayerIndex = (ma.activePlayerIndex + 1) % n;
        else if (dy > 40) ma.activePlayerIndex = ((ma.activePlayerIndex - 1) % n + n) % n;
        else if (r < 60) { screen = SCR_PLAYER; }
        markDirty();
      }
      break;
    }
    case SCR_MENU: {
      int n = 5;
      if (dy < -40) menuIndex = (menuIndex + 1) % n;
      else if (dy > 40) menuIndex = ((menuIndex - 1) % n + n) % n;
      else if (r < 60) {
        // center touch = activate
      }
      markDirty();
      break;
    }
  }
  delay(60);  // touch debounce
}

// ------------------------------------------------------------------
// Album artwork (JPEG via HTTP, decoded with JPEGDEC into the center)
// ------------------------------------------------------------------
// decode into an offscreen buffer (128x128), then apply a circular clip
#define ART_SIZE 160  // circular artwork diameter: fills the inner area
                       // up to the volume ring (ring inner edge ~r108)
static uint16_t artDecodeBuf[ART_SIZE * ART_SIZE];

static int jpegDrawCallback(JPEGDRAW *pDraw) {
  // pDraw->x/y are buffer coordinates (JPEGDEC adds the decode origin).
  // MCUs that start outside the buffer partially overlap: pDraw->x can be
  // negative (left/top crop) - handle per-pixel clipping correctly.
  for (int y = 0; y < pDraw->iHeight; y++) {
    int dy = pDraw->y + y;
    if (dy < 0 || dy >= ART_SIZE) continue;
    for (int x = 0; x < pDraw->iWidth; x++) {
      int dx = pDraw->x + x;
      if (dx < 0 || dx >= ART_SIZE) continue;
      artDecodeBuf[dy * ART_SIZE + dx] = pDraw->pPixels[y * pDraw->iWidth + x];
    }
  }
  return 1;
}

void fetchArtwork() {
  if (ma.players.empty()) return;
  MaPlayer &p = ma.players[ma.activePlayerIndex];
  if (p.artwork_url.length() == 0) {
    Serial.printf("[ART] no artwork_url for %s (title: %s)\n",
                  p.name.c_str(), p.current_title.c_str());
    return;
  }
  // already loaded for this player+url?
  String key = p.player_id + "|" + p.artwork_url;
  if (artworkLoadedFor == key) return;

  WiFiClient client;
  HTTPClient http;
  String url = p.artwork_url;
  if (!url.startsWith("http")) url = String("http://") + ma_host + ":" + String(ma_port) + url;
  http.begin(client, url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    int len = http.getSize();
    if (len > 0 && len < 200000) {
      if (artworkJpeg) { free(artworkJpeg); artworkJpeg = nullptr; }
      artworkJpeg = (uint8_t *)malloc(len);
      if (artworkJpeg) {
        WiFiClient *stream = http.getStreamPtr();
        size_t read = 0;
        while (read < (size_t)len) {
          size_t n = stream->readBytes(artworkJpeg + read, len - read);
          if (n == 0) break;
          read += n;
        }
        artworkJpegLen = read;
        artworkLoadedFor = key;
        Serial.printf("[ART] loaded %d bytes for %s\n", (int)read, p.name.c_str());
        decodeArtworkToBuffer();  // decode ONCE here, not on every redraw
        markDirty();  // redraw the player screen with the new artwork
      }
    }
  } else {
    Serial.printf("[ART] HTTP %d\n", code);
  }
  http.end();
}

bool drawArtwork() {
  if (!artworkBufValid) return false;
  int x0 = (SCREEN_W - ART_SIZE) / 2;
  int y0 = (SCREEN_H - ART_SIZE) / 2;
  int cx = ART_SIZE / 2, cy = ART_SIZE / 2;
  int r2 = cx * cx;  // circle radius = half size
  for (int y = 0; y < ART_SIZE; y++) {
    for (int x = 0; x < ART_SIZE; x++) {
      int dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= r2) {
        gfx->drawPixel(x0 + x, y0 + y, artworkBuf[y * ART_SIZE + x]);
      }
    }
  }
  return true;
}

bool decodeArtworkToBuffer() {
  if (!artworkJpeg || artworkJpegLen == 0) return false;
  if (jpeg.openRAM(artworkJpeg, artworkJpegLen, jpegDrawCallback) != 1) {
    Serial.println("[ART] openRAM failed");
    return false;
  }
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  // scale: power of two. Pick the LARGEST scale that still yields at least
  // ART_SIZE in both dimensions -> the crop path fills the whole circle.
  // (e.g. 200x194 -> scale 0 = 200x194 -> center-crop 160x160)
  // Only if the source is smaller than the buffer does the centered path run.
  int scale = 0;
  while ((jpeg.getWidth() >> (scale + 1)) >= ART_SIZE &&
         (jpeg.getHeight() >> (scale + 1)) >= ART_SIZE) scale++;
  int w = jpeg.getWidth() >> scale;
  int h = jpeg.getHeight() >> scale;
  memset(artDecodeBuf, 0, sizeof(artDecodeBuf));
  if (w > ART_SIZE || h > ART_SIZE) {
    // scaled image larger than the buffer: crop the CENTER region.
    // JPEGDEC's setCropArea works along MCU boundaries and passes the
    // cropped pixels to the callback - position (0,0) in the callback
    // maps to the crop area's top-left corner.
    int cx = (w - ART_SIZE) / 2;
    int cy = (h - ART_SIZE) / 2;
    Serial.printf("[ART] src %dx%d crop (%d,%d) %dx%d scale 1/%d\n",
                  jpeg.getWidth(), jpeg.getHeight(), cx, cy, ART_SIZE, ART_SIZE,
                  1 << scale);
    jpeg.setCropArea(cx, cy, ART_SIZE, ART_SIZE);
    if (jpeg.decode(0, 0, scale) != 1) {
      Serial.println("[ART] decode failed");
      return false;
    }
  } else {
    // image fits: center it in the buffer
    int x0 = (ART_SIZE - w) / 2;
    int y0 = (ART_SIZE - h) / 2;
    Serial.printf("[ART] src %dx%d centered at (%d,%d) scale 1/%d\n",
                  jpeg.getWidth(), jpeg.getHeight(), x0, y0, 1 << scale);
    if (jpeg.decode(x0, y0, scale) != 1) {
      Serial.println("[ART] decode failed");
      return false;
    }
  }
  // (re)allocate the persistent buffer in PSRAM
  if (!artworkBuf) {
    artworkBuf = (uint16_t *)ps_malloc(sizeof(artDecodeBuf));
    if (!artworkBuf) {
      Serial.println("[ART] PSRAM alloc failed");
      return false;
    }
  }
  memcpy(artworkBuf, artDecodeBuf, sizeof(artDecodeBuf));
  artworkBufValid = true;
  return true;
}

// ------------------------------------------------------------------
// Screens
// ------------------------------------------------------------------
void drawScreen() {
  switch (screen) {
    case SCR_PLAYER:     drawPlayerScreen(); break;
    case SCR_VOLUME:     drawVolumeScreen(); break;
    case SCR_PLAYERLIST: drawPlayerList(); break;
    case SCR_MENU:       drawMenu(); break;
  }
}

void drawMessage(const char *msg) {
  gfx->fillScreen(BLACK);
  gfx->setTextColor(COL_ACCENT);
  gfx->setTextSize(2);
  gfx->setCursor(60, 110);
  gfx->println(msg);
}

void drawPlayerScreen() {
  gfx->fillScreen(BLACK);
  if (ma.players.empty()) {
    // show WHY there are no players (connection state)
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(30, 45);
    gfx->println("MA Server:");
    gfx->setCursor(30, 65);
    switch (ma.getState()) {
      case MaConnState::Disconnected:
        gfx->setTextColor(COL_STOP);
        gfx->println("disconnected");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(30, 85);
        gfx->println("check host/port");
        break;
      case MaConnState::Connecting:
        gfx->setTextColor(COL_PAUSE);
        gfx->println("connecting...");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(30, 85);
        if (ma.getProbeResult() == -1)
          gfx->println("host unreachable!");
        else if (ma.getProbeResult() == 1) {
          gfx->println("host reachable,");
          gfx->setCursor(30, 100);
          gfx->println("WS handshake stuck");
        }
        break;
      case MaConnState::Connected:
        gfx->setTextColor(COL_ACCENT);
        gfx->println("WS connected");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(30, 85);
        gfx->println("waiting for server");
        gfx->setCursor(30, 100);
        gfx->println("info message...");
        break;
      case MaConnState::Authenticated:
        gfx->setTextColor(COL_PLAY);
        gfx->println("connected");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(30, 85);
        gfx->println("no players found");
        break;
      case MaConnState::Error:
        gfx->setTextColor(COL_STOP);
        gfx->println("auth failed!");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(30, 85);
        gfx->println("check token");
        break;
    }
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(30, 130);
    gfx->println(String(ma_host) + ":" + String(ma_port));
    return;
  }
  MaPlayer &p = ma.players[ma.activePlayerIndex];

  uint16_t stateCol = COL_STOP;
  String st = p.state; st.toLowerCase();
  if (st == "playing") stateCol = COL_PLAY;
  else if (st == "paused" || st == "pausing") stateCol = COL_PAUSE;
  Serial.printf("[UI] player screen: state='%s' vol=%d ring=0x%04x\n",
                p.state.c_str(), p.volume_level, stateCol);

  // outer ring in STATE COLOR, filled proportionally to the volume -
  // shows both status (color) and volume (fill) permanently.
  // Players without volume support get a full ring instead.
  int ringVol = p.volume_support ? p.volume_level : 100;
  drawVolumeArc(120, 120, 117, ringVol, 6, stateCol, COL_RING_OFF);

  // album art in the center when available
  bool hasArt = drawArtwork();

  if (hasArt) {
    // text below the artwork
    gfx->setTextSize(1);
    gfx->setCursor(15, 178);
    gfx->println(p.current_title.substring(0, 30));
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(15, 192);
    gfx->println(p.current_artist.substring(0, 30));
  } else {
    gfx->setTextSize(2);
    gfx->setCursor(15, 88);
    gfx->println(p.current_title.substring(0, 16));
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(15, 112);
    gfx->println(p.current_artist.substring(0, 30));
  }

}

void drawVolumeScreen() {
  // draw the full player screen first, then the volume arc on top
  drawPlayerScreen();
  drawVolumeOverlay();
}

void drawVolumeOverlay() {
  // arc ring on the very outer edge, on top of the player view
  drawVolumeArc(120, 120, 117, volumeTarget, 8, COL_ACCENT, COL_RING_OFF);

  // semi-cover the center for the number
  gfx->fillCircle(120, 120, 55, BLACK);
  gfx->setTextColor(COL_FG);
  gfx->setTextSize(3);
  String v = String(volumeTarget) + "%";
  int16_t x = 120 - (int16_t)(v.length() * 9);
  gfx->setCursor(x, 108);
  gfx->println(v);
}

void updateVolumeScreen(bool force) {
  static int lastVol = -1;
  if (!force && lastVol == volumeTarget) return;
  lastVol = volumeTarget;
  drawVolumeOverlay();
}

void drawPlayerList() {
  gfx->fillScreen(BLACK);
  gfx->setTextColor(COL_FG);
  gfx->setTextSize(1);
  gfx->setCursor(30, 22);
  gfx->println("Players");

  int n = (int)ma.players.size();
  if (n == 0) {
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(70, 115);
    gfx->println("No players");
    return;
  }
  int start = max(0, ma.activePlayerIndex - 3);
  int end = min(n, start + 7);
  for (int i = start; i < end; i++) {
    int y = 45 + (i - start) * 24;
    if (i == ma.activePlayerIndex) {
      gfx->fillCircle(22, y + 7, 4, COL_ACCENT);
      gfx->setTextColor(COL_ACCENT);
    } else {
      gfx->setTextColor(COL_DIM);
    }
    gfx->setCursor(32, y);
    gfx->println(ma.players[i].name.substring(0, 18));
  }
}

void drawMenu() {
  gfx->fillScreen(BLACK);
  const char *items[5] = {"Back", "Volume", "Players", "Power", "WiFi setup"};
  gfx->setTextSize(1);
  for (int i = 0; i < 5; i++) {
    int y = 60 + i * 25;
    if (i == menuIndex) {
      gfx->fillCircle(40, y + 7, 4, COL_ACCENT);
      gfx->setTextColor(COL_ACCENT);
    } else {
      gfx->setTextColor(COL_DIM);
    }
    gfx->setCursor(50, y);
    gfx->println(items[i]);
  }
}

// ------------------------------------------------------------------
// Config persistence
// ------------------------------------------------------------------
Preferences prefs;

void saveSelectedPlayer() {
  if (ma.players.empty()) return;
  prefs.begin("mactl", false);
  prefs.putString("selplayer", ma.players[ma.activePlayerIndex].player_id);
  prefs.end();
}

void restoreSelectedPlayer() {
  prefs.begin("mactl", true);
  String saved = prefs.getString("selplayer", "");
  prefs.end();
  if (saved.length() == 0) return;
  for (size_t i = 0; i < ma.players.size(); i++) {
    if (ma.players[i].player_id == saved) {
      ma.activePlayerIndex = (int)i;
      Serial.printf("[CFG] restored player: %s\n", ma.players[i].name.c_str());
      return;
    }
  }
  Serial.println("[CFG] saved player not in list, using first");
}

void loadConfig() {
  prefs.begin("mactl", true);
  prefs.getString("host", ma_host, sizeof(ma_host));
  ma_port = prefs.getUShort("port", MA_WS_PORT_DEFAULT);
  prefs.getString("token", ma_token, sizeof(ma_token));
  prefs.end();
}

void saveConfig() {
  prefs.begin("mactl", false);
  prefs.putString("host", ma_host);
  prefs.putUShort("port", ma_port);
  prefs.putString("token", ma_token);
  prefs.end();
}
