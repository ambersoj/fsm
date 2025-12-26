#pragma once

#include "Component.hpp"

#include <string>
#include <map>
#include <vector>

using json = nlohmann::ordered_json;

static constexpr int NET_SBA = 4002;

// -----------------------------------------------------------------------------
// FSM Registers
// -----------------------------------------------------------------------------
struct FsmRegisters
{
    int         sba_ = 0;

    bool        run_ = false;
    bool        load_ = false;
    bool        loaded_ = false;

    std::string current_state_;
    std::string next_state_;
    bool        transition_fired_ = false;

    std::string last_applied_state_;
    std::string last_error_;
};

// -----------------------------------------------------------------------------
// FSM Component (BUS-driven)
// -----------------------------------------------------------------------------
class Fsm : public mpp::Component<Fsm>
{
public:
    explicit Fsm(int sba);

    json serialize_registers() const;
    void apply_snapshot(const json& j);

    void on_message(const json& j);
    void on_parse_error(const json::parse_error& e);
    void on_unknown_parse_error();

private:
    struct Transition {
        std::string from;
        std::string to;
        json        guards;
    };

    // FSM definition
    std::string fsm_text_;
    std::map<std::string, json> state_notes_;
    std::vector<std::string> state_order_;
    std::map<std::string, std::vector<Transition>> transitions_;

    // Runtime
    FsmRegisters regs_;

    // Observed BUS state
    std::map<std::string, json> observed_;

    // Helpers
    bool parse_plantuml_notes(const std::string& text);
    bool evaluate_transition(const Transition& t);
    void step();

    void set_last_error(const std::string& msg,
                        const char* file,
                        int line,
                        const char* func);
};

#define FSM_ERROR(obj, msg) \
    (obj).set_last_error((msg), __FILE__, __LINE__, __func__)
