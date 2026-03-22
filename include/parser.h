#pragma once
#include <string>
#include <vector>
#include "structures.h"

// just function declaration that returns a vector of DecodedInstructionStruct
std::vector<DecodedInstructionStruct> parseProgram(const std::string& filename); 
