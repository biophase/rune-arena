#pragma once

#include "emitters/emitter_base.h"

class SmokeEmitter : public EmitterBase {
  public:
    void UpdateAndEmit(Projectile& projectile, std::vector<Particle>& out_particles) const override;
};
