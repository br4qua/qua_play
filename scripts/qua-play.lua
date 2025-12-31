#!/home/free2/Downloads/lua-5.4.7/src/lua

print("DEBUG: Script started")

-- Configuration
local NUKE_SUFFIX = ".pgo8"
local DEVICE = "hw:0,0"
local CORE = "4"
local BIT_DEPTH_VALID = {16, 32}
local BIT_DEPTH_OVERRIDE = nil
local BIT_DEPTH_FALLBACK = 32
local SAMPLE_RATE_VALID = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000}
local SAMPLE_RATE_OVERRIDE = nil
local SAMPLE_RATE_FALLBACK = 96000
local PADDING_HEAD = 0.00
local PADDING_TAIL = 0.00
local CACHE_LIMIT = 2147483648

local HOME = os.getenv("HOME")
local CURRENT_SONG_FILE = HOME .. "/.config/qua-player/current-song"

-- Helper function to safely quote paths for the shell
local function quote(path)
    if not path then return "''" end
    return "'" .. path:gsub("'", "'\\''") .. "'"
end

-- Helper function to run command and get output
local function run_command(cmd)
    local handle = io.popen(cmd)
    local result = handle:read("*a")
    handle:close()
    return result:gsub("%s+$", "") 
end

-- Helper function to check if file exists
local function file_exists(path)
    local f = io.open(path, "r")
    if f then f:close(); return true end
    return false
end

-- Safe tonumber conversion to prevent "base out of range"
local function safe_tonumber(val)
    if not val or val == "" then return nil end
    -- Strip any non-numeric characters except decimals
    local cleaned = val:match("[%d%.]+")
    return tonumber(cleaned)
end

-- Get player path with .pgo8 priority
local function get_player_path(bit_depth, sample_rate)
    local base_name = string.format("qua-player-%s-%s", bit_depth, sample_rate)
    local nuke_name = base_name .. NUKE_SUFFIX
    
    local path = run_command(string.format("command -v %s 2>/dev/null", nuke_name))
    if path == "" then
        path = run_command(string.format("command -v %s 2>/dev/null", base_name))
    end
    
    return path ~= "" and path or nil
end

local function launch_playback(player_path, wav_file, file_basename)
    if not player_path then
        print("Error: No player binary found for this format.")
        os.exit(1)
    end

    print("DEBUG: Using player: " .. player_path)
    os.execute(string.format('notify-send %s %s &', quote(player_path:match("([^/]+)$")), quote(file_basename)))

    local launch_cmd = string.format('qua-bare-launcher %s "$(realpath %s)" %s %s &',
        CORE, quote(player_path), quote(wav_file), quote(DEVICE))
    print("DEBUG: Launch command: " .. launch_cmd)
    os.execute(launch_cmd)
end

-- Parse arguments
local OFFSET, FILE_ARG = nil, nil
local i = 1
while i <= #arg do
    if arg[i] == "-n" then
        OFFSET = tonumber(arg[i + 1]); i = i + 2
    else
        FILE_ARG = arg[i]; i = i + 1
    end
end

-- Main file selection logic
local FILE = nil
if OFFSET and OFFSET ~= 0 then
    local current_file = io.open(CURRENT_SONG_FILE, "r")
    local CURRENT = current_file and current_file:read("*l")
    if current_file then current_file:close() end
    
    if CURRENT then
        local dir = CURRENT:match("(.*/)") or "./"
        local extensions = {"flac", "mp3", "wv", "ape", "opus", "ogg", "m4a", "mp4", "wav", "aiff", "aif"}
        local files = {}
        for _, ext in ipairs(extensions) do
            local handle = io.popen(string.format('ls %s*.%s 2>/dev/null | sort', quote(dir), ext))
            for line in handle:lines() do table.insert(files, line) end
            handle:close()
        end

        local current_index = nil
        for idx, f in ipairs(files) do if f == CURRENT then current_index = idx; break end end

        if current_index then
            local next_idx = ((current_index - 1 + OFFSET) % #files) + 1
            FILE = files[next_idx]
        end
    end
elseif FILE_ARG then
    FILE = FILE_ARG
else
    local f = io.open(CURRENT_SONG_FILE, "r")
    FILE = f and f:read("*l")
    if f then f:close() end
end

if not FILE or FILE == "" or not file_exists(FILE) then
    print("Error: No valid file to play"); os.exit(1)
end

local FILE_BASENAME = FILE:match("([^/]+)$") or FILE

-- Cleanup environment (FIXED systemctl stop)
local pids = run_command("pgrep qua-player")
if pids ~= "" then os.execute("kill -9 " .. pids .. " 2>/dev/null") end
os.execute("pkill -9 picom 2>/dev/null &")
os.execute("systemctl --user stop pipewire-pulse.socket pipewire.socket pipewire-pulse.service wireplumber.service pipewire.service")

-- Cache Check
local file_stat = run_command(string.format("stat -c '%%i-%%Y' %s", quote(FILE)))
local script_stat = run_command(string.format("stat -c '%%Y' %s", quote(arg[0] or "/proc/self/exe")))
local CACHE_PREFIX = string.format("/dev/shm/qua-cache/qua-temp-playback-%s-%s", file_stat, script_stat)

local cache_cmd = string.format('ls %s-*.wav 2>/dev/null', CACHE_PREFIX)
local cache_file = run_command(cache_cmd)

if cache_file ~= "" then
    print("DEBUG: Cache hit: " .. cache_file)
    local playback_sr = cache_file:match("%-(%d+)%.wav$")
    local playback_bd = cache_file:match("%-(%d+)%-%d+%.wav$")
    
    local out = io.open(CURRENT_SONG_FILE, "w")
    if out then out:write(FILE .. "\n"); out:close() end

    launch_playback(get_player_path(playback_bd, playback_sr), cache_file, FILE_BASENAME)
    os.exit(0)
end

-- Full Conversion Path
print("DEBUG: Cache miss, converting...")
os.execute("mkdir -p /dev/shm/qua-cache")
local TEMP_WAV_RAW = "/dev/shm/qua-temp-raw-" .. os.time() .. ".wav"
local EXT = FILE:match("%.([^.]+)$"):lower()

local decoders = {
    flac = 'flac -d %s -o %s',
    wv   = 'wvunpack %s -o %s',
    ape  = 'mac %s -d %s',
    mp3  = 'mpg123 -w %s %s',
    opus = 'opusdec --force-wav %s %s',
    ogg  = 'oggdec %s -o %s'
}

local decode_fmt = decoders[EXT] or 'ffmpeg -v quiet -i %s -f wav %s'
if EXT == "mp3" then
    os.execute(string.format(decode_fmt, quote(TEMP_WAV_RAW), quote(FILE)))
else
    os.execute(string.format(decode_fmt, quote(FILE), quote(TEMP_WAV_RAW)))
end

-- Give filesystem a tiny breath to ensure file is closed
os.execute("sleep 0.05")

-- Safety on tonumber to prevent "base out of range"
local detected_bd = safe_tonumber(run_command(string.format('soxi -p %s 2>/dev/null', quote(TEMP_WAV_RAW)))) or 16
local detected_sr = safe_tonumber(run_command(string.format('soxi -r %s 2>/dev/null', quote(TEMP_WAV_RAW)))) or 44100

local playback_bd = BIT_DEPTH_OVERRIDE or (detected_bd == 16 and 16 or 32)
local playback_sr = SAMPLE_RATE_OVERRIDE or detected_sr

local CACHE_FILE = string.format("%s-%s-%s.wav", CACHE_PREFIX, playback_bd, playback_sr)
local channels = safe_tonumber(run_command(string.format('soxi -c %s 2>/dev/null', quote(TEMP_WAV_RAW)))) or 2

local remix = ""
if channels == 1 then 
    remix = "channels 2" 
elseif channels == 6 then 
    remix = "remix 1,3v0.707,5v0.707 2,3v0.707,6v0.707"
    playback_bd = 32 
end

os.execute(string.format('sox -V3 %s -t wav -b %s -e signed-integer %s %s rate -v %s pad %s %s',
    quote(TEMP_WAV_RAW), playback_bd, quote(CACHE_FILE), remix, playback_sr, PADDING_HEAD, PADDING_TAIL))

os.execute(string.format('rm -f %s', quote(TEMP_WAV_RAW)))

-- Final Launch
local out = io.open(CURRENT_SONG_FILE, "w")
if out then out:write(FILE .. "\n"); out:close() end

launch_playback(get_player_path(playback_bd, playback_sr), CACHE_FILE, FILE_BASENAME)

print("DEBUG: Script completed")
