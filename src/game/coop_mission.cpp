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
    // The state of the objectives is published again in the new round: the
    // events of the session only exist inside a round, so the one that was
    // published before the round began did not go out.
    objective_logged_ = false;
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

    if (!host_) {
        // The guest's own engine fired the event: it happens here as the
        // engine means it to. The event is remembered so that the copy of
        // the host is not fired a second time: the two would count the same
        // objective twice.
        std::lock_guard<std::mutex> lock(mutex_);
        local_events_.push_back(EventKey(event));
        while (local_events_.size() > 256) local_events_.pop_front();
        return;
    }

    LOGI("coop: event epoch=%u id=%u kind=%u name=%s count=%u pos=%g,%g,%g", epoch_, event.event_id, event.kind,
         Safe(event.name, sizeof(event.name)).c_str(), event.count, event.pos[0], event.pos[1], event.pos[2]);
    OnNamedEvent(event);
}

std::string Mission::EventKey(const NamedEvent& event) {
    char key[224];
    std::snprintf(key, sizeof(key), "%s|%s|%u", event.name, event.text, event.count);
    return key;
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
        LOGE("coop: error epoch=%u what=outbox_full kind=%s", epoch_, Safe(event.name, sizeof(event.name)).c_str());
        unpaired_++;
        return;
    }
    outbox_.push_back(wire);
    events_++;
}

void Mission::OnInteraction(uint32_t object, uint16_t object_type, const float pos[3], float radius) {
    if (!host_) return;
    // The engine registers the request of use again and again while the
    // player holds the button, so the same object only goes to the network
    // once: when another object is used, or when a second went by.
    //
    // The clock is the wall clock, and not the engine's counter of frames:
    // sixty frames is a second at sixty frames a second, and twice that at
    // thirty.
    const auto now = std::chrono::steady_clock::now();
    if (object == last_object_ && now - last_interaction_time_ < kInteractionWindow) return;
    last_object_ = object;
    last_interaction_time_ = now;

    LOGI("coop: interaction epoch=%u type=%u pos=%g,%g,%g radius=%g", epoch_, object_type, pos[0], pos[1], pos[2],
         radius);

    net::coop::Event wire{};
    wire.state.event_type = uint8_t(net::coop::EventType::ObjectInteracted);
    wire.state.object_type = object_type;
    wire.state.radius = radius;
    wire.state.pos[0] = pos[0];
    wire.state.pos[1] = pos[1];
    wire.state.pos[2] = pos[2];

    std::lock_guard<std::mutex> lock(mutex_);
    if (outbox_.size() >= 64) {
        LOGE("coop: error epoch=%u what=outbox_full kind=interaction", epoch_);
        unpaired_++;
        return;
    }
    outbox_.push_back(wire);
}

void Mission::OnMissionEnded() {
    if (!host_ || ended_) return;
    ended_ = true;
    ObjectiveState state = ReadObjectives();
    LOGI("coop: mission epoch=%u state=ended objectives=%u done=%u", epoch_, state.count, state.done);

    net::coop::Event wire{};
    wire.state.event_type = uint8_t(net::coop::EventType::MissionEnded);
    wire.state.value = float(state.done);
    wire.state.extra = state.count;

    std::lock_guard<std::mutex> lock(mutex_);
    outbox_.push_back(wire);
    LogSummaryOnce();
}

void Mission::ApplyEvent(const net::coop::Event& event) {
    // The mission only acts on some of the events. The damage of an actor is
    // the world layer's business, and when it took a place in this inbox,
    // sixty four hits were enough to push out the event of a mission.
    std::lock_guard<std::mutex> lock(mutex_);
    const EventQueueing decision = DecideQueueing(event.state.event_type, inbox_.size(), kInboxCapacity);
    if (decision == EventQueueing::kIgnore) return;
    if (decision == EventQueueing::kFull) {
        LOGE("coop: error epoch=%u what=inbox_full type=%u", epoch_, event.state.event_type);
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
    // The table of the objectives is only there once the level is loaded, and
    // the host's table is there before the guest's is: the two are only
    // compared when this process has its own table.
    if (!state.count) return true;
    if (count == state.count && done == state.done) return true;

    NoteDivergence(count, done, state);
    return false;
}

void Mission::NoteDivergence(uint32_t host_count, uint32_t host_done, const ObjectiveState& local) {
    // The same divergence is only counted and logged once: the check runs
    // every frame and the log would be full of the same line.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (divergence_logged_ && divergence_count_ == host_count && divergence_done_ == host_done) return;
        divergence_logged_ = true;
        divergence_count_ = host_count;
        divergence_done_ = host_done;
        divergent_++;
    }
    LOGE("coop: divergence epoch=%u what=objectives host=%u guest=%u objectives=%u done=%u", epoch_, host_done, local.done,
         local.count, local.done);
}

void Mission::ApplyPending() {
    // The call into the engine is only safe on the thread that runs the
    // guest. On any other thread the events wait for one.
    if (!Current()) return;

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
                if (HasTwin(named)) {
                    // The guest's own engine already fired this event: the
                    // copy of the host would count the same objective twice.
                    LOGI("coop: event epoch=%u id=%u kind=%u name=%s count=%u replayed=0", epoch_, named.event_id,
                         named.kind, Safe(named.name, sizeof(named.name)).c_str(), named.count);
                    break;
                }
                LOGI("coop: event epoch=%u id=%u kind=%u name=%s count=%u pos=%g,%g,%g replayed=1", epoch_,
                     named.event_id, named.kind, Safe(named.name, sizeof(named.name)).c_str(), named.count,
                     named.pos[0], named.pos[1], named.pos[2]);
                FireLocal(named);
                {
                    // The counter is read by the thread that draws the
                    // summary, so it is written under the lock.
                    std::lock_guard<std::mutex> lock(mutex_);
                    events_++;
                }
                break;
            }
            case net::coop::EventType::ObjectiveChanged:
                SetRemoteState(event.state.extra, uint32_t(event.state.value));
                CompareState();
                break;
            case net::coop::EventType::MissionEnded:
                ended_ = true;
                LOGI("coop: mission epoch=%u state=ended objectives=%u done=%u", epoch_, event.state.extra,
                     uint32_t(event.state.value));
                LogSummaryOnce();
                break;
            case net::coop::EventType::ObjectInteracted:
                // The use of an object changes the world, not the mission: the
                // world layer has the state of the object. What the mission
                // sees of it is the named event the use fires.
                LOGI("coop: interaction epoch=%u type=%u pos=%g,%g,%g radius=%g", epoch_, event.state.object_type,
                     event.state.pos[0], event.state.pos[1], event.state.pos[2], event.state.radius);
                break;
            default:
                break;
        }
    }
}

void Mission::Pump() {
    // The state of the mission, and the end of it. This is the bookkeeping of
    // the session: it touches no engine memory, so it can run on any thread.
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
    if (state.count && has_remote && (remote_count != state.count || remote_done != state.done)) {
        NoteDivergence(remote_count, remote_done, state);
    }

    if (!host_) return;
    // A new level is a new table: the state of the mission is logged again.
    uint32_t world = R32(layout::kWorld);
    if (world != last_world_) {
        last_world_ = world;
        objective_logged_ = false;
    }
    bool changed = published_count_ != state.count || published_done_ != state.done;
    if (!objective_logged_ || changed) {
        objective_logged_ = true;
        published_count_ = state.count;
        published_done_ = state.done;
        LOGI("coop: objective epoch=%u count=%u done=%u complete=%u c0=%u c1=%u c2=%u c3=%u c4=%u c5=%u", epoch_,
             state.count, state.done, state.complete, state.counters[0], state.counters[1], state.counters[2],
             state.counters[3], state.counters[4], state.counters[5]);
        if (changed && state.count) {
            net::coop::Event wire{};
            wire.state.event_type = uint8_t(net::coop::EventType::ObjectiveChanged);
            wire.state.value = float(state.done);
            wire.state.extra = state.count;
            std::lock_guard<std::mutex> lock(mutex_);
            outbox_.push_back(wire);
        }
    }
    if (!ended_ && MissionFinished(state)) OnMissionEnded();
}

bool Mission::HasTwin(const NamedEvent& event) {
    // The guest's own engine fires the same events as the host's, one frame
    // or two earlier. The copy of the host that has a twin here is not fired
    // again: the objective would be counted twice.
    const std::string key = EventKey(event);
    std::lock_guard<std::mutex> lock(mutex_);
    if (local_events_.empty()) return false;
    if (local_events_.front() != key) return false;
    local_events_.pop_front();
    return true;
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
        LOGE("coop: error epoch=%u what=no_bus", epoch_);
        unpaired_++;
        return;
    }

    // The strings and the position have to live in the guest's memory: the
    // engine copies them from there.
    constexpr uint32_t kNameBytes = 128;
    constexpr uint32_t kTextBytes = 128;
    uint32_t block = mem::AllocSystem(kNameBytes + kTextBytes + 16, 16);
    if (!block) {
        LOGE("coop: error epoch=%u what=no_memory", epoch_);
        unpaired_++;
        return;
    }
    scratch_allocs_++;
    uint32_t name = block;
    uint32_t text = block + kNameBytes;
    uint32_t pos = block + kNameBytes + kTextBytes;
    std::memset(HostPtr(name), 0, kNameBytes);
    std::memset(HostPtr(text), 0, kTextBytes);
    std::memcpy(HostPtr(name), event.name, sizeof(event.name));
    std::memcpy(HostPtr(text), event.text, sizeof(event.text));
    std::memcpy(HostPtr(pos), event.pos, sizeof(event.pos));
    std::memset(static_cast<uint8_t*>(HostPtr(pos)) + sizeof(event.pos), 0, 4);

    // The call into the engine has to happen on the thread that runs the
    // guest, the same way the world layer does it.
    X86Context& ctx = CurrentContext();
    replaying_ = true;
    ctx.eax = bus;
    CallGuest(ctx, kQueueEvent, {name, text, pos, event.count});
    replaying_ = false;

    // The block goes back to the guest's heap. The engine copied the name,
    // the text and the position into its own slot of the bus (the copy of
    // the name is at bus+0x10E4, the text at bus+0x1164 and the position at
    // bus+0x940), so nothing points into the block after the call. Without
    // this every replayed event left 272 bytes of the guest's address space
    // behind.
    mem::FreeSystem(block);
    scratch_frees_++;
}

void Mission::LogSummary() const {
    ObjectiveState state = ReadObjectives();
    LOGI("%s", SummaryLine(epoch_, state.count, state.done, events_, unpaired_, divergent_).c_str());
    // The guest's memory that the replayed events used. The two numbers have
    // to be the same: a difference is a leak of the guest's address space.
    LOGI("coop: scratch epoch=%u alloc=%u free=%u", epoch_, scratch_allocs_, scratch_frees_);
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

    // The guest is not the source: it never publishes anything of its own.
    // Its own events do happen, though: the engine's own logic expects them,
    // and dropping them leaves the mission of the guest in a state the engine
    // does not expect. The copy of the host is only fired when the guest did
    // not have the event by itself.
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
    Mission::Get().OnInteraction(object, type, where, 0.0f);
}

// The consumer of the bus: the tick of the mission. It runs on the thread
// that runs the guest, so the events of the host are applied here, before the
// engine reads the bus.
X86_EXTERN_FUNC(__imp__sub_001337C0);
X86_FUNC(sub_001337C0) {
    Mission::Get().ApplyPending();
    __imp__sub_001337C0(ctx, base);
}

}  // namespace
