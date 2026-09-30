// Reflection helpers for the UMG widgets (ui/umg.hpp). Bodies moved from debug_overlay.cpp unchanged, plus the reads
// and the bool writes the marker layer needs.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.

#include "umg.hpp"

#include <cstring>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UClass.hpp>

namespace dw::smoothwalker::ui::umg
{
    auto find_property(UStruct* owner, const wchar_t* name) -> FProperty*
    {
        if (!owner) return nullptr;
        for (FProperty* property : TFieldRange<FProperty>(owner))
        {
            if (_wcsicmp(property->GetName().c_str(), name) == 0) return property;
        }
        return nullptr;
    }

    auto member(FProperty* of, const wchar_t* name) -> FProperty*
    {
        auto* as_struct = CastField<FStructProperty>(of);
        return as_struct ? find_property(as_struct->GetStruct().Get(), name) : nullptr;
    }

    auto pointer_at(UStruct* owner, const wchar_t* name) -> int32_t
    {
        auto* property = find_property(owner, name);
        return property && property->GetElementSize() == static_cast<int32_t>(sizeof(void*)) ? property->GetOffset_ForInternal() : -1;
    }

    auto value_in(FProperty* property, void* container) -> uint8_t*
    {
        return static_cast<uint8_t*>(container) + property->GetOffset_ForInternal();
    }

    auto write_real(FProperty* property, void* container, double value) -> bool
    {
        if (!property) return false;
        if (CastField<FDoubleProperty>(property))
        {
            memcpy(value_in(property, container), &value, sizeof(value));
            return true;
        }
        if (CastField<FFloatProperty>(property))
        {
            auto single = static_cast<float>(value);
            memcpy(value_in(property, container), &single, sizeof(single));
            return true;
        }
        return false;
    }

    auto read_real(FProperty* property, const void* container, double& out) -> bool
    {
        if (!property) return false;
        const auto* at = static_cast<const uint8_t*>(container) + property->GetOffset_ForInternal();
        if (CastField<FDoubleProperty>(property))
        {
            memcpy(&out, at, sizeof(out));
            return true;
        }
        if (CastField<FFloatProperty>(property))
        {
            float single = 0.0f;
            memcpy(&single, at, sizeof(single));
            out = single;
            return true;
        }
        return false;
    }

    auto is_real(FProperty* property) -> bool
    {
        return CastField<FDoubleProperty>(property) || CastField<FFloatProperty>(property);
    }

    auto write_bool(FProperty* property, void* container, bool value) -> bool
    {
        auto* as_bool = CastField<FBoolProperty>(property);
        if (!as_bool) return false;
        as_bool->SetPropertyValue(value_in(property, container), value);
        return true;
    }

    auto read_bool(FProperty* property, const void* container, bool& out) -> bool
    {
        auto* as_bool = CastField<FBoolProperty>(property);
        if (!as_bool) return false;
        out = as_bool->GetPropertyValue(static_cast<const uint8_t*>(container) + property->GetOffset_ForInternal());
        return true;
    }

    auto writes_xy(FProperty* vector) -> bool
    {
        return CastField<FStructProperty>(vector) && is_real(member(vector, L"X")) && is_real(member(vector, L"Y"));
    }

    auto write_xy(FProperty* vector, void* container, double x, double y) -> void
    {
        auto* at = value_in(vector, container);
        write_real(member(vector, L"X"), at, x);
        write_real(member(vector, L"Y"), at, y);
    }

    auto read_xy(FProperty* vector, const void* container, double& x, double& y) -> bool
    {
        if (!writes_xy(vector)) return false;
        const auto* at = static_cast<const uint8_t*>(container) + vector->GetOffset_ForInternal();
        double rx = 0.0, ry = 0.0;
        if (!read_real(member(vector, L"X"), at, rx) || !read_real(member(vector, L"Y"), at, ry)) return false;
        x = rx;
        y = ry;
        return true;
    }

    auto write_color(FProperty* color, void* container, const double (&rgba)[4]) -> bool
    {
        if (!color) return false;
        auto* at = value_in(color, container);
        bool ok = true;
        const wchar_t* channels[4]{L"R", L"G", L"B", L"A"};
        for (int i = 0; i < 4; ++i) ok = write_real(member(color, channels[i]), at, rgba[i]) && ok;
        return ok;
    }

    auto sized(UFunction* function) -> bool
    {
        return function && function->GetPropertiesSize() >= 0 && static_cast<size_t>(function->GetPropertiesSize()) <= PARAMS;
    }

    auto call(UObject* object, UFunction* function, uint8_t* params) -> void
    {
        object->ProcessEvent(function, params);
    }
} // namespace dw::smoothwalker::ui::umg
