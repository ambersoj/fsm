// Fsm.cpp
#include "Fsm.hpp"

#include <sstream>
#include <chrono>
#include <thread>
#include <cctype>

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------
static void trim(std::string& s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
}

// -----------------------------------------------------------------------------
// Construction / Destruction
// -----------------------------------------------------------------------------
Fsm::Fsm(int sba_port)
    : mpp::Component<Fsm>(sba_port, /*publish_period_ms=*/0, /*listen_bus=*/true)
{
    regs_.sba_ = sba_port;
}

Fsm::~Fsm()
{
}

// -----------------------------------------------------------------------------
// Serialization (READ)
// -----------------------------------------------------------------------------
json Fsm::serialize_registers() const
{
    json j;
    j["component"]          = "FSM";
    j["sba"]                = regs_.sba_;
    j["load"]               = regs_.load_;
    j["loaded"]             = regs_.loaded_;
    j["run"]                = regs_.run_;
    j["current_state"]      = regs_.current_state_;
    j["next_state"]         = regs_.next_state_;
    j["transition_fired"]   = regs_.transition_fired_;
    j["last_applied_state"] = regs_.last_applied_state_;
    j["last_error"]         = regs_.last_error_;
    return j;
}

// -----------------------------------------------------------------------------
// Apply Snapshot (WRITE)
// -----------------------------------------------------------------------------
void Fsm::apply_snapshot(const json& j)
{
    if (j.contains("fsm_text"))
        fsm_text_ = j["fsm_text"];

    if (j.value("load", false)) {
        regs_.load_ = true;

        transitions_.clear();
        state_notes_.clear();
        state_order_.clear();
        observed_.clear();

        regs_.loaded_ = parse_plantuml_notes(fsm_text_);

        if (!regs_.loaded_) {
            FSM_ERROR(*this, "PlantUML parse failed");
        }
        else if (!state_order_.empty()) {
            regs_.current_state_ = state_order_.front();
            regs_.next_state_.clear();
            regs_.last_error_.clear();
        }
        else {
            FSM_ERROR(*this, "FSM has no states");
        }

        regs_.load_ = false;
    }
    if (j.contains("run")) {
        regs_.run_ = j["run"];
    }
}

// -----------------------------------------------------------------------------
// Message Handling
// -----------------------------------------------------------------------------
void Fsm::on_message(const json& j)
{
    if (!j.is_object())
        return;

    // BUS observation ingestion
    if (j.contains("component")) {

        std::string src = j.value("component", "");
        observed_[src] = j;
        return;
    }

    if (j.value("step", false)) {
        on_step();
        return;
    }
    apply_snapshot(j);
}

// -----------------------------------------------------------------------------
// FSM Step
// -----------------------------------------------------------------------------
void Fsm::on_step()
{
    regs_.transition_fired_ = false;
    regs_.next_state_.clear();

    auto it = transitions_.find(regs_.current_state_);
    if (it == transitions_.end()) {
        regs_.last_error_ = "No transitions from " + regs_.current_state_;
        return;
    }

    for (const auto& t : it->second) {
        if (!evaluate_transition(t))
            continue;

        regs_.current_state_ = t.to;
        regs_.next_state_ = t.to;
        regs_.transition_fired_ = true;
        regs_.last_error_.clear();

        auto note = state_notes_.find(t.to);
        if (note != state_notes_.end()) {
            regs_.last_applied_state_ = t.to;
        }
        if (note != state_notes_.end()) {
            json cmd = note->second;
            send_json(cmd, NET_SBA);   // ← NET SBA
        }
        return;
    }

    regs_.last_error_ = "No guards satisfied for " + regs_.current_state_;
}

// -----------------------------------------------------------------------------
// Guard Evaluation
// -----------------------------------------------------------------------------
bool Fsm::evaluate_transition(const Transition& t)
{
    if (t.guards.empty())
        return true;

    for (auto it = t.guards.begin(); it != t.guards.end(); ++it) {
        const std::string& key = it.key();
        const json& expected = it.value();

        auto dot = key.find('.');
        if (dot == std::string::npos)
            return false;

        std::string comp = key.substr(0, dot);
        std::string field = key.substr(dot + 1);

        auto obs = observed_.find(comp);
        if (obs == observed_.end())
            return false;

        const json& msg = obs->second;
        if (!msg.contains("status"))
            return false;

        const json& status = msg["status"];
        if (!status.contains(field))
            return false;

        if (status[field] != expected)
            return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Error Hooks
// -----------------------------------------------------------------------------
void Fsm::on_parse_error(const json::parse_error& e)
{
    FSM_ERROR(*this, std::string("JSON parse error: ") + e.what());
}

void Fsm::on_unknown_parse_error()
{
    FSM_ERROR(*this, "unknown JSON parse error");
}

void Fsm::set_last_error(const std::string& msg,
                         const char* file,
                         int line,
                         const char* func)
{
    std::ostringstream oss;
    oss << msg << " | " << file << ":" << line << " in " << func;
    regs_.last_error_ = oss.str();
}

// -----------------------------------------------------------------------------
// PlantUML Note Parser
// -----------------------------------------------------------------------------
bool Fsm::parse_plantuml_notes(const std::string& text)
{
    bool any = false;
    std::istringstream iss(text);
    std::string line;

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
            t.to = to;
            t.guards = json::object();

            if (colon != std::string::npos) {
                std::string conds = rest.substr(colon + 1);
                std::istringstream css(conds);
                std::string kv;
                while (std::getline(css, kv, ',')) {
                    auto eq = kv.find('=');
                    if (eq == std::string::npos) continue;

                    std::string key = kv.substr(0, eq);
                    std::string val = kv.substr(eq + 1);
                    trim(key); trim(val);

                    try { t.guards[key] = json::parse(val); }
                    catch (...) {
                        if (val == "true") t.guards[key] = true;
                        else if (val == "false") t.guards[key] = false;
                        else t.guards[key] = val;
                    }
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
                if (line.find("end note") != std::string::npos) break;
                body += line + "\n";
            }

            try {
                state_notes_[state] = json::parse(body);
                any = true;
            }
            catch (...) {
                state_notes_[state] = json{{"_raw", body}};
            }
        }
    }

    return any;
}
