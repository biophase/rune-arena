#pragma once

#include <vector>

#include "game/projectile.h"
#include "particles/particle.h"

class EmitterBase {
  public:
    virtual ~EmitterBase() = default;
    virtual void UpdateAndEmit(Projectile& projectile, std::vector<Particle>& out_particles) const = 0;
};
