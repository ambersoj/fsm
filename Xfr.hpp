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
  int sba = 4005;

  // identity / intent
  std::string mode = "idle";            // idle | send | recv
  std::string file_path = "";
  std::string peer_id = "";

  // chunking
  int chunk_size = 512;
  int total_chunks = 0;
  int current_chunk = 0;

  // data exchange
  std::string chunk_payload = "";
  bool chunk_ready = false;
  bool chunk_accepted = false;

  // progress
  bool send_done = false;
  bool recv_done =false;

  // control
  bool advance = false;

  // errors
  std::string last_error ="";
};

class Xfr : public mpp::Component<Xfr>
{
public:
    explicit Xfr(int sba);

    // MPP interface
    const char* component_name() const override { return "XFR"; }
    json serialize_registers() const;
    void apply_snapshot(const mpp::json& j);
    void on_message(const mpp::json& j);
    void publish_snapshot() {}


private:
    XfrRegisters    regs_;

};
