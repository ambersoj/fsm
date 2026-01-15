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
    j["total_chunks"]  = regs_.total_chunks;
    j["current_chunk"] = regs_.current_chunk;

    j["chunk_payload"]  = regs_.chunk_payload;
    j["chunk_ready"]    = regs_.chunk_ready;
    j["chunk_accepted"] = regs_.chunk_accepted;

    j["send_done"] = regs_.send_done;
    j["recv_done"] = regs_.recv_done;

    j["advance"]   = regs_.advance;

    j["last_error"] = regs_.last_error;

    return j;
}

void Xfr::apply_snapshot(const json& j)
{
    if (j.contains("mode")) regs_.mode = j["mode"];
    if (j.contains("advance")) regs_.advance = j["advance"];
}

void Xfr::on_message(const json& j)
{
}
