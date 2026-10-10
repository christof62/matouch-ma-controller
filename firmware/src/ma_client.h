#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <vector>

// Player state as shown on the display
struct MaPlayer {
  String player_id;
  String name;
  bool available = false;
  bool powered = false;
  String state;        // "playing" | "paused" | "idle" | "stopped"
  int volume_level = 0;
  bool muted = false;
  String current_title;
  String current_artist;
  String current_album;
  uint32_t elapsed_seconds = 0;
  uint32_t total_seconds = 0;
};

// One queue/library item for the browse lists
struct MaItem {
  String uri;
  String name;
};

enum class MaConnState { Disconnected, Connecting, Connected, Authenticated, Error };

// Event callback: something changed (player list, player state, queue)
typedef std::function<void()> MaEventCallback;
// Debug callback: first raw message from the server (for display diagnosis)
typedef std::function<void(const char *)> MaDebugCallback;

class MusicAssistantClient {
public:
  void begin(const char *host, uint16_t port, const char *token, MaEventCallback cb);
  void setFirstMessageCallback(MaDebugCallback cb) { _onFirstMessage = cb; }
  void setStatusCallback(MaDebugCallback cb) { _onStatus = cb; }
  void loop();
  bool isConnected() const { return _state == MaConnState::Authenticated; }
  MaConnState getState() const { return _state; }
  int getSchemaVersion() const { return _schemaVersion; }
  // 0 = not probed, 1 = reachable, -1 = unreachable
  int getProbeResult() const { return _lastProbeResult; }

  // Data
  std::vector<MaPlayer> players;
  int activePlayerIndex = 0;

  // Commands (fire and forget; state updates arrive as events)
  void cmdPlayPause(const String &playerId);
  void cmdNext(const String &playerId);
  void cmdPrevious(const String &playerId);
  void cmdVolumeSet(const String &playerId, int vol);
  void cmdVolumeUp(const String &playerId);
  void cmdVolumeDown(const String &playerId);
  void cmdMute(const String &playerId, bool muted);
  void cmdPower(const String &playerId, bool on);
  void playRadioItem(const String &playerId, const MaItem &item);
  void playFavoriteItem(const String &playerId, const MaItem &item);

  // Data fetches (async; result arrives via events/refresh)
  void fetchPlayers();
  void fetchFavorites();
  void fetchRadio();
  std::vector<MaItem> favorites;
  std::vector<MaItem> radioStations;

private:
  void sendCommand(const char *command, JsonObject args);
  void handleIncoming(uint8_t *payload, size_t length);
  void parsePlayer(JsonObjectConst p, bool replaceAll);
  void connect();
  void disconnect();
  void authenticate();

  WebSocketsClient _ws;
  String _host;
  uint16_t _port;
  String _token;
  MaEventCallback _onEvent = nullptr;
  MaDebugCallback _onFirstMessage = nullptr;
  MaDebugCallback _onStatus = nullptr;
  void status(const char *msg) { if (_onStatus) _onStatus(msg); }
  MaConnState _state = MaConnState::Disconnected;
  uint32_t _lastConnectAttempt = 0;
  uint32_t _messageId = 0;
  uint32_t _lastHeartbeat = 0;
  bool _authed = false;
  String _authMessageId = "";
  int _schemaVersion = 0;
  int _lastProbeResult = 0;
  uint32_t _connectingSince = 0;
};
