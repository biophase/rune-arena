#include "gameplay/spell_runtime_registry.h"

#include "spells/fire_bolt_spell.h"
#include "spells/ice_wall_spell.h"

void SpellRuntimeRegistry::Register(const std::string& spell_name, SpellRuntimeHandler handler) {
    handlers_[spell_name] = std::move(handler);
}

bool SpellRuntimeRegistry::Cast(const SpellRuntimeMatch& match, const SpellRuntimeContext& context) const {
    auto it = handlers_.find(match.spell_name);
    if (it == handlers_.end()) {
        return false;
    }
    return it->second(match, context);
}
