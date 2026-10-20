// Standalone localhost integration test for NetworkManager's host authority.
// Build: clang++ -std=c++20 -Isrc -Isdk/runtime tests/test_multiplayer_network.cpp \
//        src/net/network.cpp sdk/runtime/core/log.cpp -pthread -o /tmp/test_multiplayer_network
#ifndef _WIN32

#include "net/network.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

[[noreturn]] void Fail(const char* message, int line) {
    throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + message);
}

#define CHECK(expr) do { if (!(expr)) Fail(#expr, __LINE__); } while (false)

uint16_t FindFreeUdpPort() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    close(fd);
    return ntohs(addr.sin_port);
}

enum class HostOp : uint8_t { Query, SetDamage, SetScale, Heal, Reset, Quit };

struct HostCommand {
    HostOp op;
    uint8_t player_id;
    float value;
};

struct HostReply {
    int32_t ok;
    int32_t found;
    float health;
    float damage_scale;
    uint16_t frags;
    uint16_t deaths;
};

bool ReadExact(int fd, void* ptr, size_t size) {
    auto* out = static_cast<uint8_t*>(ptr);
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, out + done, size - done);
        if (n > 0) done += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool WriteExact(int fd, const void* ptr, size_t size) {
    const auto* in = static_cast<const uint8_t*>(ptr);
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, in + done, size - done);
        if (n > 0) done += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

void HostProcess(int command_fd, int reply_fd, uint16_t port) {
    auto& net = net::NetworkManager::Get();
    if (!net.StartHost(port)) _exit(2);
    const uint8_t ready = 0xA5;
    if (!WriteExact(reply_fd, &ready, sizeof(ready))) _exit(3);

    for (;;) {
        net.Update(0.005f);

        pollfd pfd{command_fd, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            HostCommand cmd{};
            if (!ReadExact(command_fd, &cmd, sizeof(cmd))) _exit(4);
            HostReply reply{};
            switch (cmd.op) {
                case HostOp::Query: {
                    reply.ok = 1;
                    reply.damage_scale = net.GetDamageScale();
                    for (const auto& p : net.GetPlayers()) {
                        if (p.id == cmd.player_id) {
                            reply.found = 1;
                            reply.health = p.state.health;
                            reply.frags = p.state.frags;
                            reply.deaths = p.state.deaths;
                            break;
                        }
                    }
                    break;
                }
                case HostOp::SetDamage:
                    reply.ok = net.SetDamageEnabled(cmd.value != 0.0f);
                    break;
                case HostOp::SetScale:
                    reply.ok = net.SetDamageScale(cmd.value);
                    reply.damage_scale = net.GetDamageScale();
                    break;
                case HostOp::Heal:
                    reply.ok = net.AdminHealPlayer(cmd.player_id);
                    break;
                case HostOp::Reset:
                    reply.ok = net.AdminResetStats();
                    break;
                case HostOp::Quit:
                    reply.ok = 1;
                    net.Disconnect();
                    WriteExact(reply_fd, &reply, sizeof(reply));
                    _exit(0);
            }
            net.Update(0.005f);
            if (!WriteExact(reply_fd, &reply, sizeof(reply))) _exit(5);
        }
        usleep(1000);
    }
}

class LocalHost {
public:
    explicit LocalHost(uint16_t port) {
        int commands[2], replies[2];
        CHECK(pipe(commands) == 0 && pipe(replies) == 0);
        pid_ = fork();
        CHECK(pid_ >= 0);
        if (pid_ == 0) {
            close(commands[1]);
            close(replies[0]);
            HostProcess(commands[0], replies[1], port);
            _exit(0);
        }
        close(commands[0]);
        close(replies[1]);
        command_fd_ = commands[1];
        reply_fd_ = replies[0];
        pollfd pfd{reply_fd_, POLLIN, 0};
        CHECK(poll(&pfd, 1, 3000) == 1);
        uint8_t ready = 0;
        CHECK(ReadExact(reply_fd_, &ready, sizeof(ready)) && ready == 0xA5);
    }

    ~LocalHost() {
        if (pid_ <= 0) return;
        HostCommand cmd{HostOp::Quit, 0, 0};
        if (WriteExact(command_fd_, &cmd, sizeof(cmd))) {
            HostReply reply{};
            pollfd pfd{reply_fd_, POLLIN, 0};
            if (poll(&pfd, 1, 1000) > 0) ReadExact(reply_fd_, &reply, sizeof(reply));
        }
        int status = 0;
        waitpid(pid_, &status, 0);
        close(command_fd_);
        close(reply_fd_);
    }

    HostReply Ask(HostOp op, uint8_t player_id = 0, float value = 0.0f) {
        HostCommand cmd{op, player_id, value};
        CHECK(WriteExact(command_fd_, &cmd, sizeof(cmd)));
        pollfd pfd{reply_fd_, POLLIN, 0};
        CHECK(poll(&pfd, 1, 3000) == 1);
        HostReply reply{};
        CHECK(ReadExact(reply_fd_, &reply, sizeof(reply)));
        return reply;
    }

private:
    pid_t pid_ = -1;
    int command_fd_ = -1;
    int reply_fd_ = -1;
};

void PumpClient(net::NetworkManager& client, int milliseconds = 150) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 5) {
        client.Update(0.005f);
        usleep(5000);
    }
}

net::RemotePlayer FindPlayer(net::NetworkManager& client, uint8_t id) {
    for (const auto& p : client.GetPlayers()) if (p.id == id) return p;
    return {};
}

void SendRaw(int fd, uint16_t port, const void* packet, size_t size) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(port);
    CHECK(sendto(fd, packet, size, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == static_cast<ssize_t>(size));
}

uint8_t JoinRawClient(int fd, uint16_t port, const char* name) {
    net::PacketJoinRequest req{};
    req.header.magic = net::kProtocolMagic;
    req.header.type = static_cast<uint8_t>(net::MsgType::JoinRequest);
    req.header.player_id = 255;
    std::strncpy(req.player_name, name, sizeof(req.player_name) - 1);
    SendRaw(fd, port, &req, sizeof(req));

    pollfd pfd{fd, POLLIN, 0};
    CHECK(poll(&pfd, 1, 3000) == 1);
    net::PacketJoinAck ack{};
    CHECK(recv(fd, &ack, sizeof(ack), 0) == sizeof(ack));
    CHECK(ack.header.type == static_cast<uint8_t>(net::MsgType::JoinAck));
    CHECK(ack.assigned_id > 0 && ack.assigned_id < net::kMaxPlayers);
    return ack.assigned_id;
}

net::PacketHit HitPacket(uint8_t sender, uint8_t victim, float damage) {
    net::PacketHit hit{};
    hit.header.magic = net::kProtocolMagic;
    hit.header.type = static_cast<uint8_t>(net::MsgType::Hit);
    hit.header.player_id = sender;
    hit.victim_id = victim;
    hit.damage = damage;
    hit.hit_pos[0] = 1.0f;
    hit.hit_pos[1] = 2.0f;
    hit.hit_pos[2] = 3.0f;
    return hit;
}

void Run() {
    const uint16_t port = FindFreeUdpPort();
    LocalHost host(port);

    auto& client = net::NetworkManager::Get();
    CHECK(!client.SetDamageEnabled(false));
    CHECK(!client.SetDamageScale(2.0f));
    CHECK(!client.AdminHealPlayer(0));
    CHECK(!client.AdminResetStats());

    CHECK(client.ConnectTo("127.0.0.1", port));
    PumpClient(client, 250);
    CHECK(client.IsConnected());
    CHECK(client.GetLocalPlayerId() == 1);
    auto self = FindPlayer(client, 1);
    CHECK(self.active && self.state.health == 100.0f);

    // Host must not trust client-reported health or score fields.
    net::PlayerStateData spoof{};
    spoof.pos[0] = 10.0f;
    spoof.forward[2] = 1.0f;
    spoof.health = 1.0f;
    spoof.frags = 60000;
    spoof.deaths = 50000;
    client.SendLocalState(spoof);
    PumpClient(client);
    HostReply state = host.Ask(HostOp::Query, 1);
    CHECK(state.found && state.health == 100.0f && state.frags == 0 && state.deaths == 0);

    // Join a second endpoint and spoof player 1's ID. The source endpoint must
    // win over the packet's claimed ID, so this attempted 90 damage is ignored.
    int raw_fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(raw_fd >= 0);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    CHECK(bind(raw_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    const uint8_t raw_id = JoinRawClient(raw_fd, port, "EndpointProbe");
    CHECK(raw_id == 2);
    auto forged = HitPacket(1, 0, 90.0f);
    SendRaw(raw_fd, port, &forged, sizeof(forged));
    PumpClient(client);
    CHECK(host.Ask(HostOp::Query, 0).health == 100.0f);

    // A real player hit is applied by the host and echoed as authoritative HP.
    const float hit_pos[3] = {1.0f, 2.0f, 3.0f};
    client.SendHit(0, 10.0f, hit_pos);
    PumpClient(client);
    CHECK(host.Ask(HostOp::Query, 0).health == 90.0f);
    CHECK(FindPlayer(client, 0).state.health == 90.0f);

    CHECK(host.Ask(HostOp::SetScale, 0, 2.0f).ok);
    CHECK(host.Ask(HostOp::Query, 0).damage_scale == 2.0f);
    client.SendHit(0, 10.0f, hit_pos);
    PumpClient(client);
    CHECK(host.Ask(HostOp::Query, 0).health == 70.0f);

    CHECK(host.Ask(HostOp::SetDamage, 0, 0.0f).ok);
    client.SendHit(0, 10.0f, hit_pos);
    PumpClient(client);
    CHECK(host.Ask(HostOp::Query, 0).health == 70.0f);
    CHECK(host.Ask(HostOp::SetDamage, 0, 1.0f).ok);
    CHECK(!host.Ask(HostOp::SetScale, 0, NAN).ok);
    CHECK(host.Ask(HostOp::Query, 0).damage_scale == 2.0f);

    // Correct endpoint but malformed finite fields must also be rejected.
    net::PacketState malformed{};
    malformed.header.magic = net::kProtocolMagic;
    malformed.header.type = static_cast<uint8_t>(net::MsgType::StateUpdate);
    malformed.header.player_id = raw_id;
    malformed.state.health = NAN;
    malformed.state.pos[0] = 5.0f;
    malformed.state.forward[2] = 1.0f;
    SendRaw(raw_fd, port, &malformed, sizeof(malformed));
    auto bad_hit = HitPacket(raw_id, 0, INFINITY);
    SendRaw(raw_fd, port, &bad_hit, sizeof(bad_hit));
    PumpClient(client);
    CHECK(host.Ask(HostOp::Query, 0).health == 70.0f);
    CHECK(host.Ask(HostOp::SetScale, 0, 1.0f).ok);

    // Damage the connected client from the second legitimate endpoint, then
    // exercise host-only heal and reset replication to the client's own slot.
    auto hit_client = HitPacket(raw_id, 1, 25.0f);
    SendRaw(raw_fd, port, &hit_client, sizeof(hit_client));
    PumpClient(client);
    CHECK(host.Ask(HostOp::Query, 1).health == 75.0f);
    CHECK(FindPlayer(client, 1).state.health == 75.0f);
    CHECK(host.Ask(HostOp::Heal, 1).ok);
    PumpClient(client);
    CHECK(FindPlayer(client, 1).state.health == 100.0f);

    // Kill the host to create a server-side frag, then verify reset reaches both
    // the server record and the client's local authoritative score cache.
    client.SendHit(0, 100.0f, hit_pos);
    PumpClient(client);
    CHECK(host.Ask(HostOp::Query, 1).frags == 1);
    CHECK(host.Ask(HostOp::Reset).ok);
    PumpClient(client);
    state = host.Ask(HostOp::Query, 1);
    CHECK(state.health == 100.0f && state.frags == 0 && state.deaths == 0);
    self = FindPlayer(client, 1);
    CHECK(self.state.health == 100.0f && self.state.frags == 0 && self.state.deaths == 0);

    close(raw_fd);
    client.Disconnect();
}

} // namespace

int main() {
    try {
        Run();
        std::puts("multiplayer network integration test passed");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "multiplayer network integration test failed: %s\n", ex.what());
        return 1;
    }
}

#else
int main() { return 0; }
#endif
