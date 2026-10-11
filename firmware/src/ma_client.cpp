#include "ma_client.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>

// Music Assistant WebSocket API (schema >= 28 requires auth token).
// Wire format (JSON, one object per text frame):
//   command:  {"message_id": "...", "command": "...", "args": {...}}
//   result:   {"message_id": "...", "result": ...}
//   event:    {"event": "...", "object_id": "...", "data": {...}}

static const char *stateNames[] = {"playing", "paused", "idle", "stopped"};

static void wsEvent(WStype_t type, uint8_t *payload, size_t length, void *arg) {
  MusicAssistantClient *self = (MusicAssistantClient *)arg;
  switch (type) {
    case WStype_CONNECTED:
      self->getState();  // no-op; state handled below via friend access not needed
      break;
    default:
      break;
  }
  (void)payload;
  (void)length;
}

void MusicAssistantClient::begin(const char *host, uint16_t port, const char *token, MaEventCallback cb) {
  _host = host;
  _port = port;
  _token = token;
  _onEvent = cb;

  _ws.begin(host, port, "/ws");
  _ws.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
      case WStype_CONNECTED:
        Serial.printf("[MA] WS connected to %s:%u\n", _host.c_str(), _port);
        _state = MaConnState::Connected;
        if (_onEvent) _onEvent();  // trigger UI redraw with new state
        break;
      case WStype_DISCONNECTED:
        Serial.println("[MA] WS disconnected (handshake failed or connection lost)");
 _state = MaConnState::Disconnected;
        _authed = false;
        _authMessageId = "";
        if (payload && length) {
          Serial.printf("[MA] WS close reason: %.*s\n", (int)min((size_t)120, length), (const char *)payload);
        }
        if (_onEvent) _onEvent();  // trigger UI redraw with new state
        break;
      case WStype_TEXT:
        handleIncoming(payload, length);
        break;
      case WStype_ERROR:
        Serial.println("[MA] WS ERROR event");
        break;
    }
  });
  _ws.setReconnectInterval(RECONNECT_INTERVAL_MS);
  // keep the connection alive: send WS pings every 20s so routers/NAT
  // and the server (aiohttp heartbeat) never see an idle connection.
  // Idle TCP connections are the #1 cause of recurring drops.
  _ws.enableHeartbeat(20000, 0, true);
  connect();
}

void MusicAssistantClient::connect() {
  _lastConnectAttempt = millis();
  _state = MaConnState::Connecting;
  _connectingSince = millis();
  // WebSocketsClient handles reconnect via setReconnectInterval
}

void MusicAssistantClient::loop() {
  _ws.loop();

  // deferred actions: never send WS commands from inside the WS event callback
  if (_pendingAuth && _state == MaConnState::Connected) {
    _pendingAuth = false;
    authenticate();
  }
  if (_pendingPlayersFetch && _authed) {
    _pendingPlayersFetch = false;
    fetchPlayers();
  }

  // periodic status heartbeat so the connection state is always visible
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    static const char *names[] = {"disconnected", "connecting", "connected", "authenticated", "error"};
    Serial.printf("[MA] heartbeat: %s, players=%d, wifi=%s\n",
                  names[(int)_state], (int)players.size(),
                  WiFi.isConnected() ? "ok" : "DOWN");
  }

  // TCP reachability test: if we are stuck in Connecting, try a raw TCP
  // connect to the server every 15s so the UI can show whether the host
  // is reachable from WiFi at all (catches AP isolation / wrong IP / firewall)
  if (_state == MaConnState::Connecting || _state == MaConnState::Disconnected) {
    static uint32_t lastProbe = 0;
    if (millis() - lastProbe > 30000) {
      lastProbe = millis();
      WiFiClient probe;
      probe.setTimeout(1000);
      bool ok = probe.connect(_host.c_str(), _port);
      Serial.printf("[MA] TCP probe %s:%u -> %s\n", _host.c_str(), _port, ok ? "OK" : "FAILED");
      _lastProbeResult = ok ? 1 : -1;
      probe.stop();
      if (_onEvent) _onEvent();
    }
  }

  // connect timeout: if no WS connected within 12s of Connecting, mark disconnected
  if (_state == MaConnState::Connected) {
    _connectingSince = 0;
  } else if (_state == MaConnState::Connecting) {
    if (_connectingSince == 0) _connectingSince = millis();
    else if (millis() - _connectingSince > 12000) {
      Serial.println("[MA] connect attempt timed out");
      _state = MaConnState::Disconnected;
      if (_onEvent) _onEvent();
    }
  }
}

void MusicAssistantClient::disconnect() {
  _ws.disconnect();
  _state = MaConnState::Disconnected;
  _authed = false;
}

uint32_t nextMessageId() {
  static uint32_t id = 0;
  return ++id;
}

void MusicAssistantClient::sendCommand(const char *command, JsonObject args) {
  if (_state == MaConnState::Disconnected) return;
  String id = String(nextMessageId());
  if (strcmp(command, "auth") == 0) _authMessageId = id;
  JsonDocument doc;
  doc["message_id"] = id;
  doc["command"] = command;
  doc["args"] = args;
  String out;
  serializeJson(doc, out);
  Serial.printf("[MA] >> %s\n", out.c_str());
  _ws.sendTXT(out);
}

// ------------------------- commands -------------------------

static void argPlayerId(JsonObject &args, const String &playerId) {
  args["player_id"] = playerId;
}

void MusicAssistantClient::cmdPlayPause(const String &playerId) {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  argPlayerId(args, playerId);
  sendCommand("players/cmd/play_pause", args);
}

void MusicAssistantClient::cmdNext(const String &playerId) {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  argPlayerId(args, playerId);
  sendCommand("players/cmd/next", args);
}

void MusicAssistantClient::cmdPrevious(const String &playerId) {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  argPlayerId(args, playerId);
  sendCommand("players/cmd/previous", args);
}

void MusicAssistantClient::cmdVolumeSet(const String &playerId, int vol) {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  argPlayerId(args, playerId);
  args["volume_level"] = vol;
  sendCommand("players/cmd/volume_set", args);
}

void MusicAssistantClient::cmdVolumeUp(const String &playerId) {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  argPlayerId(args, playerId);
  sendCommand("players/cmd/volume_up", args);
}

void MusicAssistantClient::cmdVolumeDown(const String &playerId) {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  argPlayerId(args, playerId);
  sendCommand("players/cmd/volume_down", args);
}

void MusicAssistantClient::cmdMute(const String &playerId, bool muted) {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  argPlayerId(args, playerId);
  args["muted"] = muted;
  sendCommand("players/cmd/volume_mute", args);
}

void MusicAssistantClient::cmdPower(const String &playerId, bool on) {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  argPlayerId(args, playerId);
  args["powered"] = on;
  sendCommand("players/cmd/power", args);
}

void MusicAssistantClient::playRadioItem(const String &playerId, const MaItem &item) {
  // play media uri on the player's queue
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  args["player_id"] = playerId;
  args["uris"] = JsonArray().add(item.uri);
  sendCommand("player_queues/play_media", args);
}

void MusicAssistantClient::playFavoriteItem(const String &playerId, const MaItem &item) {
  playRadioItem(playerId, item);  // same play_media path with uri
}

// ------------------------- fetches -------------------------

void MusicAssistantClient::fetchPlayers() {
  // players/all responses are HUGE (10-50KB) and can kill the
  // WebSocket receive buffer on ESP32. Use the HTTP JSON-RPC endpoint
  // instead (POST /api with Bearer token), which we can parse streaming.
  if (_httpFetching) return;
  _httpFetching = true;

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + _host + ":" + String(_port) + "/api";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + _token);
  http.setTimeout(10000);

  String body = "{\"command\":\"players/all\",\"args\":{}}";
  int code = http.POST(body);

  Serial.printf("[MA] HTTP players/all -> %d\n", code);
  if (code == 200) {
    String response = http.getString();
    Serial.printf("[MA] HTTP response (%d bytes)\n", (int)response.length());
    parsePlayersJson(response.c_str(), response.length());
  } else {
    Serial.printf("[MA] HTTP error: %s\n", http.getString().c_str());
  }
  http.end();
  _httpFetching = false;
}

void MusicAssistantClient::parsePlayersJson(const char *json, size_t len) {
  // parse with filter to keep memory low (ArduinoJson7 filter syntax)
  JsonDocument filter;
  JsonArray farr = filter.to<JsonArray>();
  JsonObject f0 = farr.add<JsonObject>();
  f0["player_id"] = true;
  f0["name"] = true;
  f0["available"] = true;
  f0["powered"] = true;
  f0["state"] = true;
  f0["playback_state"] = true;
  f0["volume_level"] = true;
  f0["volume_muted"] = true;
  JsonObject fcm = f0["current_media"].to<JsonObject>();
  fcm["title"] = true;
  fcm["artist"] = true;
  fcm["album"] = true;
  fcm["image_url"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[MA] players JSON error: %s\n", err.c_str());
    return;
  }

  // HTTP /api returns the result directly as an array;
  // the WebSocket variant wraps it in {"result": [...]}
  JsonArrayConst arr = doc.as<JsonArrayConst>();
  if (arr.isNull()) {
    arr = doc["result"].as<JsonArrayConst>();
  }
  if (arr.isNull()) {
    Serial.println("[MA] no players array in response");
    // dump the first bytes to see what we actually got
    Serial.printf("[MA] response head: %.200s\n", json);
    return;
  }

  players.clear();
  static bool rawDumped = false;
  if (!rawDumped && arr.size() > 0) {
    rawDumped = true;
    // dump the raw json around "image_url" for the first player having it
    const char *pos = strstr(json, "image_url");
    if (pos) {
      Serial.printf("[DBG] raw image_url ctx: %.160s\n", pos);
    } else {
      Serial.println("[DBG] raw json contains NO image_url at all");
    }
  }
  for (JsonObjectConst p : arr) {
    MaPlayer mp;
    mp.player_id = p["player_id"].as<String>();
    if (mp.player_id.length() == 0) continue;
    mp.name = p["name"] | mp.player_id;
    mp.available = p["available"] | false;
    mp.powered = p["powered"] | false;
    // MA 2.11 serializes the state as "playback_state"
    const char *st = p["playback_state"] | (const char *)nullptr;
    if (!st) st = p["state"] | "idle";
    mp.state = String(st);
    if (p["volume_level"].is<int>())
      mp.volume_level = p["volume_level"].as<int>();
    else if (p["volume_level"].is<float>())
      mp.volume_level = (int)(p["volume_level"].as<float>() * 100);  // 0..1 float
    mp.volume_support = !p["volume_level"].isNull();
    mp.muted = p["volume_muted"] | false;
    JsonObjectConst cm = p["current_media"];
    if (!cm.isNull()) {
      mp.current_title = cm["title"] | "";
      mp.current_artist = cm["artist"] | "";
      mp.current_album = cm["album"] | "";
      // DEBUG: dump the raw current_media of the first player that has one
      mp.current_uri = cm["uri"] | "";
      static bool cmDumped = false;
      if (!cmDumped) {
        cmDumped = true;
        String out;
        serializeJson(cm, out);
        Serial.printf("[DBG] current_media: %s\n", out.c_str());
      }
      // image can be an object or array of objects with a "url"/"path"
      JsonVariantConst img = cm["image"];
      if (img.is<JsonArrayConst>()) {
        JsonArrayConst ia = img.as<JsonArrayConst>();
        if (ia.size() > 0 && ia[0].is<JsonObjectConst>()) {
          const char *u = ia[0]["url"] | (const char *)nullptr;
          if (!u) u = ia[0]["path"];
          if (u) mp.artwork_url = String(u);
        }
      } else if (img.is<JsonObjectConst>()) {
        const char *u = img["url"] | (const char *)nullptr;
        if (!u) u = img["path"];
        if (u) mp.artwork_url = String(u);
      }
    }
    Serial.printf("[MA] player: %s (%s)\n", mp.name.c_str(), mp.state.c_str());
    players.push_back(mp);
  }
  Serial.printf("[MA] parsed %d players\n", (int)players.size());
  if (activePlayerIndex >= (int)players.size()) activePlayerIndex = 0;
  if (_onPlayersLoaded) _onPlayersLoaded();  // UI restores saved selection
  if (_onEvent) _onEvent();
}

void MusicAssistantClient::fetchFavorites() {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  // favorites/library/list (schema: library command)
  sendCommand("library/favorites", args);
}

void MusicAssistantClient::fetchRadio() {
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  args["media_type"] = "radio";
  sendCommand("library/browse", args);
}

// ------------------------- incoming -------------------------

void MusicAssistantClient::authenticate() {
  Serial.printf("[MA] authenticating...\n");
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  args["token"] = _token;
  sendCommand("auth", args);
}

void MusicAssistantClient::handleIncoming(uint8_t *payload, size_t length) {
  // DEBUG: log the first incoming messages to the on-display status log
  // so we can follow the protocol even without a serial console
  static int msgCount = 0;
  msgCount++;

  static bool firstMsgShown = false;
  if (!firstMsgShown) {
    firstMsgShown = true;
    Serial.printf("[MA] first message (%d bytes): %.*s\n", (int)length,
                  (int)min((size_t)300, length), (const char *)payload);
    // show the first bytes of the first message on the display for diagnosis
    if (_onFirstMessage) {
      char buf[241];
      int n = (int)min((size_t)240, length);
      memcpy(buf, payload, n);
      buf[n] = '\0';
      // try to find "schema_version" and rotate so it is visible on the display
      char *sv = strstr(buf, "schema_version");
      if (sv && sv > buf) {
        char out[241];
        int head = min(60, (int)(sv - buf));
        snprintf(out, sizeof(out), "%.*s...%s", head, buf, sv);
        _onFirstMessage(out);
      } else {
        _onFirstMessage(buf);
      }
    }
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[MA] JSON parse error: %s\n", err.c_str());
    return;
  }

  JsonObjectConst obj = doc.as<JsonObjectConst>();

  // server info (first message after WS connect) -----------------
  // detect by any typical ServerInfoMessage field (be robust against naming)
  if (obj["schema_version"].is<int>() || obj["min_supported_schema_version"].is<int>() ||
      obj["server_version"].is<const char *>() || obj["server_id"].is<const char *>()) {
    // schema_version may be int or string; min_supported_schema_version is authoritative
    // for the auth decision when schema_version is missing
    if (obj["schema_version"].is<int>())
      _schemaVersion = obj["schema_version"].as<int>();
    else if (obj["schema_version"].is<const char *>())
      _schemaVersion = atoi(obj["schema_version"].as<const char *>());
    else if (obj["min_supported_schema_version"].is<int>())
      _schemaVersion = obj["min_supported_schema_version"].as<int>();
    Serial.printf("[MA] server info: schema %d\n", _schemaVersion);
    char m[40];
    snprintf(m, sizeof(m), "schema %d -> auth needed", _schemaVersion);
    status(m);
    if (obj["server_version"].is<const char *>())
      Serial.printf("[MA] server version: %s\n", obj["server_version"].as<const char *>());
    if (_schemaVersion >= 28 && _token.length() == 0) {
      Serial.println("[MA] WARNING: server requires a token but none is configured!");
      _state = MaConnState::Error;
      return;
    }
    if (_schemaVersion < 28) {
      // old server: no auth needed -> go straight to fetching players
      _authed = true;
      _state = MaConnState::Authenticated;
      _pendingPlayersFetch = true;
    } else {
      _pendingAuth = true;
    }
    return;
  }

  // error results -------------------------------------------------
  if (obj["message_id"].is<const char *>() && obj["error"].is<JsonVariantConst>()) {
    const char *mid = obj["message_id"];
    Serial.printf("[MA] command error for msg %s: %s\n", mid,
                  obj["error"]["message"] | "unknown");
    if (_authMessageId.length() && _authMessageId == mid) {
      Serial.println("[MA] AUTH FAILED - check token!");
      status("AUTH FAILED - check token");
      _state = MaConnState::Error;
      if (_onEvent) _onEvent();
    } else {
      char m[80];
      snprintf(m, sizeof(m), "cmd error: %s",
               obj["error"]["error_code"].is<const char *>()
                   ? obj["error"]["error_code"].as<const char *>() : "unknown");
      status(m);
    }
    return;
  }

  // command results ----------------------------------------------
  if (obj["message_id"].is<const char *>() && obj["result"].is<JsonVariantConst>()) {
    const char *mid = obj["message_id"];
    JsonVariantConst result = obj["result"];

    // auth result? ------------------------------------------------
    if (_authMessageId.length() && _authMessageId == mid) {
      if (result.is<bool>() && !result.as<bool>()) {
        Serial.println("[MA] AUTH FAILED - invalid token");
        status("AUTH FAILED - token");
        _state = MaConnState::Error;
        if (_onEvent) _onEvent();
        return;
      }
      // true / user object / anything else non-null = success
      if (!result.isNull()) {
        _authed = true;
        _state = MaConnState::Authenticated;
        Serial.println("[MA] authenticated OK");
        if (players.empty()) {
          // initial connect: we need the player list (blocking download)
          _pendingPlayersFetch = true;
        } else {
          // reconnect: keep the existing list; live events refresh it and
          // the periodic poll fills gaps - avoids multi-second blocking
          Serial.println("[MA] reconnect: keeping cached players");
        }
        if (_onEvent) _onEvent();
        return;
      }
    }

    // players/all -> array of players ---------------------------
    if (result.is<JsonArrayConst>()) {
      JsonArrayConst arr = result.as<JsonArrayConst>();
      if (arr.size() > 0 && arr[0].is<JsonObjectConst>() && arr[0]["player_id"].is<const char *>()) {
        Serial.printf("[MA] players/all: %d players\n", (int)arr.size());
        players.clear();
        for (JsonObjectConst p : arr) {
          MaPlayer mp;
          mp.player_id = p["player_id"].as<String>();
          mp.name = p["name"] | mp.player_id;
          mp.available = p["available"] | false;
          mp.powered = p["powered"] | false;
          mp.state = p["state"] | "idle";
          mp.volume_level = p["volume_level"] | 0;
          mp.muted = p["volume_muted"] | false;
          JsonObjectConst cm = p["current_media"];
          if (!cm.isNull()) {
            mp.current_title = cm["title"] | "";
            mp.current_artist = cm["artist"] | "";
            mp.current_album = cm["album"] | "";
          }
          Serial.printf("[MA] player: %s (%s)\n", mp.name.c_str(), mp.player_id.c_str());
          players.push_back(mp);
        }
        if (activePlayerIndex >= (int)players.size()) activePlayerIndex = 0;
        if (_onEvent) _onEvent();
        return;
      }
      // generic item lists (favorites/radio browse results)
      if (arr.size() > 0 && arr[0].is<JsonObjectConst>() && arr[0]["uri"].is<const char *>()) {
        std::vector<MaItem> *target = &favorites;
        for (JsonObjectConst it : arr) {
          MaItem item;
          item.uri = it["uri"].as<String>();
          item.name = it["name"] | item.uri;
          target->push_back(item);
        }
        if (_onEvent) _onEvent();
      }
    }
    return;
  }

  // events --------------------------------------------------------
  const char *event = obj["event"] | "";
  if (event[0] == '\0') return;

  if (strcmp(event, "player_updated") == 0) {
    JsonObjectConst data = obj["data"];
    // DEBUG: dump current_media of the first few player_updated events
    static int cmDump = 0;
    if (cmDump < 3 && !data.isNull() && !data["current_media"].isNull()) {
      cmDump++;
      String out;
      serializeJson(data["current_media"], out);
      Serial.printf("[DBG] current_media: %s\n", out.c_str());
    }
    if (!data.isNull()) parsePlayer(data, false);
    if (_onEvent) _onEvent();
  } else if (strcmp(event, "player_added") == 0 || strcmp(event, "player_removed") == 0) {
    fetchPlayers();
  } else if (strcmp(event, "queue_time_updated") == 0) {
    // progress update
    JsonObjectConst data = obj["data"];
    if (!data.isNull() && activePlayerIndex < (int)players.size()) {
      players[activePlayerIndex].elapsed_seconds = data["elapsed_time"] | 0;
      if (_onEvent) _onEvent();
    }
  }
}

void MusicAssistantClient::parsePlayer(JsonObjectConst p, bool replaceAll) {
  (void)replaceAll;
  const char *pid = p["player_id"];
  if (!pid) return;
  for (auto &mp : players) {
    if (mp.player_id == pid) {
      mp.name = p["name"] | mp.name;
      mp.available = p["available"] | mp.available;
      mp.powered = p["powered"] | mp.powered;
      const char *st = p["playback_state"] | (const char *)nullptr;
      if (!st) st = p["state"] | "";
      if (st[0]) mp.state = String(st);
      if (!p["volume_level"].isNull()) {
        if (p["volume_level"].is<int>())
          mp.volume_level = p["volume_level"].as<int>();
        else if (p["volume_level"].is<float>())
          mp.volume_level = (int)(p["volume_level"].as<float>() * 100);
        mp.volume_support = true;
      }
      mp.muted = p["volume_muted"] | mp.muted;
      JsonObjectConst cm = p["current_media"];
      if (!cm.isNull()) {
        mp.current_title = cm["title"] | "";
        mp.current_artist = cm["artist"] | "";
        mp.current_album = cm["album"] | "";
        mp.current_uri = cm["uri"] | mp.current_uri;
        // MA 2.11 sends the cover as simple "image_url" string
        const char *u = cm["image_url"] | (const char *)nullptr;
        if (u) mp.artwork_url = String(u);
      }
      return;
    }
  }
  // not found: full refresh
  fetchPlayers();
}
