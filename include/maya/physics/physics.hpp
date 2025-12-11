#pragma once

#include "maya/math/types.hpp"

namespace maya {

class PhysicsSystem {
public:
  static PhysicsSystem &instance();

  bool initialize();
  void shutdown();

  void step(f32 delta_time);

private:
  PhysicsSystem() = default;
  ~PhysicsSystem() = default;

  PhysicsSystem(const PhysicsSystem &) = delete;
  PhysicsSystem &operator=(const PhysicsSystem &) = delete;
};

} // namespace maya
