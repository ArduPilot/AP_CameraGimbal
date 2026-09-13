--[[
 Control SIYI camera thermal functions
--]]

local PARAM_TABLE_KEY = 82
local PARAM_TABLE_PREFIX = "SIYI_"

local SIYI_IP = "192.168.1.25"
local SIYI_UDP_PORT = 37260

local MAV_SEVERITY = {EMERGENCY=0, ALERT=1, CRITICAL=2, ERROR=3, WARNING=4, NOTICE=5, INFO=6, DEBUG=7}

-- add a parameter and bind it to a variable
local function bind_add_param(name, idx, default_value)
    assert(param:add_param(PARAM_TABLE_KEY, idx, name, default_value), string.format('could not add param %s', name))
    return Parameter(PARAM_TABLE_PREFIX .. name)
end

-- setup script specific parameters
assert(param:add_table(PARAM_TABLE_KEY, PARAM_TABLE_PREFIX, 10), 'could not add param table')

--[[
  // @Param: SIYI_ENABLE
  // @DisplayName: Enable SIYI control
  // @Description: Enable SIYI control
  // @Values: 0:Disabled,1:Enabled
  // @User: Standard
--]]
SIYI_ENABLE = bind_add_param("ENABLE", 1, 1)

--[[
  // @Param: DEBUG
  // @DisplayName: debug level
  // @Description: debug level
  // @Range: 0 1
  // @User: Standard
--]]
SIYI_DEBUG = bind_add_param("DEBUG", 2, 0)

--[[
  // @Param: ATT_RATE
  // @DisplayName: Attitude update rate
  // @Description: Attitude update rate
  // @Range: 0 50
  // @Units: Hz
  // @User: Standard
--]]
SIYI_ATT_RATE = bind_add_param("ATT_RATE", 3, 20)

--[[
  // @Param: THERM_RATE
  // @DisplayName: thermel update rate
  // @Description: thermal update rate
  // @Range: 0 50
  // @Units: Hz
  // @User: Standard
--]]
SIYI_THERM_RATE = bind_add_param("THERM_RATE", 4, 20)

--[[
  // @Param: TELEM_RATE
  // @DisplayName: telemetry rate
  // @Description: telemetry rate
  // @Range: 0 50
  // @Units: Hz
  // @User: Standard
--]]
SIYI_TELEM_RATE = bind_add_param("TELEM_RATE", 5, 10)

--[[
  // @Param: MAV_CHAN
  // @DisplayName: mavlink channel
  // @Description: mavlink channel
  // @Range: 0 10
  // @User: Standard
--]]
SIYI_MAV_CHAN = bind_add_param("MAV_CHAN", 6, 1)

--[[
  // @Param: TCAP_RATE
  // @DisplayName: Thermal capture rate
  // @Description: Rate for raw thermal capture requests. Reset to zero when the script starts and when the vehicle disarms.
  // @Range: 0 10
  // @Units: Hz
  // @User: Standard
--]]
SIYI_TCAP_RATE = bind_add_param("TCAP_RATE", 7, 0)

--[[
  // @Param: LIDR_RATE
  // @DisplayName: LiDAR update rate
  // @Description: Rate for MT11 laser distance requests. A positive value enables the laser. Zero disables the laser and distance requests.
  // @Range: 0 20
  // @Units: Hz
  // @User: Standard
--]]
SIYI_LIDR_RATE = bind_add_param("LIDR_RATE", 8, 0)

--[[
  // @Param: ROI_RATE
  // @DisplayName: ROI pointing update rate
  // @Description: Rate for converting MAV_CMD_DO_SET_ROI_LOCATION targets into local MT11 angle commands. Zero disables ROI pointing.
  // @Range: 0 50
  // @Units: Hz
  // @User: Standard
--]]
SIYI_ROI_RATE = bind_add_param("ROI_RATE", 9, 20)

--[[
  // @Param: PITCH_MAX
  // @DisplayName: Maximum ROI pitch
  // @Description: Maximum pitch angle commanded in ROI mode. Limits upward travel to prevent the camera mount binding.
  // @Range: -90 90
  // @Units: deg
  // @Increment: 1
  // @User: Standard
--]]
SIYI_PITCH_MAX = bind_add_param("PITCH_MAX", 10, 30)

-- Capturing must always be explicitly enabled after the script starts. Use a
-- RAM-only parameter update so this safety reset does not cause a flash write.
SIYI_TCAP_RATE:set(0)

local SIYI_HEADER1 = 0x55
local SIYI_HEADER2 = 0x66

--[[
   SIYI command codes
--]]
local READ_TEMP_FULL_SCREEN = 0x14
local ATTITUDE_EXTERNAL = 0x22
local GPS_EXTERNAL = 0x3E
local ACQUIRE_GIMBAL_ATTITUDE = 0x0D
local REQUEST_FIRMWARE_VERSION = 0x01
local PHOTO = 0x0C
local GIMBAL_ROTATION = 0x07
local SET_GIMBAL_ANGLE = 0x0E
local REQUEST_LASER_RANGE = 0x15
local SET_LASER_STATUS = 0x32

local last_therm_send_ms = uint32_t(0)
local last_att_send_ms = uint32_t(0)
local last_tcap_send_ms = uint32_t(0)
local last_lidar_send_ms = uint32_t(0)
local last_laser_enable_ms = uint32_t(0)
local last_roi_send_ms = uint32_t(0)
local last_att_request_ms = uint32_t(0)
local last_version_request_ms = uint32_t(0)
local version_request_sent = false
local version_reported = false

local last_fwd_source = uint32_t(0)
local last_fwd_port = 0

local send_sequence = 0
local siyi_attitude = nil
local was_armed = arming:is_armed()
local laser_enabled = false
local roi_target = nil
local roi_control_active = false

if SIYI_ENABLE:get() ~= 1 then
   return
end

--[[
   sock_udp_fwd is used for proxying requests from a GCS to the camera
--]]
local sock_udp_fwd = Socket(1)
if not sock_udp_fwd:bind("0.0.0.0", SIYI_UDP_PORT) then
   gcs:send_text(MAV_SEVERITY.ERROR, "SIYI: Failed to bind")
   return
end

--[[
   sock_udp_out is for our own requests to the camera
--]]
local sock_udp_out = Socket(1)
if not sock_udp_out:connect(SIYI_IP, SIYI_UDP_PORT) then
   gcs:send_text(MAV_SEVERITY.ERROR, "SIYI: Failed to connect")
   return
end

--[[
   CRC-16-CCITT
--]]
local function crc16(bytes)
   local crc = 0

   for i = 1, #bytes do
      local b = string.byte(string.sub(bytes, i, i))
      crc = crc ~ (b << 8)
      for _ = 1, 8 do
         if (crc & 0x8000) ~= 0 then
            crc = ((crc << 1) ~ 0x1021) & 0xFFFF
         else
            crc = (crc << 1) & 0xFFFF
         end
      end
   end
   return crc & 0xFFFF
end

-- Method to send packet
function send_packet(command_id, pkt)
    local plen = pkt and #pkt or 0
    local buf = string.pack("<BBBHHB", SIYI_HEADER1, SIYI_HEADER2, 1, plen, send_sequence, command_id)

    if pkt then
        buf = buf .. pkt
    end

    buf = buf .. string.pack("<H", crc16(buf))

    send_sequence = (send_sequence + 1) % 0xffff

    local success, err = pcall(function()
        sock_udp_out:send(buf, #buf)
    end)
    if err and SIYI_DEBUG:get() > 0 then
       gcs:send_text(MAV_SEVERITY.INFO, string.format("send_packet err:%s len=%u", err, #buf))
    end

    return success
end

-- Method to send packet with formatting
function send_packet_fmt(command_id, fmt, ...)
    local args = { ... }
    fmt = fmt or ""

    local success, err = pcall(function()
        local packed_pkt = string.pack(fmt, table.unpack(args))
        send_packet(command_id, packed_pkt)
    end)
    if err and SIYI_DEBUG:get() > 0 then
       gcs:send_text(MAV_SEVERITY.INFO, string.format("send_packet_fmt err:%s", err))
    end

    return success
end

-- wrap yaw angle in degrees to value between 0 and 360
local function wrap_360(angle)
   local res = math.fmod(angle, 360.0)
    if res < 0 then
        res = res + 360.0
    end
    return res
end

-- wrap yaw angle in degrees to value between -180 and +180
local function wrap_180(angle_deg)
  local res = wrap_360(angle_deg)
  if res > 180 then
    res = res - 360
  end
  return res
end

-- round to the nearest integer, symmetrically around zero
local function round_int(value)
   if value >= 0 then
      return math.floor(value + 0.5)
   end
   return math.ceil(value - 0.5)
end

local function unpack_version(version)
   local product = (version >> 24) & 0xff
   local major = (version >> 16) & 0xff
   local minor = (version >> 8) & 0xff
   local patch = version & 0xff
   return product, major, minor, patch
end

local function handle_firmware_version(data)
   if #data < 8 then
      return
   end

   local camera_version, gimbal_version = string.unpack("<I4I4", data)
   local _, camera_major, camera_minor, camera_patch = unpack_version(camera_version)
   local _, gimbal_major, gimbal_minor, gimbal_patch = unpack_version(gimbal_version)

   -- The camera can return v0.0.0 for about 30 seconds while it initializes.
   -- Keep retrying rather than displaying that transient value at startup.
   if camera_major == 0 and camera_minor == 0 and camera_patch == 0 and
      gimbal_major == 0 and gimbal_minor == 0 and gimbal_patch == 0 then
      return
   end

   version_reported = true
   gcs:send_text(MAV_SEVERITY.INFO,
                 string.format("SIYI camera v%u.%u.%u gimbal v%u.%u.%u",
                               camera_major, camera_minor, camera_patch,
                               gimbal_major, gimbal_minor, gimbal_patch))
end

local function handle_gimbal_attitude(data)
   if #data < 12 then
      return
   end
   local z,y,x,sz,sy,sx = string.unpack("<hhhhhh", data)
   local roll, pitch, yaw = x*0.1, y*0.1, wrap_180(-z*0.1)
   siyi_attitude = { roll, pitch, yaw }
   -- Keep ArduPilot's scripting-mount attitude current when that backend is
   -- configured. This call is harmless if no scripting mount exists.
   mount:set_attitude_euler(0, roll, pitch, yaw)
   gcs:send_named_float("CROLL", roll)
   gcs:send_named_float("CPITCH", pitch)
   gcs:send_named_float("CYAW", yaw)
   -- The MT11 rate fields are invalid, so do not stream or log them.
   logger:write('SIAR', 'Yaw,Pitch,Roll', 'fff', 'ddd', '---',
                yaw, pitch, roll)
end

local function handle_laser_range(data)
   if #data < 2 then
      return
   end
   local distance_dm = string.unpack("<I2", data)
   local distance_m = distance_dm * 0.1
   gcs:send_named_float("SIYI_LIDR", distance_m)
   logger:write('SILR', 'Dist', 'f', distance_m)
end

--[[
   handle full screen temperature
--]]
local function handle_temp_full_screen(data)
   if #data < 12 then
      return
   end
   local tmax, tmin, tmax_x, tmax_y, tmin_x, tmin_y = string.unpack("<HHHHHH", data)
   gcs:send_named_float("SIYI_X_TMAX", tmax)
   logger:write("SITM",'TMax,TMin,TMaxX,TMaxY,TMinX,TMinY','ffHHHH',
                tmax*0.01, tmin*0.01,
                tmax_x, tmax_y,
                tmin_x, tmin_y)

   if not siyi_attitude then
      return
   end

   local loc = ahrs:get_location()
   local gpsloc = gps:location(0)
   if not loc or not gpsloc then
      return
   end

   local data = string.pack("<iiifffHHffffff",
                            millis():toint(),
                            loc:lat(), loc:lng(), loc:alt()*0.01, gpsloc:alt()*0.01,
                            tmax*0.01, tmax_x, tmax_y,
                            siyi_attitude[1], siyi_attitude[2], siyi_attitude[3],
                            math.deg(ahrs:get_roll_rad()), math.deg(ahrs:get_pitch_rad()), math.deg(ahrs:get_yaw_rad()))
   local data96_hdr = string.pack("<BB", 71, #data)
   local data96 = data96_hdr .. data .. string.rep("\0", 96 - #data)
   mavlink.send_chan(SIYI_MAV_CHAN:get(), 172, data96 )
end

-- Function to parse a single SIYI packet
local function parse_packet(pkt)
   local _, _, _, _, _, cmd = string.unpack("<BBBHHB", pkt:sub(1, 8))
   local data = pkt:sub(9, -3)
   local crc = string.unpack("<H", pkt:sub(-2))
   local crc2 = crc16(pkt:sub(1, -3))

   if crc ~= crc2 then
      --gcs:send_text(MAV_SEVERITY.INFO, "bad crc")
      return
   end

   -- gcs:send_text(MAV_SEVERITY.INFO, string.format('got cmd=0x%x', cmd))

   if cmd == REQUEST_FIRMWARE_VERSION then
      handle_firmware_version(data)
   elseif cmd == READ_TEMP_FULL_SCREEN then
      handle_temp_full_screen(data)
   elseif cmd == ACQUIRE_GIMBAL_ATTITUDE then
      handle_gimbal_attitude(data)
   elseif cmd == REQUEST_LASER_RANGE then
      handle_laser_range(data)
   end
end

-- Function to parse SIYI packet data
local function parse_data(pkt)
   while #pkt >= 10 do
      local h1, h2, _, plen, _, _ = string.unpack("<BBBHHB", pkt:sub(1, 8))
      if h1 ~= SIYI_HEADER1 or h2 ~= SIYI_HEADER2 then
         --gcs:send_text(MAV_SEVERITY.INFO, "bad header")
         break
      end
      if plen + 10 > #pkt then
         --gcs:send_text(MAV_SEVERITY.INFO, "bad len")
         break
      end
      parse_packet(pkt:sub(1, plen + 10))
      pkt = pkt:sub(plen + 11)
   end
end

--[[
   request full frame thermel data
--]]
local function thermal_request()
   send_packet_fmt(READ_TEMP_FULL_SCREEN, "<B", 2)
end

--[[
   Request a still capture. With the MT11 raw-only camera-app patch and
   temp_frame enabled, this produces one raw thermal _I.bin file.
--]]
local function thermal_capture()
   send_packet_fmt(PHOTO, "<B", 0)
end

-- Request one LiDAR sample. The MT11 returns uint16 decimetres and returns
-- zero when it does not have a valid 5m--1200m measurement.
local function lidar_request()
   send_packet_fmt(REQUEST_LASER_RANGE)
end

local function log_roi_target(target, active)
   if target == nil then
      logger:write('SIRT', 'Lat,Lng,Alt,Frame,Active', 'LLfBB',
                   'DUm--', 'GG---', 0, 0, 0.0, 0, active and 1 or 0)
      return
   end
   logger:write('SIRT', 'Lat,Lng,Alt,Frame,Active', 'LLfBB',
                'DUm--', 'GG---', target:lat(), target:lng(),
                target:alt() * 0.01, target:get_alt_frame(),
                active and 1 or 0)
end

local function same_location(a, b)
   return a ~= nil and b ~= nil and
          a:lat() == b:lat() and a:lng() == b:lng() and
          a:alt() == b:alt() and
          a:get_alt_frame() == b:get_alt_frame()
end

local function stop_roi_control()
   if not roi_control_active then
      return
   end
   roi_control_active = false
   log_roi_target(roi_target, false)
   gcs:send_text(MAV_SEVERITY.INFO, "SIYI: ROI control stopped")
end

-- ArduPilot has already validated DO_SET_ROI_LOCATION and retained its
-- altitude frame in the scripting mount backend. Reading the mount target also
-- avoids consuming the singleton scripting MAVLink queue, which is shared with
-- other scripts such as hourstracker.lua.
local function refresh_roi_target()
   local target = mount:get_location_target(0)
   if target == nil then
      stop_roi_control()
      roi_target = nil
      return
   end

   if not same_location(roi_target, target) then
      roi_target = target:copy()
      log_roi_target(roi_target, roi_control_active)
   else
      -- Keep a private copy rather than retaining mount frontend userdata.
      roi_target = target:copy()
   end
end

local function update_roi_target()
   if roi_target == nil then
      return
   end

   local current = ahrs:get_location()
   local yaw_rad = ahrs:get_yaw_rad()
   if current == nil or yaw_rad == nil then
      return
   end

   local target_ned = current:get_distance_NED(roi_target)
   if target_ned == nil or target_ned:is_nan() or target_ned:is_inf() then
      return
   end

   local north = target_ned:x()
   local east = target_ned:y()
   local down = target_ned:z()
   local horizontal_distance = math.sqrt(north*north + east*east)
   if horizontal_distance < 0.01 and math.abs(down) < 0.01 then
      return
   end

   local yaw_earth_deg = math.deg(math.atan(east, north))
   local yaw_body_deg = wrap_180(yaw_earth_deg - math.deg(yaw_rad))
   local pitch_deg = math.deg(math.atan(-down, horizontal_distance))

   if not roi_control_active then
      -- A rate command persists until stopped. Stop it once before beginning
      -- angle updates, matching the transition used by mavproxy_SIYI.
      send_packet_fmt(GIMBAL_ROTATION, "<bb", 0, 0)
      roi_control_active = true
      log_roi_target(roi_target, true)
      gcs:send_text(MAV_SEVERITY.INFO, "SIYI: ROI control started")
   end

   -- SIYI yaw has the opposite sign to the vehicle-frame yaw convention.
   local command_yaw_deg = -yaw_body_deg
   local command_pitch_deg = math.min(pitch_deg, SIYI_PITCH_MAX:get())
   gcs:send_named_float("CYAW_D", command_yaw_deg)
   gcs:send_named_float("CPITCH_D", command_pitch_deg)
   send_packet_fmt(SET_GIMBAL_ANGLE, "<hh",
                   round_int(command_yaw_deg * 10.0),
                   round_int(command_pitch_deg * 10.0))
   logger:write('SICM', 'Yaw,Pitch', 'ff', 'dd', '--',
                command_yaw_deg, command_pitch_deg)
end

--[[
   send vehicle attitude to SIYI to aid in attitude estimation
--]]
local function send_attitude()
   local roll_rad = ahrs:get_roll_rad()
   local pitch_rad = ahrs:get_pitch_rad()
   local yaw_rad = ahrs:get_yaw_rad()
   local gyro_rad = ahrs:get_gyro()
   send_packet_fmt(ATTITUDE_EXTERNAL, "<iffffff",
                   millis():toint(),
                   roll_rad, pitch_rad, yaw_rad,
                   gyro_rad:x(), gyro_rad:y(), gyro_rad:z())
   -- gcs:send_text(MAV_SEVERITY.INFO, string.format("send att yaw=%f yaw-rate=%f", yaw_rad, gyro_rad:z()))
end

--[[
   Send the aircraft position and earth-frame velocity to the MT11.

   Opcode 0x3E uses NED velocity, not the gimbal's RFU/body frame:
      AHRS velocity x (north) -> vn
      AHRS velocity y (east)  -> ve
      AHRS velocity z (down)  -> vd

   Location latitude/longitude are already degE7 and absolute altitude is
   centimetres above MSL.  AHRS velocity is m/s, while the MT11 requires
   mE3/s (millimetres/s), hence the factor of 1000.

   The Lua AHRS interface does not expose geoid undulation, so an independent
   WGS84 ellipsoid altitude cannot be reconstructed here.  Use the AHRS MSL
   altitude for both altitude fields rather than mixing AHRS position with a
   different position source.
--]]
local function send_gps()
   local loc = ahrs:get_location()
   local velocity_ned = ahrs:get_velocity_NED()
   if not loc or not velocity_ned or
      velocity_ned:is_nan() or velocity_ned:is_inf() then
      return
   end

   local time_boot_ms = millis():toint() & 0xFFFFFFFF
   local alt_msl_cm = loc:alt()
   local vn_mm_s = round_int(velocity_ned:x() * 1000.0)
   local ve_mm_s = round_int(velocity_ned:y() * 1000.0)
   local vd_mm_s = round_int(velocity_ned:z() * 1000.0)

   send_packet_fmt(GPS_EXTERNAL, "<I4i4i4i4i4i4i4i4",
                   time_boot_ms,
                   loc:lat(), loc:lng(),
                   alt_msl_cm, alt_msl_cm,
                   vn_mm_s, ve_mm_s, vd_mm_s)
end

--[[
   check for input packets (replies) or packets from a GCS
--]]
local function check_input()
   while true do
      local pkt, ip, port = sock_udp_fwd:recv(1024)
      if not pkt then
         break
      end
      if ip then
         last_fwd_source = ip
         last_fwd_port = port
      end
      -- forward packet to the camera from a GCS
      sock_udp_out:send(pkt, #pkt)
   end

   while true do
      local pkt = sock_udp_out:recv(1024)
      if not pkt then
         break
      end
      --[[
         forward all replies to the GCS, for logging and to allow GCS control
      --]]
      if last_fwd_port ~= 0 then
         sock_udp_fwd:sendto(pkt, #pkt, last_fwd_source, last_fwd_port)
      end
      parse_data(pkt)
   end
end

--[[
   main update function
--]]
local function update()
   local now_ms = millis()

   local armed = arming:is_armed()
   if was_armed and not armed then
      -- Reset only on the armed-to-disarmed edge. A rate deliberately set
      -- while already disarmed is allowed to run.
      SIYI_TCAP_RATE:set(0)
   end
   was_armed = armed

   check_input()
   refresh_roi_target()

   -- Request immediately on script startup, then retry while the camera is
   -- initializing or if a request/reply is lost.
   if not version_reported and
      (not version_request_sent or now_ms - last_version_request_ms >= 2000) then
      send_packet_fmt(REQUEST_FIRMWARE_VERSION)
      last_version_request_ms = now_ms
      version_request_sent = true
   end

   local therm_rate = SIYI_THERM_RATE:get()
   if therm_rate > 0 then
      local therm_period_ms = 1000.0 / therm_rate
      if now_ms - last_therm_send_ms >= therm_period_ms then
         last_therm_send_ms = now_ms
         thermal_request()
      end
   end

   local tcap_rate = SIYI_TCAP_RATE:get()
   if tcap_rate > 0 then
      local tcap_period_ms = 1000.0 / tcap_rate
      if now_ms - last_tcap_send_ms >= tcap_period_ms then
         last_tcap_send_ms = now_ms
         thermal_capture()
      end
   end

   local lidar_rate = SIYI_LIDR_RATE:get()
   if lidar_rate > 0 then
      -- 0x32 has no ACK on the MT11. Repeat it slowly so a dropped enable
      -- request does not leave polling permanently inactive.
      if not laser_enabled or now_ms - last_laser_enable_ms >= 5000 then
         send_packet_fmt(SET_LASER_STATUS, "<B", 1)
         last_laser_enable_ms = now_ms
         laser_enabled = true
      end
      local lidar_period_ms = 1000.0 / lidar_rate
      if now_ms - last_lidar_send_ms >= lidar_period_ms then
         last_lidar_send_ms = now_ms
         lidar_request()
      end
   elseif laser_enabled then
      send_packet_fmt(SET_LASER_STATUS, "<B", 0)
      laser_enabled = false
   end

   local roi_rate = SIYI_ROI_RATE:get()
   if roi_rate > 0 and roi_target ~= nil then
      local roi_period_ms = 1000.0 / roi_rate
      if now_ms - last_roi_send_ms >= roi_period_ms then
         last_roi_send_ms = now_ms
         update_roi_target()
      end
   else
      stop_roi_control()
   end

   local att_rate = SIYI_ATT_RATE:get()
   if att_rate > 0 then
      local att_period_ms = 1000.0 / att_rate
      if now_ms - last_att_send_ms >= att_period_ms then
         last_att_send_ms = now_ms
         send_attitude()
         send_gps()
      end
   end

   -- Poll 0x0D directly rather than using continuous stream type 1. The MT11
   -- can leave a previously enabled 0x26 magnetic-encoder stream active and
   -- then ignore attempts to select the attitude stream. Direct requests work
   -- independently of that stream and always return the stabilized attitude
   -- format handled above.
   local telem_rate = SIYI_TELEM_RATE:get()
   if telem_rate > 0 then
      local telem_period_ms = 1000.0 / telem_rate
      if now_ms - last_att_request_ms >= telem_period_ms then
         last_att_request_ms = now_ms
         send_packet_fmt(ACQUIRE_GIMBAL_ATTITUDE)
      end
   end

   return update, 10
end

gcs:send_text(MAV_SEVERITY.INFO, "SIYI_control: loaded")

return update()
