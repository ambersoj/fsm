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
