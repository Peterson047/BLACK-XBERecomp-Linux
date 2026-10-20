// The mission layer of the co-op session.
//
// The mission of a level is a table of objectives and a bus of named events.
// The two processes load the same table, so the table is not replicated: what
// has to be the same is the live state of the objectives, and the live state
// advances because of the events. So the unit of replication is the named
// event.
//
// The host captures the events the engine fires and publishes them. The guest
// is a replica: it lets the host's events happen in its own bus and does not
// let its own events happen at all, because the guest is never a source. Its
// own matcher then advances its own objective exactly like the host's.
//
// The engine's calls:
//
//   0x0012B8E0  the level script fires a named event. eax is the kind of the
//               event (0 to 3), ecx the trigger it comes from, esi the bus and
//               the stack holds the text of the event.
//   0x0012B790  the bus takes an event. eax is the bus and the stack holds
//               the name, the text, the position and the count.
//   0x001BAA40  the "use" of an object is registered in the list of the
//               requests of use.
//   0x000BE550  the frame counter, once a frame.
//
// The proof of each offset is in .agents/teamwork/coop/LAYOUT.md, section 7.

#include "game/coop_mission.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/layout.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout::coop;

namespace {

// The log's vocabulary is "coop: <verb> key=value key=value ...". A strict
// parser reads these, so a value never has a space in it.
std::string Safe(const char* text, size_t max) {
    std::string out;
    for (size_t i = 0; i < max && text[i]; i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        out += (std::isalnum(c) || c == '_' || c == '-' || c == '.') ? char(c) : '_';
    }
    return out;
}

// The context of the guest thread, for the calls the mission makes into the
// engine. The frame tick sets it before it pumps the mission.
X86Context* g_context = nullptr;

// The bus of this process, the one the engine's own events go to.
uint32_t g_bus = 0;

}  // namespace

namespace game {
namespace coop {

Mission& Mission::Get() {
    static Mission mission;
    return mission;
}

void Mission::SetHost(bool host) { host_ = host; }

void Mission::ResetRound(uint32_t epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = epoch;
    events_ = 0;
    unpaired_ = 0;
    divergent_ = 0;
    has_remote_ = false;
    remote_count_ = 0;
    remote_done_ = 0;
    ended_ = false;
    summary_ = false;
    divergence_logged_ = false;
    divergence_count_ = 0;
    divergence_done_ = 0;
    sequence_ = 0;
    published_count_ = 0;
    published_done_ = 0;
    outbox_.clear();
    inbox_.clear();
}

void Mission::BeginFire(uint32_t kind) {
    kind_ = uint8_t(kind);
    has_kind_ = true;
}

void Mission::EndFire() { has_kind_ = false; }

void Mission::OnBusEvent(uint32_t bus, const char* name, const char* text, const float pos[3], uint32_t count) {
    if (bus) g_bus = bus;
    if (replaying_) return;  // the guest is firing the host's event: not a new one

    NamedEvent event;
    std::snprintf(event.name, sizeof(event.name), "%s", name ? name : "");
    std::snprintf(event.text, sizeof(event.text), "%s", text ? text : "");
    event.count = count;
    event.kind = has_kind_ ? kind_ : 0;
    event.pos[0] = pos[0];
    event.pos[1] = pos[1];
    event.pos[2] = pos[2];
    event.event_id = NameHash(event.name, std::strlen(event.name));

    if (!host_) return;  // the guest never publishes anything

    LOGI("coop: event id=%u kind=%u name=%s count=%u pos=%g,%g,%g", event.event_id, event.kind,
         Safe(event.name, sizeof(event.name)).c_str(), event.count, event.pos[0], event.pos[1], event.pos[2]);
    OnNamedEvent(event);
}

void Mission::OnNamedEvent(const NamedEvent& event) {
    if (!host_) return;  // the guest never publishes anything

    net::coop::Event wire{};
    wire.state.event_type = uint8_t(net::coop::EventType::NamedEvent);
    wire.state.event_id = event.event_id;
    wire.state.value = float(event.kind);
    wire.state.extra = event.count;
    wire.state.pos[0] = event.pos[0];
    wire.state.pos[1] = event.pos[1];
    wire.state.pos[2] = event.pos[2];
    std::snprintf(wire.state.text[0], sizeof(wire.state.text[0]), "%s", event.name);
    std::snprintf(wire.state.text[1], sizeof(wire.state.text[1]), "%s", event.text);

    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.size() >= 64) {
        // The network is not draining: the event is lost and the two sides
        // will show it in the divergence.
        LOGE("coop: error what=outbox_full kind=%s", Safe(event.name, sizeof(event.name)).c_str());
        unpaired_++;
        return;
    }
    outbox_.push_back(wire);
    events_++;
}

void Mission::OnInteraction(uint16_t object_type, const float pos[3], float radius) {
    if (!host_) return;
    LOGI("coop: interaction type=%u pos=%g,%g,%g radius=%g", object_type, pos[0], pos[1], pos[2], radius);

    net::coop::Event wire{};
    wire.state.event_type = uint8_t(net::coop::EventType::ObjectInteracted);
    wire.state.object_type = object_type;
    wire.state.radius = radius;
    wire.state.pos[0] = pos[0];
    wire.state.pos[1] = pos[1];
    wire.state.pos[2] = pos[2];

    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.size() >= 64) {
        LOGE("coop: error what=outbox_full kind=interaction");
        unpaired_++;
        return;
    }
    outbox_.push_back(wire);
}

void Mission::OnMissionEnded() {
    if (!host_ || ended_) return;
    ended_ = true;
    ObjectiveState state = ReadObjectives();
    LOGI("coop: mission state=ended objectives=%u done=%u", state.count, state.done);

    net::coop::Event wire{};
    wire.state.event_type = uint8_t(net::coop::EventType::MissionEnded);
    wire.state.value = float(state.done);
    wire.state.extra = state.count;

    std::lock_guard<std::mutex> lock(mutex_);
    outbox_.push_back(wire);
    LogSummaryOnce();
}

void Mission::ApplyEvent(const net::coop::Event& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inbox_.size() >= 64) {
        LOGE("coop: error what=inbox_full type=%u", event.state.event_type);
        unpaired_++;
        return;
    }
    inbox_.push_back(event);
}

bool Mission::NextEvent(net::coop::Event* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.empty()) return false;
    *out = outbox_.front();
    outbox_.pop_front();
    out->state.start_epoch = epoch_;
    out->state.event_seq = ++sequence_;
    return true;
}

void Mission::SetRemoteState(uint32_t count, uint32_t done) {
    std::lock_guard<std::mutex> lock(mutex_);
    has_remote_ = true;
    remote_count_ = count;
    remote_done_ = done;
}

ObjectiveState Mission::ReadObjectives() const {
    ObjectiveState state;
    uint32_t world = R32(layout::kWorld);
    if (!world) return state;
    uint32_t table = R32(world + kObjectiveTable);
    state.count = R32(world + kObjectiveCount);
    for (uint32_t i = 0; i < kObjectiveCounterCount; i++) {
        state.counters[i] = R32(world + kObjectiveCounters + 4 * i);
    }
    for (uint32_t i = 0; i < state.count && i < kMaxObjectives; i++) {
        if (!table) break;  // the table of the objectives is not there yet
        uint32_t record = table + i * kObjectiveRecord;
        state.states[i] = uint8_t(R32(record + kObjectiveState));
    }
    state.done = CountDone(state);
    state.complete = SumCounters(state);
    return state;
}

bool Mission::CompareState() {
    ObjectiveState state = ReadObjectives();
    uint32_t count = 0;
    uint32_t done = 0;
    bool has = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has = has_remote_;
        count = remote_count_;
        done = remote_done_;
    }
    if (!has) return true;
    if (count == state.count && done == state.done) return true;

    NoteDivergence(count, done, state);
    return false;
}

void Mission::NoteDivergence(uint32_t host_count, uint32_t host_done, const ObjectiveState& local) {
    // The same divergence is only counted and logged once: the check runs
    // every frame and the log would be full of the same line.
    if (divergence_logged_ && divergence_count_ == host_count && divergence_done_ == host_done) return;
    divergence_logged_ = true;
    divergence_count_ = host_count;
    divergence_done_ = host_done;
    divergent_++;
    LOGE("coop: divergence what=objectives host=%u guest=%u objectives=%u done=%u", host_done, local.done,
         local.count, local.done);
}

void Mission::Pump() {
    // What the host sent, applied here.
    for (;;) {
        net::coop::Event event;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inbox_.empty()) break;
            event = inbox_.front();
            inbox_.pop_front();
        }
        switch (net::coop::EventType(event.state.event_type)) {
            case net::coop::EventType::NamedEvent: {
                NamedEvent named;
                named.event_id = event.state.event_id;
                named.kind = uint8_t(event.state.value);
                named.count = event.state.extra;
                named.pos[0] = event.state.pos[0];
                named.pos[1] = event.state.pos[1];
                named.pos[2] = event.state.pos[2];
                std::snprintf(named.name, sizeof(named.name), "%s", event.state.text[0]);
                std::snprintf(named.text, sizeof(named.text), "%s", event.state.text[1]);
                LOGI("coop: event id=%u kind=%u name=%s count=%u pos=%g,%g,%g", named.event_id, named.kind,
                     Safe(named.name, sizeof(named.name)).c_str(), named.count, named.pos[0], named.pos[1],
                     named.pos[2]);
                FireLocal(named);
                events_++;
                break;
            }
            case net::coop::EventType::ObjectiveChanged:
                SetRemoteState(event.state.extra, uint32_t(event.state.value));
                CompareState();
                break;
            case net::coop::EventType::MissionEnded:
                ended_ = true;
                LOGI("coop: mission state=ended objectives=%u done=%u", event.state.extra,
                     uint32_t(event.state.value));
                LogSummaryOnce();
                break;
            case net::coop::EventType::ObjectInteracted:
                // The use of an object changes the world, not the mission: the
                // world layer has the state of the object. What the mission
                // sees of it is the named event the use fires.
                LOGI("coop: interaction type=%u pos=%g,%g,%g radius=%g", event.state.object_type,
                     event.state.pos[0], event.state.pos[1], event.state.pos[2], event.state.radius);
                break;
            default:
                break;
        }
    }

    // The state of the mission, and the end of it.
    ObjectiveState state = ReadObjectives();
    if (!state.count) return;

    uint32_t remote_count = 0;
    uint32_t remote_done = 0;
    bool has_remote = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_remote = has_remote_;
        remote_count = remote_count_;
        remote_done = remote_done_;
    }
    if (has_remote && (remote_count != state.count || remote_done != state.done)) {
        NoteDivergence(remote_count, remote_done, state);
    }

    if (!host_) return;
    if (published_count_ != state.count || published_done_ != state.done) {
        published_count_ = state.count;
        published_done_ = state.done;
        LOGI("coop: objective count=%u done=%u complete=%u c0=%u c1=%u c2=%u c3=%u c4=%u c5=%u", state.count, state.done,
             state.complete, state.counters[0], state.counters[1], state.counters[2], state.counters[3],
             state.counters[4], state.counters[5]);
        net::coop::Event wire{};
        wire.state.event_type = uint8_t(net::coop::EventType::ObjectiveChanged);
        wire.state.value = float(state.done);
        wire.state.extra = state.count;
        std::lock_guard<std::mutex> lock(mutex_);
        outbox_.push_back(wire);
    }
    if (!ended_ && MissionFinished(state)) OnMissionEnded();
}

void Mission::FireLocal(const NamedEvent& event) {
    // The guest fires the event of the host in its own bus. The queue push is
    // the function the engine uses to put an event in the bus, so the guest's
    // bus gets exactly the event the host's bus got: the same name, the same
    // text, the same position and the same count. The guest's own matcher
    // then advances the guest's own objective.
    //
    // The firing routine (0x0012B8E0) is not the entry point here: it takes
    // the trigger of the event, and the trigger is a runtime object of the
    // host, whose address says nothing in the guest. What the event carries
    // is what the bus takes, and that is what is replicated.
    uint32_t bus = g_bus ? g_bus : R32(kEventBus);
    if (!bus) {
        LOGE("coop: error what=no_bus");
        unpaired_++;
        return;
    }
    if (!g_context) {
        LOGE("coop: error what=no_context");
        unpaired_++;
        return;
    }

    // The strings and the position have to live in the guest's memory: the
    // engine copies them from there.
    constexpr uint32_t kNameBytes = 128;
    constexpr uint32_t kTextBytes = 128;
    uint32_t block = mem::AllocSystem(kNameBytes + kTextBytes + 16, 16);
    if (!block) {
        LOGE("coop: error what=no_memory");
        unpaired_++;
        return;
    }
    uint32_t name = block;
    uint32_t text = block + kNameBytes;
    uint32_t pos = block + kNameBytes + kTextBytes;
    std::memset(HostPtr(name), 0, kNameBytes);
    std::memset(HostPtr(text), 0, kTextBytes);
    std::memcpy(HostPtr(name), event.name, sizeof(event.name));
    std::memcpy(HostPtr(text), event.text, sizeof(event.text));
    std::memcpy(HostPtr(pos), event.pos, sizeof(event.pos));
    std::memset(static_cast<uint8_t*>(HostPtr(pos)) + sizeof(event.pos), 0, 4);

    replaying_ = true;
    g_context->eax = bus;
    CallGuest(*g_context, kQueueEvent, {name, text, pos, event.count});
    replaying_ = false;
}

void Mission::LogSummary() const {
    ObjectiveState state = ReadObjectives();
    LOGI("%s", SummaryLine(epoch_, state.count, state.done, events_, unpaired_, divergent_).c_str());
}

void Mission::LogSummaryOnce() {
    if (summary_) return;
    summary_ = true;
    LogSummary();
}

}  // namespace coop
}  // namespace game

namespace {

using game::coop::Mission;

// The level script fires a named event. The body of the routine puts the
// event in the bus, so the hook only watches: the kind of the event is the
// one the routine was called with.
X86_EXTERN_FUNC(__imp__sub_0012B8E0);
X86_FUNC(sub_0012B8E0) {
    Mission::Get().BeginFire(ctx.eax);
    __imp__sub_0012B8E0(ctx, base);
    Mission::Get().EndFire();
}

// The bus takes an event: the name, the text, the position and the count.
// This is where the event is whole, so this is where it is captured.
X86_EXTERN_FUNC(__imp__sub_0012B790);
X86_FUNC(sub_0012B790) {
    uint32_t bus = ctx.eax;
    uint32_t name = R32(ctx.esp + 4);
    uint32_t text = R32(ctx.esp + 8);
    uint32_t pos = R32(ctx.esp + 0xC);
    uint32_t count = R32(ctx.esp + 0x10);

    char name_text[96] = {0};
    char body_text[96] = {0};
    float where[3] = {0, 0, 0};
    if (name) std::snprintf(name_text, sizeof(name_text), "%s", ReadCString(name, sizeof(name_text) - 1).c_str());
    if (text) std::snprintf(body_text, sizeof(body_text), "%s", ReadCString(text, sizeof(body_text) - 1).c_str());
    if (pos) std::memcpy(where, HostPtr(pos), sizeof(where));

    Mission::Get().OnBusEvent(bus, name_text, body_text, where, count);

    // The guest is not the source: its own events do not happen here. The
    // events of the host do, and they are the ones that move the mission.
    if (!Mission::Get().IsHost() && Mission::Get().Epoch() != 0 && !Mission::Get().IsReplaying()) {
        // The routine pops its four arguments and returns: the frame it made
        // is given back and the return address is gone.
        uint32_t frame = ctx.ebp;
        ctx.ebp = R32(frame);
        ctx.esp = frame + 24;
        ctx.eax = bus;
        return;
    }
    __imp__sub_0012B790(ctx, base);
}

// The player used an object: the engine registers the request in the list of
// the requests of use and the script of the level picks it up later. The
// object is a runtime object of this process, so what crosses the network is
// its type, not its address: the address of the object says nothing in the
// other process.
X86_EXTERN_FUNC(__imp__sub_001BAA40);
X86_FUNC(sub_001BAA40) {
    uint32_t object = R32(ctx.esp + 4);
    __imp__sub_001BAA40(ctx, base);
    if (!object) return;
    uint16_t type = uint16_t(R32(object + 0xC4));
    float where[3] = {0, 0, 0};
    // The request that was just registered: the first place of the list whose
    // object is this one and that is still waiting to be used. The two rows
    // the engine copies with the request are the point of the use; what they
    // mean exactly is not known, so they only go to the log.
    uint32_t list = R32(kUseRequests);
    if (list) {
        for (uint32_t i = 0; i < kUseRequestSlots; i++) {
            uint32_t place = list + kUseRequestFirst + i * kUseRequestSize;
            if (R32(place + 0x20) != object) continue;
            if (!R32(place + 0x2C)) continue;  // the request was already used
            std::memcpy(where, HostPtr(place + 0x10), sizeof(where));
            break;
        }
    }
    Mission::Get().OnInteraction(type, where, 0.0f);
}

// Once a frame: the mission applies what the host sent and watches the end of
// the level.
X86_EXTERN_FUNC(__imp__sub_000BE550);
X86_FUNC(sub_000BE550) {
    __imp__sub_000BE550(ctx, base);
    g_context = &ctx;
    Mission::Get().Pump();
}

}  // namespace
