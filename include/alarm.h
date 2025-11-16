#pragma once

#include <string>
#include <chrono>

namespace opendial::alarm {

struct Alarm {
    const std::string uuid;
    std::chrono::system_clock::time_point time;
    std::string label;
    bool enabled{true};
    std::uint64_t version{0}; 
    std::string device_id;

    Alarm();

};

} // namespace opendial::alarm