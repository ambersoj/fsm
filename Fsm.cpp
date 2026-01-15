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
