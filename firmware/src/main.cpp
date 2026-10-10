#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Arduino_GFX_Library.h>
#include <ESP32Encoder.h>
#include <Preferences.h>

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
char ma_token[96] = "";
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
void drawPlayerList();
void drawMenu();
void drawMessage(const char *msg);
void handleInput();
void saveConfig();
void loadConfig();
void markDirty() { uiDirty = true; }
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
  encoder.attachHalfQuad(ENC_DT, ENC_CLK);
  encoder.setCount(0);

  loadConfig();
  strlcpy(ma_port_str, String(ma_port).c_str(), sizeof(ma_port_str));
  Serial.printf("[CFG] loaded host='%s' port=%u token=%s\n",
                ma_host, ma_port, strlen(ma_token) > 0 ? "<set>" : "<empty>");

  WiFiManager wm;
  WiFiManagerParameter p_host("ma_host", "MA Server (IP/Hostname)", ma_host, 64);
  WiFiManagerParameter p_port("ma_port", "MA Port (default 8095)", ma_port_str, 8);
  WiFiManagerParameter p_token("ma_token", "MA Token", ma_token, 96);
  wm.addParameter(&p_host);
  wm.addParameter(&p_port);
  wm.addParameter(&p_token);

  drawMessage("Connecting WiFi...");

  bool portalWasUsed = false;
  bool connected;
  if (strlen(ma_host) == 0) {
    // no MA server configured yet: force the config portal
    Serial.println("[CFG] host empty -> starting config portal");
    drawMessage("Setup portal...");
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
    Serial.printf("[CFG] saved host='%s' port=%u token=%s\n",
                  ma_host, ma_port, strlen(ma_token) > 0 ? "<set>" : "<empty>");
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

  // auto-return from volume screen after inactivity
  if (screen == SCR_VOLUME && volumeShownAt != 0 && millis() - volumeShownAt > 5000) {
    volumeShownAt = 0;
    screen = SCR_PLAYER;
    markDirty();
  }

  if (takeDirty()) drawScreen();
}

// ------------------------------------------------------------------
// Input: encoder rotation + push, touch
// ------------------------------------------------------------------
void applyVolume() {
  if (!ma.players.empty()) {
    ma.cmdVolumeSet(ma.players[ma.activePlayerIndex].player_id, volumeTarget);
  }
}

void handleInput() {
  // ---- encoder rotation ----
  static int64_t lastCount = 0;
  int64_t now = encoder.getCount();
  int delta = (int)(now - lastCount);
  if (delta != 0) {
    lastCount = now;
    switch (screen) {
      case SCR_PLAYER:
      case SCR_VOLUME:
        volumeTarget = constrain(volumeTarget + delta * 2, 0, 100);
        screen = SCR_VOLUME;
        volumeShownAt = millis();
        markDirty();
        break;
      case SCR_PLAYERLIST: {
        int n = (int)ma.players.size();
        if (n > 0) {
          ma.activePlayerIndex = ((ma.activePlayerIndex + delta) % n + n) % n;
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

  // ---- encoder push ----
  static uint32_t lastPush = 0;
  if (digitalRead(ENC_SW) == LOW && millis() - lastPush > 300) {
    lastPush = millis();
    switch (screen) {
      case SCR_VOLUME:
        applyVolume();
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
        // push on center = activate (same as encoder push)
        lastPush = 0;
      }
      markDirty();
      break;
    }
  }
  delay(60);  // touch debounce
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
  if (p.state == "playing") stateCol = COL_PLAY;
  else if (p.state == "paused") stateCol = COL_PAUSE;

  int vol = p.volume_level;
  drawVolumeArc(120, 120, 112, vol, 8, COL_ACCENT2, COL_RING_OFF);
  drawThickArc(120, 120, 100, 0, TWO_PI_F, 2, stateCol);

  gfx->setTextColor(COL_FG);
  gfx->setTextSize(1);
  gfx->setCursor(30, 40);
  gfx->println(p.name.substring(0, 24));

  gfx->setTextSize(2);
  gfx->setCursor(15, 88);
  gfx->println(p.current_title.substring(0, 16));
  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(15, 112);
  gfx->println(p.current_artist.substring(0, 30));

  gfx->setTextColor(stateCol);
  gfx->setCursor(95, 185);
  gfx->println(p.state);
  gfx->setTextColor(COL_ACCENT);
  gfx->setCursor(105, 205);
  gfx->print(vol);
  gfx->println("%");
}

void drawVolumeScreen() {
  gfx->fillScreen(BLACK);
  if (ma.players.empty()) { screen = SCR_PLAYER; return; }
  MaPlayer &p = ma.players[ma.activePlayerIndex];

  drawVolumeArc(120, 120, 106, volumeTarget, 12, COL_ACCENT, COL_RING_OFF);

  gfx->setTextColor(COL_FG);
  gfx->setTextSize(4);
  String v = String(volumeTarget) + "%";
  int16_t x = 120 - (int16_t)(v.length() * 12);
  gfx->setCursor(x, 105);
  gfx->println(v);

  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(52, 165);
  gfx->println(p.name.substring(0, 22));

  gfx->setCursor(52, 185);
  gfx->println("push = set");
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
