-- SmoothwalkerExample: a drop-in demo of the Smoothwalker camera API (docs/api.md in the
-- dw-smoothwalker repo). F9 toggles a zoom layer, F10 a dutch-angle layer, F11 claims the
-- camera and releases with a glide. Console command: swexample view|layers|owner.
-- GPL-3.0-or-later, matching this repo's LICENSE; use, copy and modify freely under those terms.
-- Install: copy this SmoothwalkerExample folder into ue4ss/Mods/, keep enabled.txt with it.

local cfg = {
    zoom_fov      = 60,    -- degrees, absolute
    zoom_blend    = 0.4,   -- seconds
    tilt_roll     = 8,     -- degrees
    tilt_blend    = 0.3,   -- seconds
    claim_ttl     = 5.0,   -- seconds, refresh not needed for this short demo
    zoom_key      = "F9",
    tilt_key      = "F10",
    claim_key     = "F11",
}

local function log(fmt, ...) print("[SmoothwalkerExample] " .. string.format(fmt, ...) .. "\n") end

if Smoothwalker == nil then
    log("Smoothwalker not installed, nothing to do")
    return
end
if Smoothwalker.api_version < 3 then
    log("Smoothwalker api_version %d too old, need at least 3", Smoothwalker.api_version)
    return
end

local name, reason = Smoothwalker.register()
if name then log("registered as '%s'", name) else log("register() failed: %s", tostring(reason)) end

---------------------------------------------------------------------------- game-thread route
-- Smoothwalker.layer_set/layer_clear/claim/release are game thread only (docs/api.md, "Threads").
-- RegisterKeyBind callbacks are not guaranteed to land there, so queue through ExecuteInGameThread
-- with a prebuilt runner, same practice as DWFreeCam's "game-thread route".
local function guarded(fn)
    return function()
        local ok, yes = pcall(IsInGameThread)
        if not (ok and yes) then return end
        local ok2, err = pcall(fn)
        if not ok2 then log("error: %s", tostring(err)) end
    end
end

---------------------------------------------------------------------------- zoom layer (F9)

local zoomed = false
local function toggleZoom()
    zoomed = not zoomed
    local ok, why
    if zoomed then
        ok, why = Smoothwalker.layer_set{ fov_abs = cfg.zoom_fov, blend = cfg.zoom_blend }
    else
        ok, why = Smoothwalker.layer_clear()
    end
    if ok then log("zoom %s", zoomed and "on" or "off") else log("zoom layer failed: %s", tostring(why)) end
end
local runToggleZoom = guarded(toggleZoom)

---------------------------------------------------------------------------- dutch-angle layer (F10)

local tilted = false
local function toggleTilt()
    tilted = not tilted
    local ok, why
    if tilted then
        ok, why = Smoothwalker.layer_set{ rotation = { roll = cfg.tilt_roll }, blend = cfg.tilt_blend }
    else
        ok, why = Smoothwalker.layer_clear()
    end
    if ok then log("tilt %s", tilted and "on" or "off") else log("tilt layer failed: %s", tostring(why)) end
end
local runToggleTilt = guarded(toggleTilt)

-- Both toggles share this mod's one layer slot; a real mod driving two effects at once needs its
-- own state so one toggle's layer_clear does not clobber the other's layer_set.

---------------------------------------------------------------------------- claim / release (F11)

local owns = false
local function togglePhoto()
    if not owns then
        local ok, reason = Smoothwalker.claim{ ttl = cfg.claim_ttl }
        if ok then
            owns = true
            log("claimed the camera")
        elseif reason == "already_yours" then
            owns = true -- state drifted, re-sync
        elseif reason == "taken" then
            log("camera taken by '%s'", tostring(Smoothwalker.owner()))
        elseif reason == "must_keep" then
            log("Smoothwalker mid-crossfade, try again shortly")
        else
            log("claim failed: %s", tostring(reason))
        end
        return
    end
    local ok, reason = Smoothwalker.release("glide")
    if ok or reason == "not_owner" then
        owns = false
        log("released the camera")
    else
        log("release failed: %s", tostring(reason))
    end
end
local runTogglePhoto = guarded(togglePhoto)

---------------------------------------------------------------------------- key binds

local function bind(keyName, run)
    local k = Key[keyName]
    if not k then log("unknown key '%s', binding skipped", tostring(keyName)) return end
    RegisterKeyBind(k, function() ExecuteInGameThread(run) end)
end
bind(cfg.zoom_key, runToggleZoom)
bind(cfg.tilt_key, runToggleTilt)
bind(cfg.claim_key, runTogglePhoto)

---------------------------------------------------------------------------- console command

-- view(), layers() and owner() are thread-agnostic (docs/api.md), so this needs no game-thread hop.
RegisterConsoleCommandHandler("swexample", function(_, parameters)
    local verb = (parameters[1] or ""):lower()
    if verb == "view" then
        local v, why = Smoothwalker.view()
        if not v then log("view: nil, %s", tostring(why))
        else log("view shown fov=%.1f pitch=%.1f yaw=%.1f roll=%.1f age=%.3f",
            v.shown.fov, v.shown.pitch, v.shown.yaw, v.shown.roll, v.age) end
    elseif verb == "layers" then
        for _, l in ipairs(Smoothwalker.layers()) do
            log("layer mod=%s active=%s fov=%.1f weight=%.2f", l.mod, tostring(l.active), l.fov, l.weight)
        end
    elseif verb == "owner" then
        log("owner: %s", tostring(Smoothwalker.owner()))
    else
        log("usage: swexample view|layers|owner")
    end
    return true
end)
