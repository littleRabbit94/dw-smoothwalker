// Reflection helpers for the UMG widgets (ui/debug_overlay.cpp, ui/marker_layer.cpp): properties and parameters found
// by name, and structs written member by member through their FProperty offsets, never a hardcoded layout (UE 5's
// FVector2D is two doubles). Game thread only.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>

namespace dw::smoothwalker::ui::umg
{
    using namespace RC::Unreal;

    constexpr size_t PARAMS = 256; // every params buffer here; checked against reflection (sized)

    // A property or parameter of `owner` (its supers included), by name without case: FNames compare that way, and
    // the stored spelling is the first one registered (the game has SetPositionInViewport's parameter as "position").
    auto find_property(UStruct* owner, const wchar_t* name) -> FProperty*;
    // A member of a struct-typed property, or null.
    auto member(FProperty* of, const wchar_t* name) -> FProperty*;
    // The offset of a pointer-sized parameter or property, or -1.
    auto pointer_at(UStruct* owner, const wchar_t* name) -> int32_t;
    auto value_in(FProperty* property, void* container) -> uint8_t*;
    // A float or a double property; false for anything else, which is left alone.
    auto write_real(FProperty* property, void* container, double value) -> bool;
    // A float or a double property's value; false (out untouched) for anything else.
    auto read_real(FProperty* property, const void* container, double& out) -> bool;
    auto is_real(FProperty* property) -> bool;
    // A bool property, native or a bitfield, through its field mask; false for anything else.
    auto write_bool(FProperty* property, void* container, bool value) -> bool;
    auto read_bool(FProperty* property, const void* container, bool& out) -> bool;
    // X and Y both float or double: anything else would leave them zero.
    auto writes_xy(FProperty* vector) -> bool;
    auto write_xy(FProperty* vector, void* container, double x, double y) -> void;
    // The X and Y of a vector property; false (x, y untouched) unless both are float or double.
    auto read_xy(FProperty* vector, const void* container, double& x, double& y) -> bool;
    // R, G, B, A of an FLinearColor property; false when any channel is not float or double.
    auto write_color(FProperty* color, void* container, const double (&rgba)[4]) -> bool;
    // Non-null, with parameters that fit a PARAMS buffer.
    auto sized(UFunction* function) -> bool;
    auto call(UObject* object, UFunction* function, uint8_t* params) -> void;
} // namespace dw::smoothwalker::ui::umg
