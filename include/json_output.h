#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "structures.h"

// Appends a snapshot of the current processor state to the log array
void dumpStateIntoLog(const ProcessorStateStruct& state, nlohmann::json& log);

// Writes the accumulated log to a JSON file
void saveLog(const nlohmann::json& log, const std::string& filename);
