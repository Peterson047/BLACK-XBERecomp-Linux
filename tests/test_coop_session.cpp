// Standalone test of the co-op session: the room, the load barrier, the
// sequence rules and the acknowledgement. Real UDP on localhost, two
// processes.
// Build: clang++ -std=c++20 -Isrc -Isdk/runtime tests/test_coop_session.cpp \
//        src/net/network.cpp src/net/coop_protocol.cpp src/net/coop_session.cpp \
//        sdk/runtime/core/log.cpp -pthread -o /tmp/test_coop_session
#include "net/coop_session.h"

#include <arpa/inet.h>
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace net;
using namespace net::coop;

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

void WriteLine(int fd, const std::string& text) {
    const std::string line = text + "\n";
    size_t written = 0;
    while (written < line.size()) {
        const ssize_t n = write(fd, line.data() + written, line.size() - written);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
}

bool ReadLine(int fd, std::string& out) {
    out.clear();
    char c = 0;
    while (true) {
        const ssize_t n = read(fd, &c, 1);
        if (n <= 0) return !out.empty();
        if (c == '\n') return true;
        out.push_back(c);
    }
}

// The socket is only serviced when the manager is updated, so waiting for a
// line means ticking the network while the wait goes on. Otherwise the two
// sides wait for each other and nobody moves.
class Waiter {
public:
    Waiter(int fd, NetworkManager& network, CoopSession& session)
        : fd_(fd), network_(network), session_(session) {}

    bool Line(std::string& out, int timeout_ms) {
        const uint64_t deadline = NowMs() + static_cast<uint64_t>(timeout_ms);
        while (true) {
            pollfd entry{};
            entry.fd = fd_;
            entry.events = POLLIN;
            const int ready = poll(&entry, 1, 20);
            network_.Update(0.02f);
            session_.Update(0.02f);
            if (ready > 0 && (entry.revents & POLLIN)) return ReadLine(fd_, out);
            if (NowMs() >= deadline) return false;
        }
    }

private:
    static uint64_t NowMs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    int fd_;
    NetworkManager& network_;
    CoopSession& session_;
};

// The part of a packet that follows the header, so a test can call the
// session the way the transport does.
std::vector<uint8_t> EncodeRoom(const Room& room) {
    std::vector<uint8_t> bytes(sizeof(RoomState));
    coop::EncodeRoom(room, bytes.data(), bytes.size());
    return bytes;
}

std::vector<uint8_t> EncodeEvent(const Event& event) {
    std::vector<uint8_t> bytes(sizeof(EventState));
    coop::EncodeEvent(event, bytes.data(), bytes.size());
    return bytes;
}

Event MakeEvent(uint32_t epoch, uint32_t seq) {
    Event event{};
    event.state.start_epoch = epoch;
    event.state.event_seq = seq;
    event.state.event_type = static_cast<uint8_t>(EventType::ActorDamaged);
    event.state.value = 10.0f;
    return event;
}

// The ordering rules, without a network. The session is told about a room
// and then about events, the way the transport would.
void TestOrderingRules() {
    CoopSession session(NetworkManager::Get());
    session.SetEnabled(true);
    session.SetTimeouts(2.0f, 8.0f);

    uint32_t applied = 0;
    uint32_t applied_seq = 0;
    session.SetEventHandler([&](const Event& event) {
        applied++;
        applied_seq = event.state.event_seq;
    });

    // The guest learns the round from the room.
    Room room{};
    room.state.session_id = 1;
    room.state.revision = 1;
    room.state.start_epoch = 4;
    room.state.phase = static_cast<uint8_t>(Phase::InMission);
    const auto room_bytes = EncodeRoom(room);
    session.OnCoopRoom(room_bytes.data(), room_bytes.size());
    CHECK(session.GetStartEpoch() == 4);
    CHECK(session.GetPhase() == Phase::InMission);

    // An event of another round is dropped and does not count as applied.
    Event old_event = MakeEvent(3, 1);
    const auto old_bytes = EncodeEvent(old_event);
    session.OnCoopEvent(old_bytes.data(), old_bytes.size());
    CHECK(applied == 0);
    CHECK(session.GetStaleCount() == 1);

    // The first event of the round is applied.
    Event first = MakeEvent(4, 1);
    const auto first_bytes = EncodeEvent(first);
    session.OnCoopEvent(first_bytes.data(), first_bytes.size());
    CHECK(applied == 1);
    CHECK(applied_seq == 1);

    // The same event again is not applied twice.
    session.OnCoopEvent(first_bytes.data(), first_bytes.size());
    CHECK(applied == 1);
    CHECK(session.GetDroppedEventCount() == 1);

    // ... but it is acknowledged again. The host resends what it has not seen
    // acknowledged, so an event that arrives twice and is not acknowledged
    // the second time is resent for the rest of the session.
    const uint32_t acks = session.GetAckCount();
    session.OnCoopEvent(first_bytes.data(), first_bytes.size());
    CHECK(session.GetAckCount() == acks + 1);
    CHECK(applied == 1);

    // An event that arrives early waits for its turn, and the hole is
    // counted so it is visible.
    Event third = MakeEvent(4, 3);
    const auto third_bytes = EncodeEvent(third);
    session.OnCoopEvent(third_bytes.data(), third_bytes.size());
    CHECK(applied == 1);
    CHECK(session.GetDivergenceCount() == 1);

    // The missing one arrives: the two go through in order.
    Event second = MakeEvent(4, 2);
    const auto second_bytes = EncodeEvent(second);
    session.OnCoopEvent(second_bytes.data(), second_bytes.size());
    CHECK(applied == 3);
    CHECK(applied_seq == 3);
    CHECK(session.GetDivergenceCount() == 1);

    // A room that is not newer is not news.
    const auto old_room_bytes = EncodeRoom(room);
    session.OnCoopRoom(old_room_bytes.data(), old_room_bytes.size());
    CHECK(session.GetRoom().state.revision == 1);
}

// The two processes. The parent hosts, the child joins, and they talk over
// the test's own pipes so the test can drive the order of the steps.
int RunGuest(uint16_t port, int control_fd, int report_fd, int scenario) {
    NetworkManager& network = NetworkManager::Get();
    CoopSession session(network);
    session.SetEnabled(true);
    session.SetTimeouts(2.0f, 8.0f);

    uint32_t applied = 0;
    session.SetEventHandler([&](const Event&) { applied++; });

    if (!network.ConnectTo("127.0.0.1", port)) {
        WriteLine(report_fd, "error connect");
        return 1;
    }
    // The host may not be up yet: the client keeps asking.
    for (int i = 0; i < 200 && !network.IsConnected(); ++i) {
        network.Update(0.02f);
        session.Update(0.02f);
        usleep(20000);
    }
    if (!network.IsConnected()) {
        WriteLine(report_fd, "error connect");
        return 1;
    }
    WriteLine(report_fd, "connected");

    Waiter waiter(control_fd, network, session);
    std::string command;
    while (waiter.Line(command, 10000)) {
        if (command == "loaded") {
            session.SetReady(true);
            session.SetLoaded(true);
            WriteLine(report_fd, "loaded");
        } else if (command == "hold") {
            // The guest stops answering for a while.
            WriteLine(report_fd, "holding");
            usleep(3000000);
            WriteLine(report_fd, "held");
        } else if (command == "epoch") {
            WriteLine(report_fd, "epoch " + std::to_string(session.GetStartEpoch()));
        } else if (command == "applied") {
            WriteLine(report_fd, "applied " + std::to_string(applied));
        } else if (command == "divergent") {
            WriteLine(report_fd, "divergent " + std::to_string(session.GetDivergenceCount()));
        } else if (command == "counters") {
            WriteLine(report_fd, "counters applied=" + std::to_string(applied) +
                                     " stale=" + std::to_string(session.GetStaleCount()) +
                                     " dropped=" + std::to_string(session.GetDroppedEventCount()) +
                                     " divergent=" + std::to_string(session.GetDivergenceCount()) +
                                     " epoch=" + std::to_string(session.GetStartEpoch()));
        } else if (command == "quit") {
            break;
        }
    }
    return 0;
}

int RunHost(uint16_t port, pid_t child, int control_fd, int report_fd, int scenario) {
    NetworkManager& network = NetworkManager::Get();
    CoopSession session(network);
    session.SetEnabled(true);
    session.SetTimeouts(2.0f, 8.0f);

    uint32_t rounds = 0;
    session.SetRoundHandler([&](uint32_t epoch, bool started) {
        if (started) {
            rounds++;
            WriteLine(report_fd, "round " + std::to_string(epoch));
        }
    });

    CHECK(network.StartHost(port));

    Waiter waiter(report_fd, network, session);
    std::string line;

    // The guest has to be in the room before the mission is named.
    CHECK(waiter.Line(line, 10000));
    CHECK(line == "connected");

    if (scenario == 1) {
        // A peer that goes quiet in the lobby is dropped.
        WriteLine(control_fd, "hold");
        CHECK(waiter.Line(line, 10000));
        CHECK(line == "holding");
        for (int i = 0; i < 400; ++i) {
            network.Update(0.02f);
            session.Update(0.02f);
            usleep(10000);
        }
        CHECK(network.GetPlayers().size() == 1);
        WriteLine(control_fd, "quit");
        return 0;
    }

    // The host names the mission. Nobody has loaded yet, so no round starts.
    session.HostStartMission(0);
    for (int i = 0; i < 15; ++i) {
        network.Update(0.02f);
        session.Update(0.02f);
        usleep(10000);
    }
    CHECK(rounds == 0);
    CHECK(session.GetPhase() == Phase::Loading);
    // The peer timeout follows the phase: a peer that is loading is not
    // dropped for being quiet.
    CHECK(network.GetPeerTimeout() > 1.0f);

    // The host loads the level too. Its own bit counts, so the round still
    // waits for the guest.
    session.SetLoaded(true);
    CHECK(rounds == 0);

    // A peer that goes quiet while the level is loading is not dropped: it
    // is reading a level, not broken.
    WriteLine(control_fd, "hold");
    CHECK(waiter.Line(line, 10000));
    CHECK(line == "holding");
    for (int i = 0; i < 400; ++i) {
        network.Update(0.02f);
        session.Update(0.02f);
        usleep(10000);
    }
    CHECK(network.GetPlayers().size() == 2);
    // The guest says it is back.
    CHECK(waiter.Line(line, 10000));
    CHECK(line == "held");

    // The guest reports that it loaded. Now the round starts.
    WriteLine(control_fd, "loaded");
    CHECK(waiter.Line(line, 10000));
    CHECK(line == "loaded");
    for (int i = 0; i < 100 && rounds == 0; ++i) {
        session.Update(0.02f);
        network.Update(0.02f);
        usleep(10000);
    }
    CHECK(rounds == 1);
    CHECK(session.GetPhase() == Phase::InMission);
    CHECK(session.GetStartEpoch() == 1);

    // The guest is in the same round.
    WriteLine(control_fd, "epoch");
    CHECK(waiter.Line(line, 10000));
    CHECK(line == "epoch 1");

    // The host publishes an event. The guest applies it once.
    Event event = MakeEvent(session.GetStartEpoch(), 1);
    session.PublishEvent(event);
    for (int i = 0; i < 100; ++i) {
        session.Update(0.02f);
        network.Update(0.02f);
        usleep(10000);
    }
    WriteLine(control_fd, "counters");
    CHECK(waiter.Line(line, 10000));
    WriteLine(control_fd, "applied");
    CHECK(waiter.Line(line, 10000));
    CHECK(line == "applied 1");

    // The acknowledgement stopped the resending.
    CHECK(session.GetUnackedCount() == 0);

    WriteLine(control_fd, "quit");
    return 0;
}

}  // namespace

int main() {
    try {
        TestOrderingRules();
    } catch (const std::exception& error) {
        printf("coop session test failed: %s\n", error.what());
        return 1;
    }

    const uint16_t port = FindFreeUdpPort();
    int to_guest[2];
    int from_guest[2];
    CHECK(pipe(to_guest) == 0);
    CHECK(pipe(from_guest) == 0);

    // Each scenario gets its own pair of processes: the network manager is
    // a singleton, so two hosts cannot live in one process.
    for (int scenario = 0; scenario < 2; ++scenario) {
        int to_guest[2];
        int from_guest[2];
        CHECK(pipe(to_guest) == 0);
        CHECK(pipe(from_guest) == 0);

        const pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            close(to_guest[1]);
            close(from_guest[0]);
            // The two processes write to the same log file with their own
            // offsets, so they overwrite each other. Give the guest its own.
            const char* guest_log = getenv("COOP_TEST_GUEST_LOG");
            if (guest_log) freopen(guest_log, "w", stderr);
            _exit(RunGuest(port, to_guest[0], from_guest[1], scenario));
        }

        close(to_guest[0]);
        close(from_guest[1]);

        int result = 0;
        try {
            RunHost(port, child, to_guest[1], from_guest[0], scenario);
        } catch (const std::exception& error) {
            printf("coop session test failed (scenario %d): %s\n", scenario, error.what());
            result = 1;
        }

        close(to_guest[1]);
        close(from_guest[0]);
        kill(child, SIGTERM);
        int status = 0;
        waitpid(child, &status, 0);
        if (result != 0) return result;
    }

    puts("coop session test passed");
    return 0;
}
