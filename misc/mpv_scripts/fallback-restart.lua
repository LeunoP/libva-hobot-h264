-- mpv Lua script: fallback-restart.lua
--
-- Features:
-- 1. Silent Probing (first 5 seconds):
--    Keeps screen black and mutes audio behind the scenes to validate hardware acceleration.
-- 2. Probing Failure (1 anomaly detected):
--    - When an anomaly is detected during the probe:
--        Switches decoder to software mode (hwdec=no),
--        Rewinds to beginning (00:00:00),
--        Reveals screen, restores audio, and displays top-left OSD notice:
--          SW 디코더 (1.5s)
-- 3. Probing Success (Clean 5 seconds):
--    - Rewinds to beginning (00:00:00), reveals screen, restores audio,
--      and continues smooth HW playback.
-- 4. Mid-Playback Anomaly / Decode Error (Post-probing):
--    - If an error/anomaly occurs during playback:
--        Does NOT rewind (continues playing as-is),
--        Switches decoder to software mode (hwdec=no),
--        Displays top-left OSD notice:
--          SW 디코더 (1.5s)

local probing = false
local prev_mute = false
local probe_target_time = 5.0
local my_pid = mp.get_property_number("pid")
local file_start_epoch = os.time()
local is_switching = false
local last_ipc_count = 0
local anomaly_threshold = 1

local black_ov = mp.create_osd_overlay("ass-events")
black_ov.res_x = 1920
black_ov.res_y = 1080
black_ov.data = "{\\an7\\pos(0,0)\\bord0\\shad0\\1c&H000000&\\p1}m 0 0 l 1920 0 l 1920 1080 l 0 1080{\\p0}"

-- Top-left small OSD overlay for SW decoder notice (1.5s)
local notice_ov = mp.create_osd_overlay("ass-events")
notice_ov.res_x = 1920
notice_ov.res_y = 1080
local notice_timer = nil

local function show_sw_notice()
    notice_ov.data = "{\\an7\\pos(30,30)}{\\fs26}{\\b1\\bord2\\shad1\\c&HFFFFFF&\\3c&H111111&}SW 디코더"
    notice_ov:update()
    if notice_timer then notice_timer:kill() end
    notice_timer = mp.add_timeout(1.5, function()
        notice_ov:remove()
        notice_timer = nil
    end)
end

-- Probing SW switch: rewinds to 00:00:00, shows top-left notice, reveals screen
local function switch_to_sw_probe(reason)
    if is_switching then return end
    is_switching = true
    probing = false

    mp.msg.warn("[fallback-restart] [PROBE] HW anomaly threshold reached (" .. reason .. "). Switching to SW and restarting from 00:00:00...")

    mp.set_property("hwdec", "no")
    mp.commandv("seek", 0, "absolute", "exact")
    show_sw_notice()

    mp.add_timeout(0.2, function()
        black_ov:remove()
        mp.set_property_bool("mute", prev_mute)
        mp.msg.info("[fallback-restart] Screen revealed (SW mode from 00:00:00)")
    end)
end

-- Mid-playback SW switch: continues playing in place, NO seek, shows top-left alert
local function switch_to_sw_playback(reason)
    if is_switching then return end
    if mp.get_property("hwdec") == "no" then return end
    is_switching = true

    mp.msg.warn("[fallback-restart] [PLAYBACK] Mid-playback anomaly (" .. reason .. "). Switching to SW in place...")

    mp.set_property("hwdec", "no")
    show_sw_notice()
    mp.msg.info("[fallback-restart] Switched to SW mode seamlessly at current playback position.")
end

local function on_hw_success()
    if not probing then return end
    probing = false

    mp.msg.info("[fallback-restart] HW acceleration verified normal (5s passed).")
    mp.msg.info("[fallback-restart] Rewinding to beginning (00:00:00) and revealing screen...")

    mp.commandv("seek", 0, "absolute", "exact")
    pcall(os.remove, "/dev/shm/hobot_va_watchdog")
    last_ipc_count = 0

    mp.add_timeout(0.2, function()
        black_ov:remove()
        mp.set_property_bool("mute", prev_mute)
        mp.msg.info("[fallback-restart] Screen revealed (HW mode from 00:00:00)")
    end)
end

local function handle_anomaly(reason, count)
    if mp.get_property("hwdec") == "no" or is_switching then return end

    if probing then
        -- 검증 단계: 어노말리 1개 감지 시 바로 SW로 전환하고 처음으로 복귀
        if count >= anomaly_threshold then
            switch_to_sw_probe(reason)
        else
            mp.msg.info(string.format("[fallback-restart] [PROBE] WatchDog anomaly detected (%d/%d): %s", count, anomaly_threshold, reason))
        end
    else
        -- 이후 재생 중: 오류 발생 시 처음으로 보내지 않고, 왼쪽 위에 "SW디코더 전환" 작게 띄우고 재생 유지
        if count >= anomaly_threshold then
            switch_to_sw_playback(reason)
        else
            mp.msg.info(string.format("[fallback-restart] [PLAYBACK] WatchDog anomaly detected (%d/%d): %s", count, anomaly_threshold, reason))
        end
    end
end

local function check_watchdog_ipc()
    if mp.get_property("hwdec") == "no" or is_switching then return end
    local f = io.open("/dev/shm/hobot_va_watchdog", "r")
    if not f then return end
    local line = f:read("*line")
    f:close()
    if not line then return end

    local fpid, count, ts, err_mb, total_mb = line:match("(%d+)%s+(%d+)%s+(%d+)%s+(%d+)%s+(%d+)")
    fpid = tonumber(fpid)
    count = tonumber(count)
    ts = tonumber(ts)
    err_mb = tonumber(err_mb)
    total_mb = tonumber(total_mb)

    if fpid and my_pid and fpid == my_pid and ts and ts >= (file_start_epoch - 1) then
        if count and count > last_ipc_count then
            last_ipc_count = count
            local reason = string.format("Hobot-VA WatchDog (#%d, err_mb=%d/%d)", count, err_mb or 0, total_mb or 0)
            handle_anomaly(reason, count)
        end
    end
end

mp.add_periodic_timer(0.05, check_watchdog_ipc)

mp.register_event("file-loaded", function()
    if notice_ov then notice_ov:remove() end
    if notice_timer then notice_timer:kill() notice_timer = nil end

    local default_hwdec = mp.get_property("options/hwdec") or "vaapi"
    if default_hwdec ~= "no" then
        mp.set_property("hwdec", default_hwdec)
    end

    local hwdec_opt = mp.get_property("hwdec")
    if hwdec_opt == "no" then
        probing = false
        if black_ov then black_ov:remove() end
        return
    end

    probing = true
    is_switching = false
    last_ipc_count = 0
    file_start_epoch = os.time()
    pcall(os.remove, "/dev/shm/hobot_va_watchdog")

    prev_mute = mp.get_property_native("mute") or false
    mp.set_property_bool("mute", true)
    black_ov:update()
    mp.msg.info("[fallback-restart] Probing started behind black screen: validating HW acceleration for 5s...")
end)

mp.register_event("shutdown", function()
    pcall(os.remove, "/dev/shm/hobot_va_watchdog")
end)

mp.observe_property("time-pos", "number", function(name, pos)
    if probing and pos then
        local dur = mp.get_property_number("duration")
        local target = probe_target_time
        if dur and dur > 0 and dur < probe_target_time then
            target = math.max(0.5, dur - 0.2)
        end
        if pos >= target then
            on_hw_success()
        end
    end
end)

mp.enable_messages("v")

mp.register_event("log-message", function(e)
    if e.prefix == "fallback_restart" or e.prefix == "fallback-restart" then return end
    local txt = e.text or ""

    if string.find(txt, "Falling back to software decoding") then
        if probing then
            switch_to_sw_probe("Falling back to software decoding")
        else
            switch_to_sw_playback("Falling back to software decoding")
        end
        return
    end

    if string.find(txt, "Mapping hardware decoded surface failed")
       or string.find(txt, "hardware accelerator failed")
       or string.find(txt, "Error while decoding frame %(hardware decoding%)") then
        local reason = e.prefix .. ": " .. string.gsub(txt, "\n", " ")
        if probing then
            switch_to_sw_probe(reason)
        else
            switch_to_sw_playback(reason)
        end
    end
end)
