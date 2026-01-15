// Belief.hpp
#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace mpp {

struct Belief
{
    std::string component;   // "NET"
    std::string subject;     // "NET.started"
    bool polarity;           // true / false
    nlohmann::json context;  // opaque
};

} // namespace mpp
