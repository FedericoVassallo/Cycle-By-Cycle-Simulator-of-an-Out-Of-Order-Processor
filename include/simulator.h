#pragma once
#include "structures.h"
#include <vector>

// Runs one cycle of propagation (all stages)
void propagate(ProcessorStateStruct& state,
               const std::vector<DecodedInstructionStruct>& program);

// Copies next state into current state
void latch(ProcessorStateStruct& state);