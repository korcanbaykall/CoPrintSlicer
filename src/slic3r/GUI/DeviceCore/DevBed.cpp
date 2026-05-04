#include "DevBed.h"
#include "slic3r/GUI/DeviceManager.hpp"

#include <cstdlib>

namespace Slic3r {

namespace {

bool parse_json_float(const json& j, float& out)
{
    if (j.is_number()) {
        out = j.get<float>();
        return true;
    }
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        char*             end = nullptr;
        const float       v   = std::strtof(s.c_str(), &end);
        if (end != s.c_str()) {
            out = v;
            return true;
        }
    }
    return false;
}

} // namespace

void DevBed::ParseV1_0(const json &print_json, DevBed *system)
{
    if (print_json.contains("bed_temper")) {
        float v = 0.f;
        if (parse_json_float(print_json["bed_temper"], v))
            system->bed_temp = v;
    }
    if (print_json.contains("bed_target_temper")) {
        float v = 0.f;
        if (parse_json_float(print_json["bed_target_temper"], v))
            system->bed_temp_target = v;
    }
}

void DevBed::ParseV2_0(const json &print_json, DevBed *system)
{
    if (print_json.contains("bed_temp") && print_json["bed_temp"].is_number()) {
        int bed_temp_bits = print_json["bed_temp"].get<int>();
        system->bed_temp        = system->m_owner->get_flag_bits(bed_temp_bits, 0, 16);
        system->bed_temp_target = system->m_owner->get_flag_bits(bed_temp_bits, 16, 16);
    }
}

} // namespace Slic3r