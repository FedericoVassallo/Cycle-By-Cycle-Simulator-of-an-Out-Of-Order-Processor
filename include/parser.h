#pragma once
#include <string>
#include <vector>
#include "structures.h"

// parser function that returns a vector of DecodedInstructionStruct
std::vector<DecodedInstructionStruct> parseInstructions(const std::string& filename); 
