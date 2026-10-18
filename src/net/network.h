#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <mutex>
#include <chrono>

namespace net {

constexpr uint32_t kProtocolMagic = 0x424C4143; // 'BLAC'
constexpr uint16_t kDefaultPort = 1508;
constexpr size_t kMaxPlayers = 8;
constexpr float kTimeoutSeconds = 5.0f;

enum class MsgType : uint8_t {
    Ping = 0,
    Pong = 1,
    JoinRequest = 2,
    JoinAck = 3,
    StateUpdate = 4,
    Shoot = 5,
    Hit = 6,
    Disconnect = 7,
    Chat = 8
};

#pragma pack(push, 1)

struct PacketHeader {
    uint32_t magic;
    uint8_t type;
    uint8_t player_id;
    uint16_t sequence;
    uint32_t timestamp_ms;
};

struct PlayerStateData {
    float pos[3];
    float forward[3];
    float yaw;
    float pitch;
    uint32_t weapon_id;
    uint32_t flags; // bit 0: firing, bit 1: aiming, bit 2: crouching, bit 3: reloading
    float health;
    uint16_t frags;
    uint16_t deaths;
};

struct PacketJoinRequest {
    PacketHeader header;
    char player_name[32];
};

struct PacketJoinAck {
    PacketHeader header;
    uint8_t assigned_id;
    uint8_t max_players;
    char server_name[32];
};

struct PacketState {
    PacketHeader header;
    PlayerStateData state;
};

struct PacketShoot {
    PacketHeader header;
    float origin[3];
    float dir[3];
    uint32_t weapon_id;
};

struct PacketHit {
    PacketHeader header;
    uint8_t victim_id;
    float damage;
    float hit_pos[3];
};

struct PacketChat {
    PacketHeader header;
    char message[64];
};

#pragma pack(pop)

struct RemotePlayer {
    bool active = false;
    uint8_t id = 0;
    std::string name;
    std::string endpoint_str;
    PlayerStateData state{};
    float ping_ms = 0.0f;
    std::chrono::steady_clock::time_point last_seen{};
    bool just_fired = false;

    // Entity Interpolation (cl_interp GoldSrc style)
    float prev_pos[3]{};
    float current_pos[3]{};
    float interp_alpha = 1.0f;
};

enum class NetMode {
    Offline,
    Host,
    Client
};

class NetworkManager {
public:
    static NetworkManager& Get();

    bool StartHost(uint16_t port = kDefaultPort);
    bool ConnectTo(const std::string& host_ip, uint16_t port = kDefaultPort);
    void Disconnect();

    void Update(float dt);

    void SendLocalState(const PlayerStateData& state);
    void BroadcastShoot(const float origin[3], const float dir[3], uint32_t weapon_id);
    void SendHit(uint8_t victim_id, float damage, const float hit_pos[3]);
    void SendChatMessage(const std::string& msg);

    NetMode GetMode() const { return mode_; }
    bool IsConnected() const;
    uint8_t GetLocalPlayerId() const { return local_player_id_; }
    float GetPing() const { return current_ping_ms_; }
    const std::string& GetServerAddress() const { return host_ip_; }
    uint16_t GetPort() const { return port_; }
    const std::string& GetLocalPlayerName() const { return local_player_name_; }
    void SetLocalPlayerName(const std::string& name) { local_player_name_ = name; }

    std::vector<RemotePlayer> GetPlayers();
    std::vector<std::string> GetEventLogs();
    void LogEvent(const std::string& msg);

private:
    NetworkManager();
    ~NetworkManager();

    void ProcessIncomingPackets();
    void HandlePacket(const uint8_t* data, size_t size, void* sender_addr, uint32_t addr_len);
    void SendPacket(const void* data, size_t size, void* dest_addr, uint32_t addr_len);
    void CheckTimeouts();

    NetMode mode_ = NetMode::Offline;
    int socket_fd_ = -1;
    uint16_t port_ = kDefaultPort;
    std::string host_ip_ = "127.0.0.1";
    std::string local_player_name_ = "Player";
    uint8_t local_player_id_ = 0;
    uint16_t sequence_counter_ = 0;
    float current_ping_ms_ = 0.0f;
    float time_since_last_ping_ = 0.0f;
    float time_since_last_state_broadcast_ = 0.0f;

    void* server_sockaddr_ = nullptr; // For client connecting to host
    uint32_t server_socklen_ = 0;

    mutable std::mutex mutex_;
    std::array<RemotePlayer, kMaxPlayers> players_{};
    std::vector<std::string> event_logs_;

    // Private helper for host client tracking
    struct HostClient {
        bool active = false;
        uint8_t id = 0;
        std::string name;
        uint8_t raw_addr[128]; // sockaddr_storage storage
        uint32_t addr_len = 0;
        std::chrono::steady_clock::time_point last_seen{};
    };
    std::array<HostClient, kMaxPlayers> host_clients_{};
};

} // namespace net
