# MPP Continuation Brief

This document is intended to bring a fresh ChatGPT session up to speed on the design, architecture, and current state of the MPP (Message Processing Platform) project in one pass.

---

## 1. What MPP Is

MPP is a distributed, message-driven runtime for coordinating multiple components (FSMs, NET, XFR, TCK, BLS) using:

* UDP-based BUS sockets (each component has an SBA port)
* JSON control messages
* Finite State Machines loaded from PlantUML (.puml)
* A belief store (BLS) that distributes committed facts
* Tick controllers (TCK) that periodically drive FSM transitions

Everything is orchestrated declaratively by FSMs + beliefs rather than imperative control code.

Core idea:

* Components are small machines driven by register writes and beliefs
* FSMs emit `_send`, `_commit`, and `_tck` directives from state notes
* TCK provides controlled clocking where needed
* BLS distributes beliefs that trigger FSM transitions

---

## 2. Major Components

### BLS (Belief Store)

* Central belief broker
* Receives commits: `{ "subject": "X", "polarity": true }`
* Rebroadcasts beliefs to all FSMs
* FSM transitions are guarded by beliefs

### FSM

* Loads a PlantUML state machine via PUT
* Executes transitions on beliefs
* On state entry, processes:

  * `_send`  → register writes to target component
  * `_commit` → belief commit to BLS
  * `_tck`   → routed to its own TCK only

Important rule:

* `_tck` is stripped and routed only to the FSM’s own TCK
* FSM cannot directly send ticks to another component

### TCK

* Sends periodic `{ "tick": true }` to a target SBA
* Used to drive sampling or polling states

### NET

* Special component that:

  * Uses libnet to transmit raw Ethernet/IP/ICMP
  * Uses pcap to capture packets
* Acts as both TX and RX engine
* Controlled entirely by register writes from FSM-NET via `_send`

Important registers used:

* `libnet_create`, `pcap_create`
* Interface config (mac, ip, filter, etc)
* `tx_fire`
* `rx_fire`
* `net_rx_enable`

NET internally runs RX sampling when `net_rx_enable == true`.

### XFR

* Logical transfer controller
* Has send and recv FSM variants
* Coordinates with FSM-NET
* Emits `FSM.XFR.init`, `FSM.XFR.send_chunk`, waits for `NET.tx_done`, `NET.rx_done`

---

## 3. Architecture Pattern

Each node runs:

* BLS @ 4000
* NET @ 5000
* FSM-NET + FSM-XFR
* TCKs for FSMs

FSMs are loaded dynamically with PUT requests containing the .puml text.

Belief flow drives all coordination:

105 (TX side):
FSM.XFR.start → FSM.XFR.send_chunk → FSM.NET.ready → NET.tx_done → NET.rx_done → FSM.XFR.complete

109 (RX side):
FSM.XFR.init → FSM.NET.ready → NET.rx_done → FSM.XFR.complete

---

## 4. FSM Design Rules Used

### Notes fields

* `_send`   → JSON object written to component registers
* `_commit` → belief commit sent to BLS
* `_tck`    → enables the FSM’s own TCK only

Example:

```json
{
  "_send": { "tx_fire": true },
  "_commit": { "subject": "FSM.NET.ready" }
}
```

FSMs never call code directly — only emit register writes and beliefs.

---

## 5. Current Trial: MPP-XFR over ICMP

Goal:

* Send a payload from host 105 to host 109 using ICMP echo packets
* Fully orchestrated by FSMs and beliefs
* No direct imperative control loops

Transport:

* Raw Ethernet + IPv4 + ICMP
* Payload carried inside ICMP body

FSMs involved:

### fsm-xfr-send.puml (105)

States:

* IDLE → INIT → OPEN → SEND → WAIT_TX → DONE

Key actions:

* INIT: `_send { mode: "send" }`, commit FSM.XFR.init
* SEND: commit FSM.XFR.send_chunk
* WAIT_TX: wait NET.tx_done

### fsm-net-tx.puml (105)

States:

* IDLE → INIT → CONFIGURE → READY → TX → WAIT_TX → WAIT_RX → DONE

Key actions:

* CONFIGURE: setup libnet + pcap + filter
* TX: `_send { tx_fire: true }`
* WAIT_RX: `_send { net_rx_enable: true }`
* DONE: commit FSM.NET.chunk_sent

### fsm-xfr-recv.puml (109)

States:

* IDLE → INIT → OPEN → WAIT_RX → DONE

Key actions:

* INIT: `_send { mode: "recv" }`, commit FSM.XFR.init
* WAIT_RX: wait NET.rx_done

### fsm-net-rx.puml (109)

States:

* IDLE → CONFIGURE → INIT → READY → WAIT_RX → DONE

Key actions:

* INIT: setup pcap + filter, commit FSM.NET.ready
* WAIT_RX: `_send { net_rx_enable: true }`
* DONE: commit NET.rx_done

---

## 6. Key Design Decision: NET RX Sampling

Problem discovered:

* TCK ticks cannot be routed from FSM to NET (FSM strips `_tck`)
* NET shares SBA for control and state
* Using TCK directly to NET clobbered control traffic

Final solution:

* NET has internal RX loop controlled by register:

  * `net_rx_enable = true` → sample pcap periodically
* FSM-NET controls this via `_send`
* No external TCK drives NET

NET behaves as a small machine with its own internal clock when enabled.

---

## 7. Major Bug Discovered (and Root Cause)

Symptom:

* Packet arrives correctly
* FSM transitions to DONE
* `read:true` on NET often returns nothing
* Only works after unrelated later ICMP packet arrives

Observation:

* Correct packet already ingested earlier
* 120-byte ICMP error packet merely wakes the thread

Root cause:

Component base loop blocks on recvfrom()
Snapshots are only published when:

* Control message arrives
* IO wakes loop

After RX completion:

* rx_done set
* belief committed
* FSM finishes
* BUT NET thread not scheduled
* Snapshot not flushed
* `read:true` starves

This is a **control-plane scheduling bug**, not a networking bug.

---

## 8. Correct Fix Applied / Proposed

Whenever NET changes externally-visible state (rx_done, tx_done, rx_len, etc), it must immediately publish a snapshot.

In `do_rx()`:

```cpp
regs_.rx_done = true;
commit("NET.rx_done");
publish_snapshot();   // CRITICAL
```

Likewise after tx_done.

This guarantees:

* Snapshot visible immediately
* `read:true` always works
* No dependence on later traffic

This fix generalizes to all MPP components.

---

## 9. Current Status

Working:

* ICMP payload successfully transmitted from 105 to 109
* Correct packet captured by pcap
* FSM-XFR and FSM-NET flows correct
* Belief choreography stable
* No C++ changes required for main flow (mostly .puml driven)

Remaining issues now purely control-plane / reporting related, not transport.

---

## 10. Next Planned Steps

Immediate:

* Make snapshot publish after RX deterministic
* Stabilize read:true behavior

Then:

1. Extract payload bytes from pcap buffer
2. Add registers:

   * `rx_seq`
   * `rx_payload_len`
   * `rx_payload[]`
3. Add belief context `{ seq, len }`
4. Extend FSM-XFR recv loop to:

   * write chunk to file
   * request next chunk
5. Add window / ack / retransmit FSMs later

End goal:

A complete file transfer protocol implemented purely by:

* FSMs
* Beliefs
* Register writes

With NET as a micro-machine and no imperative transfer loop.

---

## 11. Core Philosophy

* FSMs describe intent
* Beliefs drive synchronization
* Components are small machines
* No hidden control loops
* Network is just another FSM-controlled device

This trial demonstrates that MPP can orchestrate a real transport protocol using only declarative state machines and beliefs.

I have this program running on two computers, 105 for tx and send and 109 for rx and recv.  See below most of the files of code base plus the .puml files currently being used as well as the startup shell scripts:

// ===== fsm =====
// Fsm.hpp
#pragma once

#include "Component.hpp"

#include <string>
#include <map>
#include <vector>
#include <set>

using json = nlohmann::ordered_json;

// -----------------------------------------------------------------------------
// FSM Registers (observable, debuggable, boring)
// -----------------------------------------------------------------------------
struct FsmRegisters
{
    int         sba_        = 0;
    int         target_sba_ = 0;
    int         tck_sba_    = 0;

    bool        run_        = false;
    bool        loaded_     = false;

    std::string current_state_;
    std::string next_state_;
    bool        transition_fired_ = false;

    std::string last_applied_state_;
    std::string last_error_;
};

// -----------------------------------------------------------------------------
// FSM Component (tick-driven, intent-only)
// -----------------------------------------------------------------------------
class Fsm : public mpp::Component<Fsm>
{
public:
    explicit Fsm(int sba);

    // ---- control plane ----
    void apply_snapshot(const json& j);

    // ---- time plane ----
    void on_tick();   // ← THE ONLY PLACE step() is called

    void on_message(const json& j);

protected:
    const char* component_name() const override { return "FSM"; }

private:
    int bls_sba_ = mpp::BLS_PORT;

    std::map<std::string, bool> observed_beliefs_;

    // -------------------------------------------------------------------------
    // FSM definition
    // -------------------------------------------------------------------------
    struct Transition {
        std::string from;
        std::string to;
        json        guards;          // register guards (key=value)
        std::vector<std::string> beliefs; // belief subjects required
    };

    std::string fsm_text_;
    std::vector<std::string> state_order_;
    std::map<std::string, json> state_notes_;
    std::map<std::string, std::vector<Transition>> transitions_;

    // -------------------------------------------------------------------------
    // Runtime belief snapshot (polled from BLS)
    // -------------------------------------------------------------------------
    std::map<std::string, bool> beliefs_;

    // -------------------------------------------------------------------------
    // Registers
    // -------------------------------------------------------------------------
    FsmRegisters regs_;

    // -------------------------------------------------------------------------
    // Core FSM logic
    // -------------------------------------------------------------------------
    void step();   // evaluates transitions exactly once per tick
    bool evaluate_transition(const Transition& t);

    // -------------------------------------------------------------------------
    // Intent routing (note channels)
    // -------------------------------------------------------------------------
    void apply_state_note(const json& note);

    void route_commit(const json& c); // _commit
    void route_send(json payload);    // _send
    void route_tck(const json& t);    // _tck

    // -------------------------------------------------------------------------
    // BLS access (read-only)
    // -------------------------------------------------------------------------
    void poll_bls();  // pulls latest belief snapshot

    // -------------------------------------------------------------------------
    // Utilities
    // -------------------------------------------------------------------------
    bool parse_plantuml(const std::string& text);
    void substitute_register_refs(json& j);

    void set_error(const std::string& msg,
                   const char* file,
                   int line,
                   const char* func);
};

#define FSM_ERROR(obj, msg) \
    (obj).set_error((msg), __FILE__, __LINE__, __func__)

// Fsm.cpp
#include "Fsm.hpp"

#include <sstream>
#include <cctype>

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------
static void trim(std::string& s)
{
    while (!s.empty() && std::isspace((unsigned char)s.front()))
        s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
}

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------
Fsm::Fsm(int sba)
    : mpp::Component<Fsm>(sba)
{
    regs_.sba_ = sba;
}

// -----------------------------------------------------------------------------
// Control Plane (PUT / GET)
// -----------------------------------------------------------------------------
void Fsm::apply_snapshot(const json& j)
{
    // ---- TICK ----
    if (j.value("tick", false)) {
        if (regs_.run_) {
            on_tick();
        }
        return;
    }
    
    if (!j.contains("verb"))
        return;

    const std::string verb = j["verb"];

    if (verb == "GET") {
        json r;
        r["component"]        = "FSM";
        r["sba"]              = regs_.sba_;
        r["target_sba"]       = regs_.target_sba_;
        r["tck_sba"]          = regs_.tck_sba_;
        r["run"]              = regs_.run_;
        r["loaded"]           = regs_.loaded_;
        r["current_state"]    = regs_.current_state_;
        r["next_state"]       = regs_.next_state_;
        r["transition_fired"] = regs_.transition_fired_;
        r["last_error"]       = regs_.last_error_;
        reply_json(r);
        return;
    }

    if (verb == "PUT" && j.value("resource","") == "fsm") {
        const auto& body = j["body"];

        if (body.contains("target_sba"))
            regs_.target_sba_ = body["target_sba"].get<int>();

        if (body.contains("tck_sba"))
            regs_.tck_sba_ = body["tck_sba"].get<int>();

        if (body.contains("fsm_text")) {
            fsm_text_ = body["fsm_text"].get<std::string>();
            regs_.loaded_ = parse_plantuml(fsm_text_);
            if (regs_.loaded_ && !state_order_.empty()) {
                regs_.current_state_ = state_order_.front();
                regs_.run_ = true;
            }
        }
        return;
    }

    if (verb == "POST") {
        const std::string action = j.value("action","");
        if (action == "run")  regs_.run_ = true;
        if (action == "stop") regs_.run_ = false;
    }
}

// -----------------------------------------------------------------------------
// Time Plane
// -----------------------------------------------------------------------------
void Fsm::on_tick()
{
    poll_bls();
    step();
}

// -----------------------------------------------------------------------------
// FSM Core
// -----------------------------------------------------------------------------
void Fsm::step()
{
    regs_.transition_fired_ = false;
    regs_.next_state_.clear();

    auto it = transitions_.find(regs_.current_state_);
    if (it == transitions_.end())
        return;

    for (const auto& t : it->second) {
        if (!evaluate_transition(t))
            continue;

        regs_.next_state_ = t.to;
        regs_.current_state_ = t.to;
        regs_.transition_fired_ = true;
        regs_.last_error_.clear();

        // State belief
        commit(("FSM.state." + t.to).c_str(), true);

        auto note_it = state_notes_.find(t.to);
        if (note_it != state_notes_.end()) {
            regs_.last_applied_state_ = t.to;
            apply_state_note(note_it->second);
        }

        return; // exactly one transition per tick
    }
}

// -----------------------------------------------------------------------------
// Guard Evaluation
// -----------------------------------------------------------------------------
bool Fsm::evaluate_transition(const Transition& t)
{
    for (const auto& subject : t.beliefs) {
        auto it = observed_beliefs_.find(subject);
        if (it == observed_beliefs_.end())
            return false;
        if (!it->second)
            return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Intent Routing
// -----------------------------------------------------------------------------
void Fsm::apply_state_note(const json& note)
{
    if (note.contains("_commit"))
        route_commit(note["_commit"]);

    if (note.contains("_send"))
        route_send(note["_send"]);

    if (note.contains("_tck"))
        route_tck(note["_tck"]);
}

void Fsm::route_commit(const json& c)
{
    std::string subject = c.value("subject","");
    if (subject.empty())
        return;

    commit(subject.c_str(),
           c.value("polarity", true),
           c.value("context", json::object()));
}

void Fsm::route_send(json payload)
{
    if (regs_.target_sba_ == 0)
        return;

    substitute_register_refs(payload);
    send_json(payload, regs_.target_sba_);
}

void Fsm::route_tck(const json& t)
{
    if (regs_.tck_sba_ == 0)
        return;

    send_json(t, regs_.tck_sba_);
}

// -----------------------------------------------------------------------------
// BLS (Read-only)
// -----------------------------------------------------------------------------
void Fsm::poll_bls()
{
    json req;
    req["verb"] = "GET";
    req["resource"] = "beliefs";

    send_json(req, bls_sba_);
}

void Fsm::on_message(const json& j)
{
    if (!j.is_object())
        return;

    if (j.contains("beliefs")) {
        observed_beliefs_.clear();
        for (auto it = j["beliefs"].begin();
             it != j["beliefs"].end(); ++it)
        {
            observed_beliefs_[it.key()] = it.value().get<bool>();
        }
        return;
    }
}

// -----------------------------------------------------------------------------
// PlantUML Parser
// -----------------------------------------------------------------------------
bool Fsm::parse_plantuml(const std::string& text)
{
    transitions_.clear();
    state_notes_.clear();
    state_order_.clear();

    std::istringstream iss(text);
    std::string line;
    bool any = false;

    while (std::getline(iss, line)) {
        auto arrow = line.find("-->");
        if (arrow != std::string::npos) {
            std::string from = line.substr(0, arrow);
            std::string rest = line.substr(arrow + 3);

            auto colon = rest.find(':');
            std::string to = (colon == std::string::npos)
                           ? rest
                           : rest.substr(0, colon);

            trim(from);
            trim(to);

            Transition t;
            t.from = from;
            t.to   = to;
            t.guards = json::object();

            if (colon != std::string::npos) {
                std::string conds = rest.substr(colon + 1);
                trim(conds);

                if (conds.rfind("belief ", 0) == 0) {
                    std::string subj = conds.substr(7);
                    trim(subj);
                    t.beliefs.push_back(subj);
                }
            }

            transitions_[from].push_back(t);
            continue;
        }

        if (line.rfind("note right of ", 0) == 0) {
            std::string state = line.substr(14);
            trim(state);

            if (!state_notes_.count(state))
                state_order_.push_back(state);

            std::string body;
            while (std::getline(iss, line)) {
                if (line.find("end note") != std::string::npos)
                    break;
                body += line + "\n";
            }

            try {
                state_notes_[state] = json::parse(body);
                any = true;
            } catch (...) {
                state_notes_[state] = json{{"_raw", body}};
            }
        }
    }

    return any;
}

// -----------------------------------------------------------------------------
// Substitution
// -----------------------------------------------------------------------------
void Fsm::substitute_register_refs(json& j)
{
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it->is_string())
            continue;

        const std::string s = it->get<std::string>();
        if (s.rfind("$REG.", 0) != 0)
            continue;

        it.value() = nullptr; // placeholder until register polling exists
    }
}

// -----------------------------------------------------------------------------
// Errors
// -----------------------------------------------------------------------------
void Fsm::set_error(const std::string& msg,
                    const char* file,
                    int line,
                    const char* func)
{
    std::ostringstream oss;
    oss << msg << " | " << file << ":" << line << " in " << func;
    regs_.last_error_ = oss.str();
}

// ===== net =====
// Net.hpp
#pragma once

#include "Component.hpp"

#include <string>
#include <netinet/in.h>

#include <pcap/pcap.h>
#include <libnet.h>

using json = nlohmann::ordered_json;

// -----------------------------------------------------------------------------
// NET Registers (Canonical MPP Form)
// -----------------------------------------------------------------------------
struct NetRegisters
{
    // Identity
    int sba = 0;

    // Lifecycle
    bool libnet_create  = false;
    bool libnet_destroy = false;
    bool pcap_create    = false;
    bool pcap_destroy   = false;

    // TX / RX triggers
    bool tx_fire = false;
    bool rx_fire = false;

    // Devices
    std::string libnet_device = "eno1";
    std::string pcap_device   = "eno1";

    // PCAP configuration
    int snaplen = 65535;
    bool promisc = true;
    int timeout_ms = 10;
    std::string pcap_filter;
    bool pcap_set_filter = false;

    // Ethernet (libnet)
    bool eth_enabled = false;
    std::string eth_src_mac;
    std::string eth_dst_mac;
    uint16_t eth_type = ETHERTYPE_IP;

    // IPv4 (libnet)
    bool ip4_enabled = false;
    std::string ip4_src;
    std::string ip4_dst;
    uint8_t ip4_ttl = 64;

    // ICMPv4 (libnet)
    bool icmp4_enabled = false;
    uint8_t  icmp4_type = ICMP_ECHO;
    uint8_t  icmp4_code = 0;
    uint16_t icmp4_id   = 0x1234;
    uint16_t icmp4_seq  = 0;
    std::string icmp4_payload;

    // RX status (published)
    bool rx_done = false;
    uint32_t rx_len = 0;
    uint32_t rx_caplen = 0;

    // TX status (optional but recommended)
    bool tx_done = false;

    // Errors
    std::string last_error;
};

// -----------------------------------------------------------------------------
// NET Component
// -----------------------------------------------------------------------------
class Net : public mpp::Component<Net>
{
public:
    explicit Net(int sba);
    ~Net();

    // MPP interface
    json serialize_registers() const;
    void apply_snapshot(const json& j);
    void on_message(const json& j);
    void on_parse_error(const json::parse_error& e);
    void on_unknown_parse_error();

protected:
    const char* component_name() const override { return "NET"; }

private:
    // Handles
    libnet_t* libnet_ = nullptr;
    pcap_t*   pcap_   = nullptr;

    NetRegisters regs_;

    // Lifecycle helpers
    void do_libnet_create();
    void do_libnet_destroy();
    void do_pcap_create();
    void do_pcap_destroy();

    // Data plane
    void do_tx();
    void do_rx();

    // Errors
    void set_error(const std::string& msg);
};

#define NET_ERROR(obj, msg) \
    (obj).set_last_error((msg), __FILE__, __LINE__, __func__)

static bool parse_mac(const std::string& s, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(s.c_str(),
               "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2],
               &b[3], &b[4], &b[5]) != 6)
        return false;

    for (int i = 0; i < 6; ++i)
        mac[i] = static_cast<uint8_t>(b[i]);

    return true;
}

static std::string mac_to_string(const uint8_t mac[6])
{
    char buf[32];
    snprintf(buf, sizeof(buf),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    return buf;
}

// Net.cpp
#include "Net.hpp"

#include <cstring>
#include <arpa/inet.h>

// -----------------------------------------------------------------------------
// Construction / Destruction
// -----------------------------------------------------------------------------
Net::Net(int sba)
    : mpp::Component<Net>(sba, /*publish_period_ms=*/0, /*listen_bus=*/true)
{
    regs_.sba = sba;
}

Net::~Net()
{
    do_pcap_destroy();
    do_libnet_destroy();
}

// -----------------------------------------------------------------------------
// Serialization (READ)
// -----------------------------------------------------------------------------
json Net::serialize_registers() const
{
    json j;
    j["component"] = "NET";
    j["sba"]       = regs_.sba;

    // Devices
    j["libnet_device"] = regs_.libnet_device;
    j["pcap_device"]   = regs_.pcap_device;

    // Live status
    j["libnet_live"] = (libnet_ != nullptr);
    j["pcap_live"]   = (pcap_ != nullptr);

    // RX / TX status
    j["tx_done"]   = regs_.tx_done;
    j["rx_done"]   = regs_.rx_done;
    j["rx_len"]    = regs_.rx_len;
    j["rx_caplen"] = regs_.rx_caplen;

    // Errors
    j["last_error"] = regs_.last_error;

    return j;
}

// -----------------------------------------------------------------------------
// Apply Snapshot (WRITE)
// -----------------------------------------------------------------------------
void Net::apply_snapshot(const json& j)

{
    // --------------------
    // Configuration
    // --------------------
    if (j.contains("libnet_device")) regs_.libnet_device = j["libnet_device"];
    if (j.contains("pcap_device"))   regs_.pcap_device   = j["pcap_device"];

    if (j.contains("snaplen"))       regs_.snaplen       = j["snaplen"];
    if (j.contains("promisc"))       regs_.promisc       = j["promisc"];
    if (j.contains("timeout_ms"))    regs_.timeout_ms    = j["timeout_ms"];
    if (j.contains("pcap_filter"))   regs_.pcap_filter   = j["pcap_filter"];

    if (j.contains("eth_enabled"))   regs_.eth_enabled   = j["eth_enabled"];
    if (j.contains("eth_src_mac"))   regs_.eth_src_mac   = j["eth_src_mac"];
    if (j.contains("eth_dst_mac"))   regs_.eth_dst_mac   = j["eth_dst_mac"];
    if (j.contains("eth_type"))      regs_.eth_type      = j["eth_type"];

    if (j.contains("ip4_enabled"))   regs_.ip4_enabled   = j["ip4_enabled"];
    if (j.contains("ip4_src"))       regs_.ip4_src       = j["ip4_src"];
    if (j.contains("ip4_dst"))       regs_.ip4_dst       = j["ip4_dst"];
    if (j.contains("ip4_ttl"))       regs_.ip4_ttl       = j["ip4_ttl"];

    if (j.contains("icmp4_enabled")) regs_.icmp4_enabled = j["icmp4_enabled"];
    if (j.contains("icmp4_type"))    regs_.icmp4_type    = j["icmp4_type"];
    if (j.contains("icmp4_code"))    regs_.icmp4_code    = j["icmp4_code"];
    if (j.contains("icmp4_id"))      regs_.icmp4_id      = j["icmp4_id"];
    if (j.contains("icmp4_seq"))     regs_.icmp4_seq     = j["icmp4_seq"];
    if (j.contains("icmp4_payload")) regs_.icmp4_payload = j["icmp4_payload"];

    // --------------------
    // Lifecycle
    // --------------------
    if (j.value("libnet_create", false))  do_libnet_create();
    if (j.value("libnet_destroy", false)) do_libnet_destroy();
    if (j.value("pcap_create", false))    do_pcap_create();
    if (j.value("pcap_destroy", false))   do_pcap_destroy();

    // --------------------
    // PCAP filter
    // --------------------
    if (j.value("pcap_set_filter", false) && pcap_) {
        if (!regs_.pcap_filter.empty()) {
            struct bpf_program fp{};
            if (pcap_compile(
                    pcap_, &fp,
                    regs_.pcap_filter.c_str(),
                    1, PCAP_NETMASK_UNKNOWN) == 0) {
                pcap_setfilter(pcap_, &fp);
                pcap_freecode(&fp);
            }
        }
    }

    if (j.value("read", false)) {
        reply_json(serialize_registers());
    }

    // --------------------
    // Actions
    // --------------------
    if (j.value("tx_fire", false)) {
        regs_.tx_done = false;
        do_tx();
    }

    if (j.value("rx_fire", false)) {
        regs_.rx_done = false;
        do_rx();
    }
    if (j.value("tick", false)) {
        do_rx();
    }
}

// -----------------------------------------------------------------------------
// BUS message handling
// -----------------------------------------------------------------------------
void Net::on_message(const json& j)
{

}

// -----------------------------------------------------------------------------
// libnet lifecycle
// -----------------------------------------------------------------------------
void Net::do_libnet_create()
{
    if (libnet_)
        return;

    char errbuf[LIBNET_ERRBUF_SIZE]{};

    libnet_ = libnet_init(
        LIBNET_LINK,
        regs_.libnet_device.c_str(),
        errbuf
    );

    if (!libnet_)
        set_error(errbuf);
}

void Net::do_libnet_destroy()
{
    if (!libnet_)
        return;

    libnet_destroy(libnet_);
    libnet_ = nullptr;
}

// -----------------------------------------------------------------------------
// pcap lifecycle
// -----------------------------------------------------------------------------
void Net::do_pcap_create()
{
    if (pcap_)
        return;

    char errbuf[PCAP_ERRBUF_SIZE]{};

    pcap_ = pcap_open_live(
        regs_.pcap_device.c_str(),
        regs_.snaplen,
        regs_.promisc,
        regs_.timeout_ms,
        errbuf
    );

    if (!pcap_) {
        set_error(errbuf);
        return;
    }
}

void Net::do_pcap_destroy()
{
    if (!pcap_)
        return;

    pcap_close(pcap_);
    pcap_ = nullptr;
}

// -----------------------------------------------------------------------------
// TX (ICMP Echo)
// -----------------------------------------------------------------------------
void Net::do_tx()
{
    if (!libnet_)
        return;

    if (!regs_.eth_enabled || !regs_.ip4_enabled || !regs_.icmp4_enabled)
        return;

    uint8_t eth_src[6], eth_dst[6];
    if (!parse_mac(regs_.eth_src_mac, eth_src) ||
        !parse_mac(regs_.eth_dst_mac, eth_dst)) {
        set_error("invalid MAC address format");
        return;
    }

    uint32_t src_ip =
        libnet_name2addr4(libnet_, (char*)regs_.ip4_src.c_str(), LIBNET_DONT_RESOLVE);
    uint32_t dst_ip =
        libnet_name2addr4(libnet_, (char *)regs_.ip4_dst.c_str(), LIBNET_DONT_RESOLVE);

    if (src_ip == 0 || dst_ip == 0) {
        set_error("invalid IP address");
        return;
    }

    const uint8_t* payload =
        reinterpret_cast<const uint8_t*>(regs_.icmp4_payload.data());
    uint32_t payload_len = regs_.icmp4_payload.size();

    libnet_build_icmpv4_echo(
        regs_.icmp4_type,
        regs_.icmp4_code,
        0,
        regs_.icmp4_id,
        regs_.icmp4_seq++,
        payload,
        payload_len,
        libnet_,
        0
    );

    libnet_build_ipv4(
        LIBNET_IPV4_H + LIBNET_ICMPV4_ECHO_H + payload_len,
        0,
        libnet_get_prand(LIBNET_PRu16),
        0,
        regs_.ip4_ttl,
        IPPROTO_ICMP,
        0,
        src_ip,
        dst_ip,
        nullptr,
        0,
        libnet_,
        0
    );

    libnet_build_ethernet(
        eth_dst,
        eth_src,
        regs_.eth_type,
        nullptr,
        0,
        libnet_,
        0
    );

    if (libnet_write(libnet_) < 0) {
        set_error(libnet_geterror(libnet_));
        return;
    }

    libnet_clear_packet(libnet_);
    regs_.tx_done = true;

    commit("NET.tx_done", true);
}

// -----------------------------------------------------------------------------
// RX (PCAP sample)
// -----------------------------------------------------------------------------
void Net::do_rx()
{
    if (!pcap_)
        return;

    struct pcap_pkthdr* hdr = nullptr;
    const u_char* data = nullptr;

    int rc = pcap_next_ex(pcap_, &hdr, &data);
    if (rc <= 0)
        return;

    regs_.rx_done   = true;
    regs_.rx_len    = hdr->len;
    regs_.rx_caplen = hdr->caplen;

    commit("NET.rx_done", true, {
        {"rx_len", regs_.rx_len},
        {"rx_caplen", regs_.rx_caplen}
    });

    json j;
    j["component"] = "NET";
    j["rx_done"]   = true;
    j["rx_len"]    = regs_.rx_len;
    j["rx_caplen"] = regs_.rx_caplen;

}

// -----------------------------------------------------------------------------
// Errors
// -----------------------------------------------------------------------------
void Net::on_parse_error(const json::parse_error& e)
{
    set_error(e.what());
}

void Net::on_unknown_parse_error()
{
    set_error("unknown JSON parse error");
}

void Net::set_error(const std::string& msg)
{
    regs_.last_error = msg;
}

// ===== xfr =====
// Xfr.hpp
#pragma once

#include "Belief.hpp"
#include "Component.hpp"

#include <map>
#include <vector>
#include <string>
#include <netinet/in.h>

using json = nlohmann::ordered_json;

struct XfrRegisters
{
  std::string component = "XFR";
  int sba = 4004;

  // identity / intent
  std::string mode = "idle";            // idle | send | recv
  std::string file_path = "";
  uint64_t file_size = 0;
  std::string peer_id = "";
  
  // chunking
  int chunk_size = 512;
  uint64_t offset = 0;
  uint32_t chunk_index = 0;
  bool eof = false;
  std::string chunk_payload = "";

  // progress
  bool send_done = false;
  bool recv_done =false;

  // control
  bool advance = false;

  // errors
  std::string last_error = "";
};

class Xfr : public mpp::Component<Xfr>
{
public:
    explicit Xfr(int sba);

    // MPP interface
    const char* component_name() const override { return "XFR"; }
    json serialize_registers() const;
    void apply_snapshot(const mpp::json& j);
    void legacy_apply_snapshot(const mpp::json& j);
    void on_message(const mpp::json& j);
    void publish_snapshot() {}
    void Xfr::on_tick();

private:
    XfrRegisters    regs_;

};

// Xfr.cpp
#include "Xfr.hpp"

using json = nlohmann::ordered_json;

Xfr::Xfr(int sba)
    : mpp::Component<Xfr>(sba)
{
    regs_.sba = sba;
}

json Xfr::serialize_registers() const
{
    json j;

    j["component"] = regs_.component;
    j["sba"]       = regs_.sba;

    j["mode"]      = regs_.mode;
    j["file_path"] = regs_.file_path;
    j["peer_id"]   = regs_.peer_id;

    j["chunk_size"]    = regs_.chunk_size;

    j["chunk_payload"]  = regs_.chunk_payload;

    j["send_done"] = regs_.send_done;
    j["recv_done"] = regs_.recv_done;

    j["advance"]   = regs_.advance;

    j["last_error"] = regs_.last_error;

    return j;
}

void Xfr::apply_snapshot(const json& j)
{
    if (j.value("tick", false)) {
        on_tick();
        return;
    }
    // --- Intent-aware path ---
    if (j.contains("verb")) {
        const std::string verb = j["verb"];

        if (verb == "GET") {
            reply_json(serialize_registers());
            return;
        }
        if (verb == "PUT") {
            if (j.value("resource","") == "xfr" && j.contains("body")) {
                const auto& body = j["body"];

            }
            return;
        }
    }
    // --- Legacy path (unchanged) ---
    legacy_apply_snapshot(j);
}

void Xfr::legacy_apply_snapshot(const json& j)
{
    if (j.contains("mode")) regs_.mode = j["mode"];
    if (j.value("advance", false)) regs_.advance = true;
    if (j.contains("mode") && j["mode"] == "send") {
        regs_.offset = 0;
        regs_.chunk_index = 0;
        regs_.eof = false;
        regs_.send_done = false;
    }
}

void Xfr::on_message(const json&)
{
}

void Xfr::on_tick()
{
    if (!regs_.advance)
        return;

    regs_.offset += regs_.chunk_size;
    regs_.chunk_index++;

    if (regs_.offset >= regs_.file_size)
        regs_.eof = true;

    regs_.chunk_payload =
        "CHUNK#" + std::to_string(regs_.chunk_index);

    regs_.advance = false;

    if (regs_.eof) {
        commit("XFR.eof", true);
    } else {
        commit("XFR.chunk_ready", true);
    }
}

// ===== bls =====
// Bls.hpp
#pragma once

#include "Component.hpp"

#include <unordered_map>
#include <string>

using json = nlohmann::ordered_json;

class Bls : public mpp::Component<Bls>
{
public:
    explicit Bls(int sba);

    void apply_snapshot(const json& j);
    void on_message(const json&) {}   // BLS is pull-only
    void on_parse_error(const json::parse_error& e);
    void on_unknown_parse_error();

protected:
    const char* component_name() const override { return "BLS"; }

private:
    std::unordered_map<std::string, bool> beliefs_;
    bool debug_ = true;

    void handle_commit(const json& belief);
    void handle_get();
};

// Bls.cpp
#include "Bls.hpp"

#include <iostream>

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------
Bls::Bls(int sba)
    : mpp::Component<Bls>(sba)
{
}

// -----------------------------------------------------------------------------
// Snapshot / Message Handling
// -----------------------------------------------------------------------------
void Bls::apply_snapshot(const json& j)
{
    if (!j.is_object())
        return;

    // ---- BELIEF COMMIT ----
    if (j.contains("belief")) {
        handle_commit(j["belief"]);
        return;
    }

    // ---- READ SNAPSHOT ----
    if (j.contains("verb") &&
        j["verb"] == "GET" &&
        j.value("resource", "") == "beliefs")
    {
        handle_get();
        return;
    }
}

void Bls::handle_commit(const json& belief)
{
    if (!belief.contains("subject") || !belief.contains("polarity"))
        return;

    const std::string subject = belief["subject"];
    const bool polarity = belief["polarity"];

    beliefs_[subject] = polarity;

    if (debug_) {
        std::cerr << "[BLS] commit " << subject
                  << " = " << polarity << std::endl;
    }
}

void Bls::handle_get()
{
    json reply;
    reply["beliefs"] = json::object();

    for (const auto& [subject, polarity] : beliefs_) {
        reply["beliefs"][subject] = polarity;
    }

    reply_json(reply);
}

// -----------------------------------------------------------------------------
// Errors
// -----------------------------------------------------------------------------
void Bls::on_parse_error(const json::parse_error& e)
{
    std::cerr << "[BLS parse error] " << e.what() << std::endl;
}

void Bls::on_unknown_parse_error()
{
    std::cerr << "[BLS parse error] unknown error" << std::endl;
}

// ===== fsm =====
// fsm-xfr-send.puml
@startuml

state XFR_IDLE
state XFR_INIT
state XFR_OPEN
state XFR_SEND
state XFR_WAIT_TX
state XFR_DONE

XFR_IDLE --> XFR_INIT : true
XFR_INIT --> XFR_OPEN : belief FSM.XFR.start
XFR_OPEN --> XFR_SEND : belief FSM.NET.ready

XFR_SEND --> XFR_WAIT_TX : true
XFR_WAIT_TX --> XFR_DONE : belief NET.tx_done

note right of XFR_IDLE
{
}
end note

note right of XFR_INIT
{
  "_send": {
    "mode": "send"
  },
  "_commit": { "subject": "FSM.XFR.init" },
  "_tck": { "enable": true }
}
end note

note right of XFR_OPEN
{
  "_commit": { "subject": "FSM.XFR.open" }
}
end note

note right of XFR_SEND
{
  "_commit": { "subject": "FSM.XFR.send_chunk" }
}
end note

note right of XFR_WAIT_TX
{
}
end note

note right of XFR_DONE
{
  "_commit": { "subject": "FSM.XFR.complete" }
}
end note

@enduml

// fsm-net-tx.puml
@startuml

state NET_IDLE
state NET_INIT
state NET_CONFIGURE
state NET_READY
state NET_TX
state NET_WAIT_TX
state NET_WAIT_RX
state NET_DONE

NET_IDLE --> NET_INIT : true
NET_INIT --> NET_CONFIGURE : true
NET_CONFIGURE --> NET_READY : true

NET_READY --> NET_TX : belief FSM.XFR.send_chunk
NET_TX --> NET_WAIT_TX : true
NET_WAIT_TX --> NET_WAIT_RX : belief NET.tx_done
NET_WAIT_RX --> NET_DONE : belief NET.rx_done
NET_DONE

note right of NET_IDLE
{
}

note right of NET_INIT
{
  "_send": {
  "libnet_create": true,
  "libnet_destroy": false,

  "eth_enabled": true,
  "ip4_enabled": true,
  "icmp4_enabled": true,

  "tx_fire": false,
  "rx_fire": false
  }
}
end note

note right of NET_CONFIGURE
{
  "_send": {
  "libnet_create": true,
  "libnet_device": "eno1",

  "eth_enabled": true,
  "eth_src_mac": "ec:b1:d7:52:8c:52",
  "eth_dst_mac": "bc:e9:2f:80:3b:56",
  "eth_type": 2048,

  "ip4_enabled": true,
  "ip4_src": "192.168.0.105",
  "ip4_dst": "192.168.0.109",
  "ip4_ttl": 64,

  "icmp4_enabled": true,
  "icmp4_type": 8,
  "icmp4_code": 0,
  "icmp4_id": 4242,
  "icmp4_seq": 1,
  "icmp4_payload": "XFR-105-to-109-test-0001",

  "pcap_create": true,
  "pcap_device": "eno1",
  "pcap_filter": "icmp and (host 192.168.0.105 or host 192.168.0.109)",
  "pcap_set_filter": true
  }
}
end note

note right of NET_READY
{
  "_commit": { "subject": "FSM.NET.ready" }
}
end note

note right of NET_TX
{
  "_send": {
  "tx_fire": true,
  "rx_fire": false
  }
}
end note

note right of NET_WAIT_TX
{
  "_send": {
    "net_rx_enable": false,
    "tx_fire": false,
    "rx_fire": false
  }
}

end note

note right of NET_WAIT_RX
{
  "_send": {
  "net_rx_enable": true,
  "tx_fire": false,
  "rx_fire": true
  }
}
end note

note right of NET_DONE
{
  "_send": {
  "net_rx_enable": false,
  "tx_fire": false,
  "rx_fire": false
  }
  "_commit": { "subject": "FSM.NET.chunk_sent" }
}
end note

@enduml

// fsm-xfr-recv.puml
@startuml

state XFR_IDLE
state XFR_INIT
state XFR_OPEN
state XFR_WAIT_RX
state XFR_DONE

XFR_IDLE --> XFR_INIT : true
XFR_INIT --> XFR_OPEN : belief FSM.XFR.start
XFR_OPEN --> XFR_WAIT_RX : belief FSM.NET.ready

XFR_WAIT_RX --> XFR_DONE : belief NET.rx_done

note right of XFR_INIT
{
  "mode": "recv",
  "_commit": { "subject": "FSM.XFR.init" }
}
end note

note right of XFR_OPEN
{
  "_commit": { "subject": "FSM.XFR.open" }
}
end note

note right of XFR_WAIT_RX
{
  "_tck": { "enable": true }
}
end note

note right of XFR_DONE
{
  "_commit": { "subject": "FSM.XFR.complete" }
}
end note

@enduml

// fsm-net-rx.puml
@startuml

state NET_IDLE
state NET_INIT
state NET_READY
state NET_WAIT_RX
state NET_DONE

NET_IDLE --> NET_INIT : true
NET_INIT --> NET_READY : true
NET_READY --> NET_WAIT_RX : true
NET_WAIT_RX --> NET_DONE : belief NET.rx_done
NET_DONE --> NET_READY : true

note right of NET_INIT
{
  "libnet_create": true,
  "libnet_destroy": false,

  "pcap_create": true,
  "pcap_destroy": false,

  "libnet_device": "enp1s0",
  "pcap_device": "enp1s0",

  "eth_enabled": true,
  "ip4_enabled": true,
  "icmp4_enabled": true,

  "tx_fire": false,
  "rx_fire": false,

  "_commit": { "subject": "FSM.NET.ready" }
}
end note

note right of NET_WAIT_RX
{
  "rx_fire": true,
  "tx_fire": false,

  "_tck": { "enable": true }
}
end note

note right of NET_DONE
{
  "rx_fire": false,
  "tx_fire": false,

  "_commit": { "subject": "NET.rx_done" }
}
end note

@enduml

// start105.sh
#!/usr/bin/env bash
set -e

echo "=== MPP startup on 105 (XFR SEND) ==="

MPP=/usr/local/mpp

# ----------------------------------------------------------------------
# 1. Create executable variants
# ----------------------------------------------------------------------
echo "[1/5] Preparing executables..."

cp -f $MPP/fsm/fsm $MPP/fsm/fsm-net
cp -f $MPP/fsm/fsm $MPP/fsm/fsm-xfr

cp -f $MPP/tck/tck $MPP/tck/net-tck
cp -f $MPP/tck/tck $MPP/tck/fsm-net-tck
cp -f $MPP/tck/tck $MPP/tck/fsm-xfr-tck

# ----------------------------------------------------------------------
# 2. Start core services
# ----------------------------------------------------------------------
echo "[2/5] Starting core services..."

$MPP/bls/bls 4000 &
$MPP/net/net 5000 &

sleep 0.5

# ----------------------------------------------------------------------
# 3. Start FSMs, TCKs, XFR
# ----------------------------------------------------------------------
echo "[3/5] Starting FSM / TCK / XFR..."

$MPP/tck/fsm-net-tck 5001 &
$MPP/fsm/fsm-net 5002 &

$MPP/xfr/xfr 6000 &
$MPP/tck/fsm-xfr-tck 6001 &
$MPP/fsm/fsm-xfr 6002 &

sleep 1

# ----------------------------------------------------------------------
# 4. Load FSMs
# ----------------------------------------------------------------------
echo "[4/5] Loading FSM definitions..."

printf '{"verb":"PUT","resource":"fsm","body":{"fsm_text":%s,"target_sba":5000,"tck_sba":5001,"run":true}}' \
  "$(jq -Rs . < /usr/local/mpp/fsm/fsm-net-tx.puml)" \
  | nc -u -w1 127.0.0.1 5002

printf '{"verb":"PUT","resource":"fsm","body":{"fsm_text":%s,"target_sba":6000,"tck_sba":6001,"run":true}}' \
  "$(jq -Rs . < /usr/local/mpp/fsm/fsm-xfr-send.puml)" \
  | nc -u -w1 127.0.0.1 6002

sleep 0.5

# ----------------------------------------------------------------------
# 5. Enable TCKs and start flow
# ----------------------------------------------------------------------
echo "[5/5] Enabling TCKs and starting XFR..."

echo '{"enable":true,"target_sba":5002}' | nc -u -w1 127.0.0.1 5001
echo '{"enable":true,"target_sba":6002}' | nc -u -w1 127.0.0.1 6001

sleep 2

echo '{"belief":{"subject":"FSM.XFR.start","polarity":true}}' \
  | nc -u -w1 127.0.0.1 4000

echo "=== MPP 105 startup complete ==="

// start109.sh
#!/usr/bin/env bash
set -e

echo "=== MPP startup on 109 (XFR RECV) ==="

MPP=/usr/local/mpp

# ----------------------------------------------------------------------
# 1. Create executable variants
# ----------------------------------------------------------------------
echo "[1/5] Preparing executables..."

cp -f $MPP/fsm/fsm $MPP/fsm/fsm-net
cp -f $MPP/fsm/fsm $MPP/fsm/fsm-xfr

cp -f $MPP/tck/tck $MPP/tck/net-tck
cp -f $MPP/tck/tck $MPP/tck/fsm-net-tck
cp -f $MPP/tck/tck $MPP/tck/fsm-xfr-tck

# ----------------------------------------------------------------------
# 2. Start core services
# ----------------------------------------------------------------------
echo "[2/5] Starting core services..."

$MPP/bls/bls 4000 &
$MPP/net/net 5000 &

sleep 0.5

# ----------------------------------------------------------------------
# 3. Start FSMs, TCKs, XFR
# ----------------------------------------------------------------------
echo "[3/5] Starting FSM / TCK / XFR..."

$MPP/tck/fsm-net 5003 &
$MPP/tck/net-tck 5001 &
$MPP/fsm/fsm-net-tck 5002 &

$MPP/xfr/xfr 6000 &
$MPP/tck/fsm-xfr-tck 6001 &
$MPP/fsm/fsm-xfr 6002 &

sleep 1

# ----------------------------------------------------------------------
# 4. Load FSMs
# ----------------------------------------------------------------------
echo "[4/5] Loading FSM definitions..."

printf '{"verb":"PUT","resource":"fsm","body":{"fsm_text":%s,"target_sba":5000,"tck_sba":5001,"run":true}}' \
  "$(jq -Rs . < /usr/local/mpp/fsm/fsm-net.puml)" \
  | nc -u -w1 127.0.0.1 5002

printf '{"verb":"PUT","resource":"fsm","body":{"fsm_text":%s,"target_sba":6000,"tck_sba":6001,"run":true}}' \
  "$(jq -Rs . < /usr/local/mpp/fsm/xfr-recv.puml)" \
  | nc -u -w1 127.0.0.1 6002

sleep 0.5

# ----------------------------------------------------------------------
# 5. Enable TCKs
# ----------------------------------------------------------------------
echo "[5/5] Enabling TCKs..."

echo '{"enable":true,"target_sba":5000}' | nc -u -w1 127.0.0.1 5003
echo '{"enable":true,"target_sba":5002}' | nc -u -w1 127.0.0.1 5001
echo '{"enable":true,"target_sba":6002}' | nc -u -w1 127.0.0.1 6001

echo "=== MPP 109 startup complete ==="
