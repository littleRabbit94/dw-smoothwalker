// Camera position tuning: writes the position settings (modes/position.hpp) into the game's camera modes: per-group
// distance, height, shoulder and FOV on every camera type, the interior adjustment on the Interior type of the modes
// that differ there, and the look limits, plus the switch for the game's own camera lag. Game thread
// only, except note_new() (any thread; it touches tuner state only when called on the game thread) and restore() at
// unload.
//
// Every write is computed from a per-mode base, never from the current value, so applies cannot compound.
// The base starts as the CDO values captured the first time a class is seen; a value another mod wrote
// replaces that field of it (adopt_foreign()). CameraOffsets is only re-read on a camera type change, so an
// apply flips the player's camera type away and back (docs/design.md, "How writes land"). Bodies are in
// mode_tuner.cpp.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later; see LICENSE.
#pragma once

#include "../../common/live_ref.hpp"
#include "position.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>

namespace dw::smoothwalker::modes
{
    using namespace RC::Unreal;

    // Whether an Aiming-, Combat-, Focus- or Traversal-group mode is blending in or active, from one scan of the
    // player's live modes, and whether the player's camera type is Interior.
    struct ModeState
    {
        bool aiming = false;
        bool combat = false;
        bool focus = false;
        bool traversal = false;
        bool interior = false;
    };

    class ModeTuner
    {
      public:
        // Shipped values and the base are kept for the session: a class that stays loaded across a map load
        // still holds the mod's values and must not have them captured as the game's.
        auto capture() -> void;

        // Without a LoadMap hook, forget() may not run before a class unloads; every use of a captured CDO or
        // class pointer is preceded by this check: capture(), scan_instances(), adopt_foreign(), for_each_live()
        // (hence for_each_instance() and set_blend()).
        auto drop_dead_classes() -> void;

        // Before a map load: classes may unload.
        auto forget() -> void;

        // Whether an Aiming-, Combat-, Focus- or Traversal-group mode is blending in or active (ECameraModeState 0 or 1;
        // 2 blending out, 3 popped). One GetState call per live instance of those four groups per engine tick,
        // skipped once a group's flag is already set. Interior: one GetCameraType call per tick, on a
        // RebelCameraComponent only; while the position flip has the camera type away, the type it left is reported
        // (tick()). A rescan request (a new camera or world, or an overflow in
        // adopt_new) is served here on the next tick, not left to an apply that may never come. Only then does it
        // capture and walk every object; every other tick only adopts the hand-off. capture() on every tick would
        // retry a class dropped by an unload each time (about 50 ms a failed lookup) and capture a reloaded one
        // without the late-class write adopt_late() gives it.
        auto mode_state(UObject* player_camera) -> ModeState;

        // Every live mode on the player's camera with its state, newest first, popped ones (3) left out: the tracked
        // modes and the other RebelCameraMode subclasses the hand-off and the scan found (m_untracked). For the debug
        // overlay's refresh, a quarter second apart, never per tick: one GetState call per listed mode, and no object
        // walk. `stale`: a rescan is pending (a new camera or world, or a hand-off overflow), served by mode_state()
        // or the next apply, so the list may be short until then.
        auto list_modes(UObject* player_camera, bool& stale) -> std::vector<ModeListing>;

        // The player's camera type (ECameraType) by its enum name when the return value's enum is reflected, else the
        // number; empty before the layout is resolved; "n/a" when the camera is not a RebelCameraComponent (the function
        // would run on the wrong class). Game thread, one GetCameraType call: the debug overlay's refresh.
        auto camera_type_name(UObject* player_camera) -> std::wstring;

        // A new player camera: its modes were made before the new-object callback could see them.
        auto camera_changed() -> void;

        // From the new-object callback, on whatever thread constructs the object, for an object whose outer is
        // the player's camera. On the game thread a captured mode is written here, before the game pushes it
        // (write_new()); anything else is only handed off, and adopt_new() sorts it on the game thread.
        auto note_new(LiveRef object, bool game_thread) -> void;

        auto apply(const PositionTuning& tuning, UObject* player_camera) -> void;

        // At unload, after the mod's callbacks are gone. The CDOs get each mode's base, so values another mod
        // wrote since the last apply stay. Live instances keep their values until the game pushes new ones.
        auto restore() -> void;

        // Once per engine tick. A stage advances only after the player's camera has updated, so a flip
        // requested under a pause (the Mod Menu) waits for the world instead of running unseen.
        // view_seconds: world time of the player's camera updates. The glide runs on world time, so the wait for
        // its end must too, or a pause or slow motion mid-glide restores the blend time while it still blends.
        auto tick(uint64_t view_updates, double view_seconds, UObject* player_camera) -> void;

      private:
        struct Original
        {
            float fov{}, pitch_min{}, pitch_max{}, type_blend{};
            bool hlag_on{}, vlag_on{};
            std::vector<OffsetValues> offsets;
        };

        struct Mode
        {
            ModeClassSpec spec;
            UObject* cdo = nullptr;
            UClass* cls = nullptr;
            LiveRef cdo_ref, cls_ref; // checked before cdo or cls is used
            bool captured = false;
            bool missing = false; // not loaded at the last lookup
            bool has_shipped = false;
            bool usable = true;
            bool keys_warned = false; // the CameraOffsets key-set warning, once per session
            Original shipped; // the first capture, never changed: for the logs and adopt_foreign()
            // What writes are computed from: shipped, with each value another mod wrote in its place. type_blend
            // and the lag bits stay as captured: the mod writes those itself (the flip, own_lag).
            Original base;
        };

        struct Offsets
        {
            int32_t fov = -1, pitch_min = -1, pitch_max = -1;
            int32_t offsets_map = -1, type_blend = -1; // type_blend: CameraTypeBlendArgs.BlendTime
            int32_t offset_target = -1, offset_fov = -1;
            int32_t offset_size = 0, offset_align = 0;
        };

        std::vector<Mode> m_modes = [] {
            std::vector<Mode> modes;
            for (auto& spec : mode_classes()) modes.push_back({spec});
            return modes;
        }();
        // The player's live modes, with their m_modes index and the order they were taken in (m_seq): the scan takes
        // what is already there, the hand-off what is pushed later, so a higher seq is a newer mode.
        // known: its values can be classified by adopt_foreign(). A scan finds modes whose values predate what this
        // tuner can tell apart (a hot reload leaves the old DLL's writes in them, a new pawn's camera an older
        // apply's), so they are not checked until an apply or adopt_late() writes them. A hand-off mode was built
        // from a CDO that is checked, so it is known from the start.
        struct LiveMode
        {
            LiveRef ref;
            size_t index;
            uint64_t seq;
            bool known = false;
        };
        std::vector<LiveMode> m_live;
        // RebelCameraMode instances on the player's camera whose class is not in mode_classes() (shadowstep attack,
        // finisher and effect cameras), each with its display name, taken like m_live. Only listed (list_modes).
        struct Untracked
        {
            LiveRef ref;
            std::wstring name;
            uint64_t seq;
        };
        std::vector<Untracked> m_untracked;
        static constexpr size_t MAX_UNTRACKED = 32;
        uint64_t m_seq = 0;
        PositionTuning m_last;                          // what the last apply wrote, for a class that loads later
        bool m_scan_needed = true;                      // m_live does not cover this camera yet
        static constexpr size_t MAX_NEW = 256;
        std::mutex m_new_mutex;                         // m_new, m_new_overflow: written by note_new on any thread
        std::vector<LiveRef> m_new;
        bool m_new_overflow = false;
        Offsets m_off;
        // bEnableCameraHorizontalLag / VerticalLag: bitfields sharing a byte, written through their masks.
        // Properties of a native class, so the pointers hold for the session.
        FBoolProperty* m_hlag_on = nullptr;
        FBoolProperty* m_vlag_on = nullptr;
        // CameraOffset.bOverrideFOV: read and written with the CameraOffset value as the container, like the lag bits.
        FBoolProperty* m_offset_override = nullptr;
        bool m_layout_ok = false;
        bool m_layout_tried = false;
        bool m_applied = false;
        bool m_was_active = false;
        FScriptMapLayout m_map_layout{};

        UFunction* m_set_type = nullptr;
        UFunction* m_get_type = nullptr;
        UFunction* m_get_state = nullptr; // RebelCameraMode:GetState; without it aiming() stays false
        UStruct* m_mode_base = nullptr;   // RebelCameraMode: what an untracked object must derive from to be listed
        UClass* m_camera_class = nullptr; // RebelCameraComponent: GetCameraType is called only on one
        bool m_camera_class_read = false;
        bool m_type_names_read = false;   // camera_type_name(): the enum names, read once
        int32_t m_type_offset = -1;       // GetCameraType's return value in its params
        std::vector<std::pair<int64, std::wstring>> m_type_names;

        enum FlipStage
        {
            Idle,
            Pending, // waiting for the camera to update
            Away,    // switched to another type
            Back     // switched back; waiting for the blend to end
        };
        FlipStage m_flip_stage = Idle;
        bool m_flip_again = false;
        UObject* m_flip_camera = nullptr;
        uint8_t m_flip_home = 0, m_flip_away = 0;
        uint64_t m_flip_seen = 0;
        float m_transition = 0.5f;
        double m_flip_back_at = 0.0; // view_seconds at the switch back

        auto resolve_layout() -> bool;

        // foreign: a value another mod wrote. Only those take the override FOV bound, which no shipped value has
        // been measured against, so a first capture is gated as before.
        static auto plausible(const Original& o, bool foreign = false) -> bool;

        // m_live gains a live object if it is one of the captured modes and not held yet. known: from the hand-off
        // (see LiveMode); a held mode the hand-off brings again becomes known. A rescan clears m_live, so every mode
        // it finds starts unknown again. The mode it joined, or nullptr.
        auto keep_if_mode(LiveRef ref, bool known) -> Mode*;

        // An untracked RebelCameraMode on the player's camera joins m_untracked. The class is checked first, so the
        // camera's other subobjects cost no name. Full: the dead are dropped, then the oldest.
        auto keep_untracked(LiveRef ref) -> void;

        // The player's live modes: objects whose outer is the camera and whose class is a captured mode, and the
        // other camera modes on it into m_untracked. It reads every object (24 ms measured; FindAllOf's name
        // compares took 60), so it runs once per camera or world, where a load hides it, and note_new() supplies
        // modes pushed later. Other cameras' modes are left alone: new instances copy the CDO, and a new player
        // camera gets its own apply.
        auto scan_instances(UObject* player_camera) -> void;

        // Capture first: after forget() nothing is captured, and a scan then would find no modes yet clear
        // m_scan_needed, leaving the apply to write the CDOs only.
        auto ensure_scanned(UObject* player_camera) -> void;

        // Game thread. Takes what note_new() handed over and drops collected modes. After an overflow the
        // list may have missed a mode, so the next mode_state() or apply scans.
        auto adopt_new() -> void;

        // A new instance does not copy its whole CDO: a Blueprint class copies only the properties whose CDO value
        // differed from its native parent's when its list was built (UBlueprintGeneratedClass::
        // CustomPropertyListForPostConstruction, UE 5.5.4 BlueprintGeneratedClass.cpp), and the rest keep the native
        // constructor's. The native modes default to FOV 90, so the 10 modes shipped at 90 (docs/design.md "0.10.1"; among them the CombatFromArm ranges,
        // CombatSprinting, Base_LongRange) were built at 90 whatever the CDO held (measured 2026-09-24: five draws,
        // each CombatFromArm_VeryLongRange born at 90 under a CDO at 110). So every hand-off instance is written,
        // CDO first as in adopt_late(). It must happen in the construction callback (note_new()): the push copies
        // DefaultFieldOfView into a native field GetFieldOfView() returns, and a write on the next tick left the
        // instance at 120 and GetFieldOfView() at 90; written at construction, both read 120 and the screen blended
        // to 120 (measured 2026-09-24). A mode handed off from another thread still gets this on the next tick.
        // No flip: CameraOffsets differs from the native default and so is copied.
        auto write_new(Mode& mode, LiveRef ref) -> void;

        // A mode whose class loaded after the last apply (CombatSprinting is loaded by day only): its
        // CDO and this first instance still hold the game's values, or, for a class captured before a map load,
        // the shipped ones or another mod's, so they are checked (adopt_from()) before the write. The name is
        // compared first, so the effect cameras pushed on the same camera cost no object lookup. True if the
        // instance joined m_live.
        auto adopt_late(LiveRef ref) -> bool;

        // LiveRefs, not pointers: a popped mode can be collected at any GC.
        template <typename Visit>
        auto for_each_live(Visit&& visit) -> void;

        template <typename Visit>
        auto for_each_instance(Visit&& visit) -> void;

        // transition: the flip's blend time; otherwise each mode's own.
        auto set_blend(bool transition) -> void;

        template <typename Visit>
        auto for_each_offset(UObject* object, Visit&& visit) -> void;

        // A CDO's or a live instance's current values: memory reads only.
        auto read_values(UObject* object, Original& out) -> void;

        // What write() puts into a mode for a tuning, computed from its base: the one computation behind the
        // writes and behind telling the mod's own values from another mod's (adopt_foreign()). type_blend is
        // carried over, not written: the flip owns it.
        static auto expected(const Mode& mode, const PositionTuning& t) -> Original;

        auto write(UObject* object, const Mode& mode, const PositionTuning& t) -> void;

        static auto same_keys(const Original& a, const Original& b) -> bool;

        // Values another mod wrote into a captured CDO or a live instance become that field of the mode's base, so
        // the writes after it apply the player's settings on top of them and restore() leaves them. A value is the
        // mod's own if it is the last write (expected() of the last tuning), the base (an instance copied from the
        // CDO, or an inactive apply) or the shipped value (a class that reloaded after a map load); anything else
        // is foreign. Only known live modes are read (LiveMode). Memory reads only, no UObject calls and no object
        // walk: at the start of apply() and restore(), and for one class in adopt_late(); never per tick. CDOs are
        // read before instances, and where they carry different foreign values the last one read wins; a value
        // that reads as the mod's own changes nothing.
        auto adopt_foreign() -> void;

        auto adopt_from(Mode& mode, const std::vector<Original>& seen) -> void;

        // ECameraModeState; 0xFF if the call wrote nothing.
        auto get_state(UObject* mode) -> uint8_t;

        // The return value at its reflected offset once camera_type_name() has resolved it, else at 0.
        auto get_camera_type(UObject* camera) -> uint8_t;

        // Whether the object is a RebelCameraComponent, the class GetCameraType belongs to. The class is looked up
        // once.
        auto is_rebel_camera(UObject* camera) -> bool;

        auto set_camera_type(UObject* camera, uint8_t type) -> void;

        // A request while away runs again after the switch back, so the latest values are read. One during the
        // glide back restarts at once: queued behind the glide, a quick second press moved the camera in two steps.
        auto request_flip(UObject* camera) -> void;
    };
} // namespace dw::smoothwalker::modes
