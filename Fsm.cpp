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
//                regs_.run_ = true;
            }
        }
        return;
    }
    if (verb == "PUT" && j.value("resource","") == "registers") {
        register_snapshot_ = j.value("body", json::object());
        return;
    }

    if (verb == "POST") {
        const std::string action = j.value("action","");
        if (action == "run")  regs_.run_ = true;
        if (action == "stop") regs_.run_ = false;
    }
        // ---- NET status injection ----
    if (j.value("component", "") == "NET") {

        if (j.contains("tx_done"))
            register_snapshot_["NET.tx_done"] = j["tx_done"];

        if (j.contains("rx_done"))
            register_snapshot_["NET.rx_done"] = j["rx_done"];
    }
}

// -----------------------------------------------------------------------------
// Time Plane
// -----------------------------------------------------------------------------
void Fsm::on_tick()
{
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
    for (const auto& bg : t.belief_guards) {

        auto it = register_snapshot_.find(bg.subject);
        if (it == register_snapshot_.end())
            return false;

        if (!it.value().get<bool>())
            return false;
    }

    return true;
}

bool Fsm::belief_context_matches(const json& belief_ctx,
                                 const json& guard_ctx)
{
    if (guard_ctx.empty())
        return true;

    if (!belief_ctx.is_object() || !guard_ctx.is_object())
        return false;

    for (auto it = guard_ctx.begin(); it != guard_ctx.end(); ++it) {
        const std::string& key = it.key();

        if (!belief_ctx.contains(key))
            return false;

        if (belief_ctx[key] != it.value())
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

    if (note.contains("_send_regs"))
        route_send(note["_send_regs"]);

    if (note.contains("_tck_ctl"))
        route_tck(note["_tck_ctl"]);
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

}

void Fsm::on_message(const json& j)
{
    if (!j.is_object())
        return;

    // tick
    if (j.contains("tick")) {
        on_tick();
        return;
    }

    // NET status
    if (j.value("component", "") == "NET") {

        if (j.contains("tx_done"))
            register_snapshot_["NET.tx_done"] = j["tx_done"];

        if (j.contains("rx_done"))
            register_snapshot_["NET.rx_done"] = j["rx_done"];

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
            // NEW t.guards removed

            if (colon != std::string::npos) {
                std::string conds = rest.substr(colon + 1);
                trim(conds);

                // NEW BeliefGuard Code
                if (conds.rfind("belief ", 0) == 0) {
                    std::string rest = conds.substr(7);
                    trim(rest);

                    BeliefGuard bg;

                    auto brace = rest.find('{');
                    if (brace == std::string::npos) {
                        bg.subject = rest;
                        bg.context = json::object();
                    } else {
                        bg.subject = rest.substr(0, brace);
                        trim(bg.subject);

                        std::string ctx = rest.substr(brace);
                        try {
                            bg.context = json::parse(ctx);
                        } catch (const std::exception& e) {
                            set_error("Invalid guard context JSON", __FILE__, __LINE__, __func__);
                            return false;
                        }
                    }
                    t.belief_guards.push_back(bg);
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
