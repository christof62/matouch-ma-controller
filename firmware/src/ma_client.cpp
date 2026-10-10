#include "ma_client.h"
#include "config.h"

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
        break;
      case WStype_DISCONNECTED:
        Serial.println("[MA] WS disconnected");
        _state = MaConnState::Disconnected;
        _authed = false;
        _authMessageId = "";
        break;
      case WStype_TEXT:
        handleIncoming(payload, length);
        break;
      default:
        break;
    }
  });
  _ws.setReconnectInterval(RECONNECT_INTERVAL_MS);
  connect();
}

void MusicAssistantClient::connect() {
  if (millis() - _lastConnectAttempt < RECONNECT_INTERVAL_MS) return;
  _lastConnectAttempt = millis();
  _state = MaConnState::Connecting;
  // WebSocketsClient handles reconnect via setReconnectInterval
}

void MusicAssistantClient::loop() {
  _ws.loop();
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
  if (_state == MaConnState::Disconnected && strcmp(command, "auth") != 0) return;
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
  JsonDocument doc;
  JsonObject args = doc.to<JsonObject>();
  sendCommand("players/all", args);
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
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[MA] JSON parse error: %s\n", err.c_str());
    return;
  }

  JsonObjectConst obj = doc.as<JsonObjectConst>();

  // server info (first message after WS connect) -----------------
  if (obj["server_version"].is<const char *>()) {
    _schemaVersion = obj["schema_version"] | 0;
    Serial.printf("[MA] server info: version %s, schema %d\n",
                  obj["server_version"].as<const char *>(), _schemaVersion);
    if (_schemaVersion >= 28 && _token.length() == 0) {
      Serial.println("[MA] WARNING: server requires a token but none is configured!");
      _state = MaConnState::Error;
      return;
    }
    if (_schemaVersion < 28) {
      // old server: no auth needed -> go straight to fetching players
      _authed = true;
      _state = MaConnState::Authenticated;
      fetchPlayers();
    } else {
      authenticate();
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
      _state = MaConnState::Error;
    }
    return;
  }

  // command results ----------------------------------------------
  if (obj["message_id"].is<const char *>() && obj["result"].is<JsonVariantConst>()) {
    const char *mid = obj["message_id"];
    JsonVariantConst result = obj["result"];

    // auth result? ------------------------------------------------
    if (_authMessageId.length() && _authMessageId == mid) {
      if (result.isNull() || result.is<bool>() || result.is<const char *>()) {
        // auth returns true or a user object depending on schema
        if (result.is<bool>() && !result.as<bool>()) {
          Serial.println("[MA] AUTH FAILED - invalid token");
          _state = MaConnState::Error;
          return;
        }
        _authed = true;
        _state = MaConnState::Authenticated;
        Serial.println("[MA] authenticated OK");
        fetchPlayers();
        if (_onEvent) _onEvent();
        return;
      }
      // user object returned = success too
      if (result.is<JsonObjectConst>()) {
        _authed = true;
        _state = MaConnState::Authenticated;
        Serial.println("[MA] authenticated OK");
        fetchPlayers();
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
      const char *st = p["state"] | "";
      if (st[0]) mp.state = st;
      mp.volume_level = p["volume_level"] | mp.volume_level;
      mp.muted = p["volume_muted"] | mp.muted;
      JsonObjectConst cm = p["current_media"];
      if (!cm.isNull()) {
        mp.current_title = cm["title"] | "";
        mp.current_artist = cm["artist"] | "";
        mp.current_album = cm["album"] | "";
      }
      return;
    }
  }
  // not found: full refresh
  fetchPlayers();
}
