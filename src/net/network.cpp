#include "net/network.h"
#include "core/log.h"

#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSESOCKET(s) closesocket(s)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#define CLOSESOCKET(s) close(s)
#endif

namespace net {

namespace {

uint32_t GetTimeMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool SetNonBlocking(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string SockAddrToString(const sockaddr* sa) {
    char buf[128] = {};
    if (sa->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(sin->sin_port));
    }
    return "unknown";
}

bool SockAddrEqual(const sockaddr* a, const sockaddr* b) {
    if (a->sa_family != b->sa_family) return false;
    if (a->sa_family == AF_INET) {
        const auto* sa1 = reinterpret_cast<const sockaddr_in*>(a);
        const auto* sa2 = reinterpret_cast<const sockaddr_in*>(b);
        return sa1->sin_addr.s_addr == sa2->sin_addr.s_addr && sa1->sin_port == sa2->sin_port;
    }
    return false;
}

} // namespace

NetworkManager& NetworkManager::Get() {
    static NetworkManager instance;
    return instance;
}

NetworkManager::NetworkManager() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    server_sockaddr_ = new sockaddr_storage();
    server_socklen_ = sizeof(sockaddr_storage);
}

NetworkManager::~NetworkManager() {
    Disconnect();
    if (server_sockaddr_) {
        delete static_cast<sockaddr_storage*>(server_sockaddr_);
        server_sockaddr_ = nullptr;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

bool NetworkManager::IsConnected() const {
    if (mode_ == NetMode::Offline) return false;
    if (mode_ == NetMode::Host) return true;
    if (mode_ == NetMode::Client) return local_player_id_ != 0;
    return false;
}

bool NetworkManager::StartHost(uint16_t port) {
    Disconnect();
    std::lock_guard<std::mutex> lock(mutex_);

    port_ = port;
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd_ < 0) {
        LOGE("NetworkManager: Failed to create socket");
        return false;
    }

    int opt = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("NetworkManager: Failed to bind socket on port %u", port);
        CLOSESOCKET(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    SetNonBlocking(socket_fd_);

    mode_ = NetMode::Host;
    local_player_id_ = 0; // Host is always ID 0

    // Setup host player entry
    players_[0].active = true;
    players_[0].id = 0;
    players_[0].name = local_player_name_.empty() ? "Host" : local_player_name_;
    players_[0].endpoint_str = "127.0.0.1 (Local)";
    players_[0].ping_ms = 0.0f;
    players_[0].last_seen = std::chrono::steady_clock::now();

    LogEvent("Server started on port " + std::to_string(port));
    LOGI("NetworkManager: Server started on port %u", port);
    return true;
}

bool NetworkManager::ConnectTo(const std::string& host_ip, uint16_t port) {
    Disconnect();
    std::lock_guard<std::mutex> lock(mutex_);

    host_ip_ = host_ip;
    port_ = port;
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd_ < 0) {
        LOGE("NetworkManager: Failed to create socket for client");
        return false;
    }

    SetNonBlocking(socket_fd_);

    sockaddr_in* target = reinterpret_cast<sockaddr_in*>(server_sockaddr_);
    std::memset(target, 0, sizeof(sockaddr_in));
    target->sin_family = AF_INET;
    target->sin_port = htons(port);
    if (inet_pton(AF_INET, host_ip.c_str(), &target->sin_addr) <= 0) {
        LOGE("NetworkManager: Invalid IP address %s", host_ip.c_str());
        CLOSESOCKET(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    server_socklen_ = sizeof(sockaddr_in);

    mode_ = NetMode::Client;
    local_player_id_ = 255; // Unassigned until Ack

    // Send Join Request
    PacketJoinRequest req{};
    req.header.magic = kProtocolMagic;
    req.header.type = static_cast<uint8_t>(MsgType::JoinRequest);
    req.header.player_id = 255;
    req.header.sequence = ++sequence_counter_;
    req.header.timestamp_ms = GetTimeMs();
    std::strncpy(req.player_name, local_player_name_.c_str(), sizeof(req.player_name) - 1);

    SendPacket(&req, sizeof(req), server_sockaddr_, server_socklen_);

    LogEvent("Connecting to " + host_ip + ":" + std::to_string(port) + "...");
    LOGI("NetworkManager: Sent JoinRequest to %s:%u", host_ip.c_str(), port);
    return true;
}

void NetworkManager::Disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != NetMode::Offline && socket_fd_ >= 0) {
        if (mode_ == NetMode::Client && local_player_id_ != 255) {
            PacketHeader disc{};
            disc.magic = kProtocolMagic;
            disc.type = static_cast<uint8_t>(MsgType::Disconnect);
            disc.player_id = local_player_id_;
            disc.sequence = ++sequence_counter_;
            disc.timestamp_ms = GetTimeMs();
            SendPacket(&disc, sizeof(disc), server_sockaddr_, server_socklen_);
        }
        CLOSESOCKET(socket_fd_);
        socket_fd_ = -1;
    }
    mode_ = NetMode::Offline;
    local_player_id_ = 0;
    for (auto& p : players_) p = RemotePlayer{};
    for (auto& c : host_clients_) c = HostClient{};
    LogEvent("Network disconnected");
}

void NetworkManager::SendPacket(const void* data, size_t size, void* dest_addr, uint32_t addr_len) {
    if (socket_fd_ < 0 || !dest_addr || addr_len == 0) return;
    sendto(socket_fd_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr*>(dest_addr), addr_len);
}

void NetworkManager::LogEvent(const std::string& msg) {
    event_logs_.push_back(msg);
    if (event_logs_.size() > 50) {
        event_logs_.erase(event_logs_.begin());
    }
}

std::vector<std::string> NetworkManager::GetEventLogs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return event_logs_;
}

std::vector<RemotePlayer> NetworkManager::GetPlayers() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RemotePlayer> result;
    for (size_t i = 0; i < kMaxPlayers; ++i) {
        if (players_[i].active) {
            result.push_back(players_[i]);
        }
    }
    return result;
}

void NetworkManager::Update(float dt) {
    if (mode_ == NetMode::Offline) return;

    ProcessIncomingPackets();
    CheckTimeouts();

    time_since_last_ping_ += dt;
    if (time_since_last_ping_ >= 1.0f) {
        time_since_last_ping_ = 0.0f;
        if (mode_ == NetMode::Client && IsConnected()) {
            PacketHeader ping{};
            ping.magic = kProtocolMagic;
            ping.type = static_cast<uint8_t>(MsgType::Ping);
            ping.player_id = local_player_id_;
            ping.sequence = ++sequence_counter_;
            ping.timestamp_ms = GetTimeMs();
            SendPacket(&ping, sizeof(ping), server_sockaddr_, server_socklen_);
        }
    }
}

void NetworkManager::ProcessIncomingPackets() {
    if (socket_fd_ < 0) return;

    uint8_t buffer[2048];
    sockaddr_storage sender_addr{};
    socklen_t sender_len = sizeof(sender_addr);

    while (true) {
        int bytes = recvfrom(socket_fd_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                             reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);
        if (bytes <= 0) break; // No more data (non-blocking)

        HandlePacket(buffer, static_cast<size_t>(bytes), &sender_addr, sender_len);
    }
}

void NetworkManager::HandlePacket(const uint8_t* data, size_t size, void* sender_addr, uint32_t addr_len) {
    if (size < sizeof(PacketHeader)) return;

    const auto* hdr = reinterpret_cast<const PacketHeader*>(data);
    if (hdr->magic != kProtocolMagic) return;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto* sa = reinterpret_cast<const sockaddr*>(sender_addr);

    MsgType type = static_cast<MsgType>(hdr->type);

    if (mode_ == NetMode::Host) {
        // HOST PACKET HANDLING
        if (type == MsgType::JoinRequest) {
            if (size < sizeof(PacketJoinRequest)) return;
            const auto* req = reinterpret_cast<const PacketJoinRequest*>(data);

            // Find existing or free slot for client
            int assigned_slot = -1;
            for (size_t i = 1; i < kMaxPlayers; ++i) {
                if (host_clients_[i].active && SockAddrEqual(sa, reinterpret_cast<const sockaddr*>(host_clients_[i].raw_addr))) {
                    assigned_slot = static_cast<int>(i);
                    break;
                }
            }
            if (assigned_slot < 0) {
                for (size_t i = 1; i < kMaxPlayers; ++i) {
                    if (!host_clients_[i].active) {
                        assigned_slot = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (assigned_slot > 0) {
                auto& client = host_clients_[assigned_slot];
                client.active = true;
                client.id = static_cast<uint8_t>(assigned_slot);
                client.name = req->player_name[0] ? req->player_name : ("Player " + std::to_string(assigned_slot + 1));
                std::memcpy(client.raw_addr, sender_addr, addr_len);
                client.addr_len = addr_len;
                client.last_seen = std::chrono::steady_clock::now();

                auto& player = players_[assigned_slot];
                player.active = true;
                player.id = client.id;
                player.name = client.name;
                player.endpoint_str = SockAddrToString(sa);
                player.last_seen = client.last_seen;

                // Send Join Ack
                PacketJoinAck ack{};
                ack.header.magic = kProtocolMagic;
                ack.header.type = static_cast<uint8_t>(MsgType::JoinAck);
                ack.header.player_id = 0;
                ack.header.sequence = ++sequence_counter_;
                ack.header.timestamp_ms = GetTimeMs();
                ack.assigned_id = client.id;
                ack.max_players = static_cast<uint8_t>(kMaxPlayers);
                std::strncpy(ack.server_name, "Black Dedicated Server", sizeof(ack.server_name) - 1);

                SendPacket(&ack, sizeof(ack), sender_addr, addr_len);

                LogEvent(client.name + " joined from " + player.endpoint_str);
                LOGI("NetworkManager: Client %u (%s) joined", client.id, client.name.c_str());
            }
            return;
        }

        // Validate client
        uint8_t sender_id = hdr->player_id;
        if (sender_id >= kMaxPlayers || !host_clients_[sender_id].active) return;
        host_clients_[sender_id].last_seen = std::chrono::steady_clock::now();
        players_[sender_id].last_seen = host_clients_[sender_id].last_seen;

        if (type == MsgType::Ping) {
            PacketHeader pong{};
            pong.magic = kProtocolMagic;
            pong.type = static_cast<uint8_t>(MsgType::Pong);
            pong.player_id = 0;
            pong.sequence = hdr->sequence;
            pong.timestamp_ms = hdr->timestamp_ms;
            SendPacket(&pong, sizeof(pong), sender_addr, addr_len);
            return;
        }

        if (type == MsgType::StateUpdate) {
            if (size < sizeof(PacketState)) return;
            const auto* st = reinterpret_cast<const PacketState*>(data);
            players_[sender_id].state = st->state;

            // Forward state to other clients
            for (size_t i = 1; i < kMaxPlayers; ++i) {
                if (i != sender_id && host_clients_[i].active) {
                    SendPacket(data, size, host_clients_[i].raw_addr, host_clients_[i].addr_len);
                }
            }
            return;
        }

        if (type == MsgType::Shoot) {
            if (size < sizeof(PacketShoot)) return;
            players_[sender_id].just_fired = true;
            // Broadcast shoot to all other clients
            for (size_t i = 1; i < kMaxPlayers; ++i) {
                if (i != sender_id && host_clients_[i].active) {
                    SendPacket(data, size, host_clients_[i].raw_addr, host_clients_[i].addr_len);
                }
            }
            return;
        }

        if (type == MsgType::Hit) {
            if (size < sizeof(PacketHit)) return;
            const auto* hit = reinterpret_cast<const PacketHit*>(data);
            if (hit->victim_id < kMaxPlayers && players_[hit->victim_id].active) {
                players_[hit->victim_id].state.health -= hit->damage;
                if (players_[hit->victim_id].state.health <= 0.0f) {
                    players_[hit->victim_id].state.health = 100.0f; // Respawn
                    players_[sender_id].state.frags++;
                    players_[hit->victim_id].state.deaths++;
                    LogEvent(players_[sender_id].name + " fragged " + players_[hit->victim_id].name);
                }
            }
            // Broadcast hit
            for (size_t i = 1; i < kMaxPlayers; ++i) {
                if (host_clients_[i].active) {
                    SendPacket(data, size, host_clients_[i].raw_addr, host_clients_[i].addr_len);
                }
            }
            return;
        }

        if (type == MsgType::Disconnect) {
            LogEvent(players_[sender_id].name + " disconnected");
            host_clients_[sender_id] = HostClient{};
            players_[sender_id] = RemotePlayer{};
            return;
        }

        if (type == MsgType::Chat) {
            if (size < sizeof(PacketChat)) return;
            const auto* chat = reinterpret_cast<const PacketChat*>(data);
            LogEvent(players_[sender_id].name + ": " + chat->message);
            // Forward chat
            for (size_t i = 1; i < kMaxPlayers; ++i) {
                if (i != sender_id && host_clients_[i].active) {
                    SendPacket(data, size, host_clients_[i].raw_addr, host_clients_[i].addr_len);
                }
            }
            return;
        }

    } else if (mode_ == NetMode::Client) {
        // CLIENT PACKET HANDLING
        if (type == MsgType::JoinAck) {
            if (size < sizeof(PacketJoinAck)) return;
            const auto* ack = reinterpret_cast<const PacketJoinAck*>(data);
            local_player_id_ = ack->assigned_id;
            LogEvent("Connected! Assigned Player ID: " + std::to_string(local_player_id_));
            LOGI("NetworkManager: Joined as player ID %u", local_player_id_);
            return;
        }

        if (type == MsgType::Pong) {
            uint32_t now = GetTimeMs();
            if (now >= hdr->timestamp_ms) {
                current_ping_ms_ = static_cast<float>(now - hdr->timestamp_ms);
            }
            return;
        }

        if (type == MsgType::StateUpdate) {
            if (size < sizeof(PacketState)) return;
            const auto* st = reinterpret_cast<const PacketState*>(data);
            uint8_t pid = st->header.player_id;
            if (pid < kMaxPlayers && pid != local_player_id_) {
                players_[pid].active = true;
                players_[pid].id = pid;
                if (players_[pid].name.empty()) {
                    players_[pid].name = (pid == 0) ? "Host" : ("Player " + std::to_string(pid + 1));
                }
                players_[pid].state = st->state;
                players_[pid].last_seen = std::chrono::steady_clock::now();
            }
            return;
        }

        if (type == MsgType::Shoot) {
            if (size < sizeof(PacketShoot)) return;
            uint8_t pid = hdr->player_id;
            if (pid < kMaxPlayers && pid != local_player_id_) {
                players_[pid].just_fired = true;
            }
            return;
        }

        if (type == MsgType::Hit) {
            if (size < sizeof(PacketHit)) return;
            const auto* hit = reinterpret_cast<const PacketHit*>(data);
            if (hit->victim_id == local_player_id_) {
                // We took damage!
                LogEvent("Hit by Player " + std::to_string(hdr->player_id) + " (-" + std::to_string(int(hit->damage)) + " HP)");
            }
            return;
        }

        if (type == MsgType::Chat) {
            if (size < sizeof(PacketChat)) return;
            const auto* chat = reinterpret_cast<const PacketChat*>(data);
            std::string sender_name = (hdr->player_id == 0) ? "Host" : ("Player " + std::to_string(hdr->player_id + 1));
            LogEvent(sender_name + ": " + chat->message);
            return;
        }
    }
}

void NetworkManager::CheckTimeouts() {
    auto now = std::chrono::steady_clock::now();

    if (mode_ == NetMode::Host) {
        for (size_t i = 1; i < kMaxPlayers; ++i) {
            if (host_clients_[i].active) {
                float elapsed = std::chrono::duration<float>(now - host_clients_[i].last_seen).count();
                if (elapsed > kTimeoutSeconds) {
                    LogEvent(players_[i].name + " timed out");
                    LOGI("NetworkManager: Client %u timed out", static_cast<uint32_t>(i));
                    host_clients_[i] = HostClient{};
                    players_[i] = RemotePlayer{};
                }
            }
        }
    } else if (mode_ == NetMode::Client && IsConnected()) {
        float elapsed = std::chrono::duration<float>(now - players_[0].last_seen).count();
        if (elapsed > kTimeoutSeconds * 2.0f && players_[0].active) {
            LogEvent("Connection to server lost (timeout)");
            Disconnect();
        }
    }
}

void NetworkManager::SendLocalState(const PlayerStateData& state) {
    if (!IsConnected()) return;

    PacketState pkt{};
    pkt.header.magic = kProtocolMagic;
    pkt.header.type = static_cast<uint8_t>(MsgType::StateUpdate);
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = ++sequence_counter_;
    pkt.header.timestamp_ms = GetTimeMs();
    pkt.state = state;

    if (mode_ == NetMode::Host) {
        // Update local state in host's array
        players_[0].state = state;
        // Broadcast to all active clients
        for (size_t i = 1; i < kMaxPlayers; ++i) {
            if (host_clients_[i].active) {
                SendPacket(&pkt, sizeof(pkt), host_clients_[i].raw_addr, host_clients_[i].addr_len);
            }
        }
    } else if (mode_ == NetMode::Client) {
        SendPacket(&pkt, sizeof(pkt), server_sockaddr_, server_socklen_);
    }
}

void NetworkManager::BroadcastShoot(const float origin[3], const float dir[3], uint32_t weapon_id) {
    if (!IsConnected()) return;

    PacketShoot pkt{};
    pkt.header.magic = kProtocolMagic;
    pkt.header.type = static_cast<uint8_t>(MsgType::Shoot);
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = ++sequence_counter_;
    pkt.header.timestamp_ms = GetTimeMs();
    std::memcpy(pkt.origin, origin, sizeof(pkt.origin));
    std::memcpy(pkt.dir, dir, sizeof(pkt.dir));
    pkt.weapon_id = weapon_id;

    if (mode_ == NetMode::Host) {
        for (size_t i = 1; i < kMaxPlayers; ++i) {
            if (host_clients_[i].active) {
                SendPacket(&pkt, sizeof(pkt), host_clients_[i].raw_addr, host_clients_[i].addr_len);
            }
        }
    } else if (mode_ == NetMode::Client) {
        SendPacket(&pkt, sizeof(pkt), server_sockaddr_, server_socklen_);
    }
}

void NetworkManager::SendHit(uint8_t victim_id, float damage, const float hit_pos[3]) {
    if (!IsConnected()) return;

    PacketHit pkt{};
    pkt.header.magic = kProtocolMagic;
    pkt.header.type = static_cast<uint8_t>(MsgType::Hit);
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = ++sequence_counter_;
    pkt.header.timestamp_ms = GetTimeMs();
    pkt.victim_id = victim_id;
    pkt.damage = damage;
    std::memcpy(pkt.hit_pos, hit_pos, sizeof(pkt.hit_pos));

    if (mode_ == NetMode::Host) {
        if (victim_id < kMaxPlayers && players_[victim_id].active) {
            players_[victim_id].state.health -= damage;
            if (players_[victim_id].state.health <= 0.0f) {
                players_[victim_id].state.health = 100.0f;
                players_[0].state.frags++;
                players_[victim_id].state.deaths++;
                LogEvent("You fragged " + players_[victim_id].name);
            }
        }
        for (size_t i = 1; i < kMaxPlayers; ++i) {
            if (host_clients_[i].active) {
                SendPacket(&pkt, sizeof(pkt), host_clients_[i].raw_addr, host_clients_[i].addr_len);
            }
        }
    } else if (mode_ == NetMode::Client) {
        SendPacket(&pkt, sizeof(pkt), server_sockaddr_, server_socklen_);
    }
}

void NetworkManager::SendChatMessage(const std::string& msg) {
    if (!IsConnected()) return;

    PacketChat pkt{};
    pkt.header.magic = kProtocolMagic;
    pkt.header.type = static_cast<uint8_t>(MsgType::Chat);
    pkt.header.player_id = local_player_id_;
    pkt.header.sequence = ++sequence_counter_;
    pkt.header.timestamp_ms = GetTimeMs();
    std::strncpy(pkt.message, msg.c_str(), sizeof(pkt.message) - 1);

    LogEvent(local_player_name_ + " (You): " + msg);

    if (mode_ == NetMode::Host) {
        for (size_t i = 1; i < kMaxPlayers; ++i) {
            if (host_clients_[i].active) {
                SendPacket(&pkt, sizeof(pkt), host_clients_[i].raw_addr, host_clients_[i].addr_len);
            }
        }
    } else if (mode_ == NetMode::Client) {
        SendPacket(&pkt, sizeof(pkt), server_sockaddr_, server_socklen_);
    }
}

} // namespace net
