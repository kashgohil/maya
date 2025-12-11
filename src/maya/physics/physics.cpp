#include "maya/physics/physics.hpp"
#include <iostream>

namespace maya {

PhysicsSystem &PhysicsSystem::instance() {
  static PhysicsSystem s_instance;
  return s_instance;
}

bool PhysicsSystem::initialize() {
  std::cout << "[Physics] Initializing physics system...\n";
  std::cout << "[Physics] Physics system initialized successfully\n";
  return true;
}

void PhysicsSystem::shutdown() {
  std::cout << "[Physics] Shutting down physics system...\n";
  std::cout << "[Physics] Physics system shutdown complete\n";
}

void PhysicsSystem::step(f32 /*delta_time*/) {}

} // namespace maya
