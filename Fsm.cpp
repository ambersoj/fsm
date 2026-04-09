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
    if (j.value("tick", false)) {
        step();
        return;
    }
    
    if (!j.contains("verb"))
        return;

    const std::string verb = j["verb"];

    if (verb == "GET") {
        json r;
        r["component"]        = "FSM";
        r["sba"]              = regs_.sba_;
        r["target_sba_net"]   = regs_.target_sba_net_;
        r["target_sba_xfr"]   = regs_.target_sba_xfr_;
        r["tck_sba"]          = regs_.tck_sba_;
        r["obs_sba"]          = regs_.obs_sba_;
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

        if (body.contains("target_sba_net"))
            regs_.target_sba_net_ = body["target_sba_net"].get<int>();

        if (body.contains("target_sba_xfr"))
            regs_.target_sba_xfr_ = body["target_sba_xfr"].get<int>();

        if (body.contains("tck_sba"))
            regs_.tck_sba_ = body["tck_sba"].get<int>();

        if (body.contains("obs_sba"))
            regs_.obs_sba_ = body["obs_sba"].get<int>();

        if (body.contains("fsm_text")) {
            fsm_text_ = body["fsm_text"].get<std::string>();
            regs_.loaded_ = parse_plantuml(fsm_text_);
            if (regs_.loaded_ && !state_order_.empty()) {
                regs_.current_state_ = state_order_.front();
            }
        }
        return;
    }
    if (verb == "PUT" && j.value("resource","") == "registers") {
        register_snapshot_ = j.value("body", json::object());
        return;
    }

}

std::string Fsm::guard_to_string(const Transition& t)
{
    if (t.belief_guards.empty())
        return "true";

    return t.belief_guards.front().subject;
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

        // capture old state BEFORE transition
        std::string from = regs_.current_state_;
        std::string to   = t.to;

        regs_.next_state_ = to;
        regs_.current_state_ = to;
        regs_.transition_fired_ = true;
        regs_.last_error_.clear();

        // ---- TRANSITION TRACE ----
        std::cerr
            << "[FSM] "
            << from
            << " -> "
            << to
            << " via "
            << guard_to_string(t)
            << std::endl;

        // ---- STATE TRACE ----
        std::cerr
            << "[FSM STATE] "
            << regs_.current_state_
            << std::endl;

        // ---- OBS BROADCAST ----
        if (regs_.obs_sba_ > 0) {
            json j;
            j["component"] = "FSM";
            j["state"]     = regs_.current_state_;
            j["from"]      = from;
            j["to"]        = to;
            send_json(j, regs_.obs_sba_);
        }

        auto note_it = state_notes_.find(to);
        if (note_it != state_notes_.end()) {
            regs_.last_applied_state_ = to;
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
    for (const auto& g : t.belief_guards) {

        auto it = register_snapshot_.find(g.subject);
        if (it == register_snapshot_.end())
            return false;

        bool val = false;

        if (it->is_boolean())
            val = it->get<bool>();

        else if (it->is_number())
            val = it->get<int>() != 0;

        else if (it->is_string())
            val = (it->get<std::string>() == "true");

        if (!val)
            return false;
    }

    return true;
}

void Fsm::substitute(json& j)
{
    if (j.is_object()) {
        for (auto& [k,v] : j.items())
            substitute(v);
    }
    else if (j.is_array()) {
        for (auto& v : j)
            substitute(v);
    }
    else if (j.is_string()) {

        std::string s = j.get<std::string>();

        if (!s.empty() && s[0] == '$') {

            s.erase(0,1); // remove $

            // Now s is like "XFR.seq" or "XFR.buffer"

            if (register_snapshot_.contains(s)) {
                j = register_snapshot_[s];
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Intent Routing
// -----------------------------------------------------------------------------
void Fsm::apply_state_note(const json& note)
{
    if (note.contains("_commit"))
        route_commit(note["_commit"]);

    if (note.contains("_send_regs_net"))
        route_send_net(note["_send_regs_net"]);

    if (note.contains("_send_regs_xfr"))
        route_send_xfr(note["_send_regs_xfr"]);

    if (note.contains("_send_regs_tck"))
        route_send_tck(note["_send_regs_tck"]);
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

void Fsm::route_send_net(json payload)
{
    if (regs_.target_sba_net_ == 0)
        return;

    substitute(payload);

    send_json(payload, regs_.target_sba_net_);
}

void Fsm::route_send_xfr(json payload)
{
    if (regs_.target_sba_xfr_ == 0)
        return;

    substitute(payload);

    send_json(payload, regs_.target_sba_xfr_);
}

void Fsm::route_send_tck(json payload)
{
    if (regs_.tck_sba_ == 0)
        return;

    substitute(payload);

    send_json(payload, regs_.tck_sba_);
}

void Fsm::on_message(const json& j)
{
    if (!j.is_object())
        return;
    
    std::cerr
        << "[FSM RX] "
        << j.dump()
        << std::endl;

    if (j.contains("component")) {

        std::string comp = j["component"].get<std::string>();

        for (auto it = j.begin(); it != j.end(); ++it) {

            if (it.key() == "component")
                continue;

            std::string namespaced = comp + "." + it.key();
            register_snapshot_[namespaced] = it.value();
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

            // -------------------------------
            // Parse condition
            // -------------------------------
            if (colon != std::string::npos) {

                std::string conds = rest.substr(colon + 1);
                trim(conds);

                // TRUE = unconditional
                if (conds != "true" && !conds.empty()) {

                    BeliefGuard bg;
                    bg.subject = conds;
                    bg.context = json::object();

                    t.belief_guards.push_back(bg);
                }
            }

            transitions_[from].push_back(t);
            continue;
        }

        // -------------------------------
        // Parse notes
        // -------------------------------
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
            }
            catch (...) {
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
