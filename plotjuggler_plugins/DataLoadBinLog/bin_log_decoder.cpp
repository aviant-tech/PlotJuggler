#include "bin_log_decoder.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace bin_log
{
namespace
{
// Every record is [type u8][seq u8][payload]; the payload starts with a
// timestamp in units of 10 microseconds and its length depends on the type.
enum Type : uint8_t
{
  IMU = 0x01,
  BARO = 0x02,
  PWM1 = 0x03,
  PWM2 = 0x04,
  GNSS = 0x05,
  TEMPERATURE = 0x06,
  TEXT = 0x07,
  PARAMETER = 0x08,
  DEPLOY_INDICATORS = 0x09,
  STATE = 0x0a,
  ERROR = 0x0b,
  POWER = 0x0c,
  SYS_INFO = 0x0d,
  MAVLINK = 0x0f,
  SBUS = 0x10,
  EXT_ATTITUDE = 0x12,
  EXT_VELOCITY = 0x13,
  IMU_SINGLE = 0x14,
  SENSOR_FUSION = 0x15,
  GNSS_RF_INFO = 0x16,
  MARKER = 0xb0,
};

// Mavlink payload length by event number (payload byte 4).
constexpr size_t MAVLINK_LENGTHS[] = { 17, 21, 23, 26, 8, 31, 29, 12, 12, 12, 12, 12, 14, 36, 18 };
constexpr size_t MAVLINK_EVENT_COUNT = std::size(MAVLINK_LENGTHS);

const char* const MAVLINK_EVENTS[MAVLINK_EVENT_COUNT] = {
  "HEARTBEAT",   "SYSTEM_TIME",      "PING",          "PARAM_REQUEST_READ", "PARAM_REQUEST_LIST",
  "PARAM_VALUE", "PARAM_SET",        "CMD_DO_PARACHUTE", "CMD_REBOOT",      "CMD_REQUEST_MESSAGE",
  "FTP",         "AUTOPILOT_VERSION", "MESSAGE_INTERVAL", "CMD_USER_1",     "ACK"
};

const char* const PARAMETER_NAMES[] = {
  "TAKEOFF_HEIGHT",  "MAX_BANK_ANGLE",   "MAX_SINKRATE",       "MAX_YAWRATE",
  "MIN_ACCELERATION", "POWER_MONITOR",   "MANUAL_DEPLOY_INPUT", "INTERFACE",
  "UART_BAUDRATE",   "GNSS_MODE",        "GNSS_TIMEOUT_DEPLOY", "DEPLOY_DELAY",
  "LOG_MODE",        "INST_ANGLE_ROLL",  "INST_ANGLE_PITCH",   "INST_ANGLE_YAW",
  "MAX_TEMP_CELSIUS", "PWM_MOTORS_ENABLE", "PWM_MOTORS_DISABLE", "MAX_CAL_TIME",
  "CUSTOM_NUMBER"
};
constexpr size_t PARAMETER_COUNT = std::size(PARAMETER_NAMES);

// Raw parameter values are integers; a few are stored scaled.
double parameterScale(uint32_t id)
{
  switch (id)
  {
    case 0:
      return 0.1;
    case 2:
    case 4:
      return 0.01;
    case 8:
      return 100.0;
    case 11:
      return 0.001;
    default:
      return 1.0;
  }
}

const char* const IMU_FIELDS[] = { "acc1_x",  "acc1_y",   "acc1_z",   "gyro1_x",   "gyro1_y",
                                   "gyro1_z", "mag1_x",   "mag1_y",   "mag1_z",    "acc1_norm",
                                   "acc2_x",  "acc2_y",   "acc2_z",   "gyro2_x",   "gyro2_y",
                                   "gyro2_z", "mag2_x",   "mag2_y",   "mag2_z",    "acc2_norm",
                                   "roll_rev", "pitch_rev", "yaw_rev", "roll_old", "pitch_old",
                                   "yaw_old" };
const char* const BARO_FIELDS[] = { "altitude1", "altitude1_lpf", "pressure1", "temperature1",
                                    "vertical velocity1", "altitude2", "altitude2_lpf",
                                    "pressure2", "temperature2", "vertical velocity2" };
const char* const FUSION_FIELDS[] = { "roll",     "pitch",     "yaw",     "altitude",
                                      "vertical velocity", "roll_GDC", "pitch_GDC", "yaw_GDC",
                                      "altitude_GDC", "vertical velocity_GDC" };
// Little-endian readers over a payload.
struct Payload
{
  const uint8_t* p;
  size_t size;

  uint8_t u8(size_t o) const
  {
    return p[o];
  }
  int8_t s8(size_t o) const
  {
    return int8_t(p[o]);
  }
  uint16_t u16(size_t o) const
  {
    return uint16_t(p[o] | (p[o + 1] << 8));
  }
  uint32_t u32(size_t o) const
  {
    return uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 |
           uint32_t(p[o + 3]) << 24;
  }
  int32_t s32(size_t o) const
  {
    return int32_t(u32(o));
  }
  uint64_t u64(size_t o) const
  {
    return uint64_t(u32(o)) | uint64_t(u32(o + 4)) << 32;
  }
  float f32(size_t o) const
  {
    uint32_t bits = u32(o);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
  }
  // The text payload (after the timestamp) is stored bitwise inverted and NUL terminated.
  std::string invertedText() const
  {
    std::string out;
    for (size_t i = 4; i + 1 < size; i++)
    {
      out.push_back(char(~p[i]));
    }
    return out;
  }
};

// Payload length of the record starting at `p`, or 0 when these bytes cannot be a
// record: unknown type, truncated, or a zero timestamp.
size_t payloadLength(uint8_t type, const uint8_t* p, size_t available)
{
  size_t length = 0;
  switch (type)
  {
    case TEXT: {
      const void* end = available > 4 ? std::memchr(p + 4, 0, available - 4) : nullptr;
      length = end ? size_t(static_cast<const uint8_t*>(end) - p) + 1 : 0;
      break;
    }
    case SYS_INFO:
      length = available >= 27 ? 27 + 15 * size_t(p[26]) : 0;
      break;
    case MAVLINK:
      length = available >= 5 && p[4] < MAVLINK_EVENT_COUNT ? MAVLINK_LENGTHS[p[4]] : 0;
      break;
    case IMU:
      length = 109;
      break;
    case BARO:
    case SENSOR_FUSION:
      length = 45;
      break;
    case PWM1:
    case PWM2:
    case TEMPERATURE:
    case POWER:
      length = 8;
      break;
    case GNSS:
      length = 49;
      break;
    case PARAMETER:
    case DEPLOY_INDICATORS:
    case STATE:
      length = 12;
      break;
    case ERROR:
      length = 20;
      break;
    case SBUS:
      length = 22;
      break;
    case EXT_ATTITUDE:
      length = 16;
      break;
    case EXT_VELOCITY:
      length = 17;
      break;
    case IMU_SINGLE:
      length = 68;
      break;
    case GNSS_RF_INFO:
      length = 6;
      break;
    default:
      return 0;
  }
  if (length == 0 || length > available || Payload{ p, length }.u32(0) == 0)
  {
    return 0;
  }
  return length;
}

class Decoder
{
public:
  explicit Decoder(Sink& sink) : sink_(sink)
  {
  }

  void record(uint8_t type, Payload pl)
  {
    const double stamp = pl.u32(0) / 100000.0;
    switch (type)
    {
      case IMU:
        floats("imu", stamp, pl, 5, IMU_FIELDS, 26);
        sink_.number("imu", "imu1_valid", stamp, (pl.u8(4) >> 1) & 1);
        sink_.number("imu", "imu2_valid", stamp, (pl.u8(4) >> 2) & 1);
        sink_.number("imu", "calibrated", stamp, pl.u8(4) & 1);
        break;
      case BARO:
        floats("baro", stamp, pl, 5, BARO_FIELDS, 10);
        sink_.number("baro", "baro1_valid", stamp, pl.u8(4) & 1);
        sink_.number("baro", "baro2_valid", stamp, (pl.u8(4) >> 1) & 1);
        break;
      case PWM1:
        sink_.number("pwm", "PWM1", stamp, pl.u32(4));
        break;
      case PWM2:
        sink_.number("pwm", "PWM2", stamp, pl.u32(4));
        break;
      case GNSS:
        fields("gnss", stamp,
               { { "iTOW", double(pl.u32(4)) },
                 { "valid", double(pl.u8(8)) },
                 { "fixtype", double(pl.u8(12)) },
                 { "numSV", double(pl.u8(16)) },
                 { "longitude [deg]", pl.s32(20) * 1e-7 },
                 { "latitude [deg]", pl.s32(24) * 1e-7 },
                 { "altitude [m]", pl.s32(28) * 1e-3 },
                 { "hAcc [m]", pl.s32(32) * 1e-3 },
                 { "vAcc [m]", pl.s32(36) * 1e-3 },
                 { "pDOP", pl.u16(40) * 1e-3 },
                 { "horVel [m/s]", pl.f32(44) },
                 { "validity", double(pl.u8(48) & 3) } });
        break;
      case TEMPERATURE:
        sink_.number("temp", "temperature", stamp, pl.f32(4));
        break;
      case TEXT:
        sink_.text("text", "text", stamp, pl.invertedText());
        break;
      case PARAMETER: {
        const uint32_t id = pl.u32(4);
        sink_.number("param", "paramID", stamp, id);
        sink_.text("param", "name", stamp,
                   id < PARAMETER_COUNT ? PARAMETER_NAMES[id] : "param" + std::to_string(id));
        sink_.number("param", "value", stamp, pl.s32(8) * parameterScale(id));
        break;
      }
      case DEPLOY_INDICATORS:
        deployIndicators(stamp, pl);
        break;
      case STATE:
        sink_.number("state", "alarmlevel", stamp, pl.u32(8));
        sink_.number("state", "DRS_state", stamp, pl.u32(4));
        break;
      case ERROR:
        sink_.number("error", "code", stamp, pl.u32(4));
        sink_.number("error", "errorflags2 (hex)", stamp, pl.u32(16));
        sink_.number("error", "errorflags1 (hex)", stamp, pl.u32(12));
        sink_.number("error", "errorflags0 (hex)", stamp, pl.u32(8));
        break;
      case POWER:
        sink_.number("power", "power state", stamp, pl.u32(4));
        break;
      case SYS_INFO:
        sysInfo(stamp, pl);
        break;
      case MAVLINK:
        mavlink(stamp, pl);
        break;
      case SBUS:
        for (size_t i = 0; i < 9; i++)
        {
          sink_.number("sbus", "CH" + std::to_string(i + 1), stamp, pl.u16(4 + 2 * i));
        }
        break;
      case EXT_ATTITUDE: {
        const char* const fields[] = { "roll", "pitch", "yaw" };
        floats("extAttitude", stamp, pl, 4, fields, 3);
        break;
      }
      case EXT_VELOCITY: {
        const char* const fields[] = { "vel_x", "vel_y", "vel_z" };
        floats("extVelocity", stamp, pl, 4, fields, 3);
        sink_.number("extVelocity", "kind", stamp, pl.u8(16));
        break;
      }
      case IMU_SINGLE:
        for (size_t i = 0; i < 16; i++)
        {
          sink_.number("imuSingle", "f" + std::to_string(i), stamp, pl.f32(4 + 4 * i));
        }
        break;
      case SENSOR_FUSION: {
        // The ten floats are [roll, pitch, yaw, altitude, vertical velocity] twice (plain,
        // then _GDC). Flag bit 0 validates the attitude values, bit 1 the altitude ones;
        // the vendor leaves invalid cells empty, so we drop those samples.
        const uint8_t flags = pl.u8(4);
        for (size_t i = 0; i < 10; i++)
        {
          const bool altitude = i % 5 >= 3;
          if (flags & (altitude ? 2 : 1))
          {
            sink_.number("sensorFusion", FUSION_FIELDS[i], stamp, pl.f32(5 + 4 * i));
          }
        }
        break;
      }
      case GNSS_RF_INFO:
        sink_.number("gnssInfo", "jammingState", stamp, pl.u8(4));
        sink_.number("gnssInfo", "cwSuppression", stamp, pl.u8(5));
        break;
      default:
        break;
    }
  }

private:
  void fields(const std::string& topic, double stamp,
              std::initializer_list<std::pair<std::string, double>> values)
  {
    for (const auto& [name, value] : values)
    {
      sink_.number(topic, name, stamp, value);
    }
  }

  void floats(const std::string& topic, double stamp, Payload pl, size_t offset,
              const char* const* fields, size_t count)
  {
    for (size_t i = 0; i < count; i++)
    {
      sink_.number(topic, fields[i], stamp, pl.f32(offset + 4 * i));
    }
  }

  void deployIndicators(double stamp, Payload pl)
  {
    const uint16_t flags = pl.u16(8);
    const auto bit = [flags](int k) { return double((flags >> k) & 1); };
    fields("deplInd", stamp,
           { { "takeoff", bit(7) },
             { "motion", bit(9) },
             { "banking", bit(6) },
             { "freefall", bit(5) },
             { "gnss_lost", bit(4) },
             { "gnss_jammed", bit(10) },
             { "powerState", pl.s8(6) },
             { "outside_geofence", bit(3) },
             { "sinkrate", bit(2) },
             { "yawrate", bit(1) },
             { "maxtemp", bit(8) },
             { "pwm1", pl.s8(4) },
             { "pwm2", pl.s8(5) },
             { "deployTimer", bit(0) },
             { "mavlinkAPI_commandMode", pl.s8(10) },
             { "sbus_deployMode", pl.u8(11) } });
  }

  void sysInfo(double stamp, Payload pl)
  {
    fields("sysInfo", stamp,
           { { "free heap current", double(pl.u32(4)) },
             { "free heap min. ever", double(pl.u32(8)) },
             { "free KiB on SD-card", double(pl.u32(12)) },
             { "maxBytesInLogBuf", double(pl.u16(16)) },
             { "maxBytesInInterfaceBuf", double(pl.u16(18)) },
             { "maxBytesInManDepBuf", double(pl.u16(20)) },
             { "maxBytesInGnssBuf", double(pl.u16(22)) },
             { "logQueueLength", double(pl.u16(24)) } });
    // Then one 15-byte block per task.
    for (size_t task = 0; task < pl.u8(26); task++)
    {
      const size_t o = 27 + 15 * task;
      const std::string prefix = "task" + std::to_string(pl.u8(o)) + "/";
      fields("sysInfo", stamp,
             { { prefix + "state", double(pl.u8(o + 1)) },
               { prefix + "prio", double(pl.u8(o + 2)) },
               { prefix + "stack high water mark", double(pl.u32(o + 3)) },
               { prefix + "CPU utilization current [%]", pl.f32(o + 7) },
               { prefix + "CPU utilization total [%]", pl.f32(o + 11) } });
    }
  }

  void mavlink(double stamp, Payload pl)
  {
    const uint8_t event = pl.u8(4);
    sink_.number("mavlink", "sequence", stamp, pl.u8(5));
    sink_.number("mavlink", "sender sysID", stamp, pl.u8(6));
    sink_.number("mavlink", "sender compID", stamp, pl.u8(7));
    sink_.number("mavlink", "event", stamp, event);
    sink_.text("mavlink", "MAVLink event", stamp, MAVLINK_EVENTS[event]);
    // Event-specific fields start at byte 8.
    const Payload q{ pl.p + 8, pl.size - 8 };
    switch (event)
    {
      case 0:  // HEARTBEAT
        sink_.number("mavlink", "type", stamp, q.u8(0));
        sink_.number("mavlink", "autopilot", stamp, q.u8(1));
        sink_.number("mavlink", "base_mode", stamp, q.u8(2));
        sink_.number("mavlink", "custom_mode", stamp, q.u32(3));
        sink_.number("mavlink", "system_status", stamp, q.u8(7));
        sink_.number("mavlink", "mavlink_version", stamp, q.u8(8));
        break;
      case 1:  // SYSTEM_TIME
        sink_.number("mavlink", "time_unix_usec", stamp, double(q.u64(0)));
        sink_.number("mavlink", "time_boot_ms", stamp, q.u32(8));
        break;
      case 2:  // PING
        sink_.number("mavlink", "time_usec", stamp, double(q.u64(0)));
        sink_.number("mavlink", "seq", stamp, q.u32(8));
        sink_.number("mavlink", "target_system", stamp, q.u8(12));
        sink_.number("mavlink", "target_component", stamp, q.u8(13));
        break;
      case 3:  // PARAM_REQUEST_READ
        sink_.text("mavlink", "id", stamp, paramId(q));
        sink_.number("mavlink", "index", stamp, q.u16(16));
        break;
      case 5:  // PARAM_VALUE
      case 6:  // PARAM_SET
        sink_.text("mavlink", "id", stamp, paramId(q));
        sink_.number("mavlink", "value", stamp, q.f32(16));
        sink_.number("mavlink", "type", stamp, q.u8(20));
        if (event == 5)
        {
          sink_.number("mavlink", "count", stamp, q.u8(21));
          sink_.number("mavlink", "index", stamp, q.u8(22));
        }
        break;
      case 7:  // CMD_DO_PARACHUTE
        sink_.number("mavlink", "parachute_action", stamp, q.s32(0));
        break;
      case 11:  // AUTOPILOT_VERSION
        sink_.number("mavlink", "vendor_id", stamp, q.u16(0));
        sink_.number("mavlink", "product_id", stamp, q.u16(2));
        break;
      case 12:  // MESSAGE_INTERVAL
        sink_.number("mavlink", "interval_us", stamp, q.s32(0));
        sink_.number("mavlink", "message_id", stamp, q.u16(4));
        break;
      case 13:  // CMD_USER_1
        for (size_t i = 0; i < 7; i++)
        {
          sink_.number("mavlink", "param" + std::to_string(i + 1), stamp, q.f32(4 * i));
        }
        break;
      case 14:  // ACK
        sink_.number("mavlink", "command", stamp, q.u16(0));
        sink_.number("mavlink", "result", stamp, q.u8(2));
        sink_.number("mavlink", "progress", stamp, q.u8(3));
        sink_.number("mavlink", "result_param2", stamp, q.s32(4));
        sink_.number("mavlink", "target_system", stamp, q.u8(8));
        sink_.number("mavlink", "target_component", stamp, q.u8(9));
        break;
      default:
        break;
    }
  }

  static std::string paramId(Payload q)
  {
    const char* id = reinterpret_cast<const char*>(q.p);
    return std::string(id, strnlen(id, 16));
  }

  Sink& sink_;
};
}  // namespace

void decode(const uint8_t* data, size_t size, Sink& sink)
{
  // The first line names the firmware, e.g. "DRS_Parachute v2.12.0". Firmware
  // before v2.9 compressed the stream (LZ77), which this decoder does not handle.
  const auto* newline =
      size > 0 ? static_cast<const uint8_t*>(std::memchr(data, '\n', size)) : nullptr;
  const std::string header(reinterpret_cast<const char*>(data),
                           newline ? size_t(newline - data) : 0);
  int major = 0, minor = 0;
  if (std::sscanf(header.c_str(), "DRS_Parachute v%d.%d", &major, &minor) != 2 ||
      std::make_pair(major, minor) < std::make_pair(2, 9))
  {
    throw std::runtime_error("Not a DRS binary log of firmware v2.9 or later (header: \"" +
                             header + "\")");
  }

  Decoder decoder(sink);
  size_t pos = size_t(newline - data) + 1;
  while (pos + 2 <= size)
  {
    const uint8_t type = data[pos];
    const uint8_t* payload = data + pos + 2;
    const size_t available = size - pos - 2;
    // The marker is the one record without a timestamp: a single 0x0b byte.
    if (type == MARKER)
    {
      pos += available >= 1 && payload[0] == 0x0b ? 3 : 1;
      continue;
    }
    const size_t length = payloadLength(type, payload, available);
    // Like the vendor parser: on anything that does not look like a record,
    // drop one byte and try again from the next one.
    if (length == 0)
    {
      pos++;
      continue;
    }
    decoder.record(type, Payload{ payload, length });
    pos += 2 + length;
  }
}
}  // namespace bin_log
