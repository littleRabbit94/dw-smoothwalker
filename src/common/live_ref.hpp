// A UObject pointer kept across engine ticks, checked against the object array before use. Shared by both sides
// (the core's player discovery, Smoothwalker's mode tuning and overlay); header-only, no state.
#pragma once

#include <cstdint>

#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

namespace dw
{
    // A UObject pointer kept across ticks, with the object array index it had when taken from a live object.
    // alive() reads only GUObjectArray, never the object: true while the slot still holds the same pointer and
    // is neither Unreachable (1 << 28) nor Garbage (1 << 21, UE 5.5.4 ObjectMacros.h), catching a GC even with no
    // EndPlay or LoadMap hook. UE4SS's FUObjectItem::IsPendingKill tests bit 29, which UE 5 reuses, so it is not
    // used. The class is compared too: a freed object replaced by another class at the same address and index
    // must not pass, or a cached property offset reads the wrong layout. Game thread only.
    struct LiveRef
    {
        RC::Unreal::UObject* object = nullptr;
        int32_t index = -1;
        RC::Unreal::UClass* cls = nullptr;

        static auto of(RC::Unreal::UObject* live) -> LiveRef
        {
            return live ? LiveRef{live, live->GetInternalIndex(), live->GetClassPrivate()} : LiveRef{};
        }

        auto alive() const -> bool
        {
            if (!object || index < 0) return false;
            static constexpr auto DEAD = static_cast<RC::Unreal::EInternalObjectFlags>((1 << 28) | (1 << 21));
            auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
            return item && item->GetUObject() == object && !item->HasAnyFlags(DEAD) && object->GetClassPrivate() == cls;
        }
    };
} // namespace dw
