#include "maya/ai/ai.hpp"
#include <iostream>

namespace maya {

AISystem &AISystem::instance() {
  static AISystem s_instance;
  return s_instance;
}

bool AISystem::initialize() {
  std::cout << "[AI] Initializing AI system...\n";
  std::cout << "[AI] AI system initialized successfully\n";
  return true;
}

void AISystem::shutdown() {
  std::cout << "[AI] Shutting down AI system...\n";
  std::cout << "[AI] AI system shutdown complete\n";
}

void AISystem::update(f32 /*delta_time*/) {}

} // namespace maya
