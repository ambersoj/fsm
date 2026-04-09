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
    int         sba_            = 0;
    int         target_sba_net_ = 0;
    int         target_sba_xfr_ = 0;
    int         tck_sba_        = 0;
    int         obs_sba_         = 0;

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

    void on_message(const json& j);

protected:
    const char* component_name() const override { return "FSM"; }

private:
    int bls_sba_ = mpp::BLS_PORT;

    struct ObservedBelief {
        bool polarity;
        json context;
    };

    std::map<std::string, ObservedBelief> observed_beliefs_;

    // -------------------------------------------------------------------------
    // FSM definition
    // -------------------------------------------------------------------------
    struct BeliefGuard {
        std::string subject;
        json        context;   // empty == subject-only
    };

    struct Transition {
        std::string from;
        std::string to;
        std::vector<BeliefGuard> belief_guards;
    };

    json belief_store_;

    void substitute(json& j);

    std::string fsm_text_;
    std::vector<std::string> state_order_;
    std::map<std::string, json> state_notes_;
    std::map<std::string, std::vector<Transition>> transitions_;
    static std::string guard_to_string(const Transition& t);
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
    void route_send_net(json payload);    // _send
    void route_send_xfr(json payload);    // _send
    void route_send_tck(json payload);    // _tck

    // -------------------------------------------------------------------------
    // Utilities
    // -------------------------------------------------------------------------
    bool parse_plantuml(const std::string& text);

    void set_error(const std::string& msg,
                   const char* file,
                   int line,
                   const char* func);

    // -------------------------------------------------------------------------
    // Register snapshot (read-only for guards)
    // -------------------------------------------------------------------------
    json register_snapshot_;
};

#define FSM_ERROR(obj, msg) \
    (obj).set_error((msg), __FILE__, __LINE__, __func__)
