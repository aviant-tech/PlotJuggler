# DRS binary `.log` format

The binary log written by the DRS unit (firmware `DRS_Parachute v2.9` and later), as
reverse engineered from the vendor's closed-source parser `SW_040200_log-parser_V1.15`
(unstripped x86-64 ELF, disassembled with `objdump -d -C`). The decoder that implements this is
`plotjuggler_plugins/DataLoadBinLog/bin_log_decoder.cpp`; topic and series names are
the vendor CSV file and column names, so this document uses those.

Verification: build `tests/dump_csv.cpp` in that plugin directory (no Qt needed, instructions at
the top of the file), dump a log, run the vendor parser on a copy of the same log, and compare
with `tests/compare_with_vendor.py VENDOR_DIR MINE_DIR`. Two sample logs of firmware v2.12.0
(1 MB and 23 MB) match on every row and column.

## Container format (verified against both logs)

- First line: ASCII firmware id, e.g. `DRS_Parachute v2.12.0\n`. Parsing starts after the first `\n`
  (`Logfile::seekToSecondLine`). The vendor picks the `LogParser_vX_Y_Z` class from this string.
- Then a flat stream of records: `[type u8][seq u8][payload]`.
  - `seq` is one global 8-bit counter over all records (+1 per record, wraps). Mismatch only
    prints "ERROR: wrong sequence number! Missing N message(s)" and continues.
  - Every payload starts with `timestamp u32 LE` in units of 10 µs (vendor multiplies by 10 →
    µs; CSV prints seconds with 5 decimals). Timestamp 0 ⇒ record is "insane".
  - No compression for v2.9+ (`LogParser_v2_9_0::byteHandler` = `parseByte`, no LZ77;
    LZ77 + zero-run handling only exist in `LogParser_v2_0_0`, used by v2.0–v2.8 firmware).
  - Resync rule (`LogParser_v2_9_0::parseByte`): unknown type or "insane" message ⇒ drop ONE byte
    ("N byte(s) skipped") and retry from the next byte.
  - Payload lengths: fixed per type except text (NUL-terminated), sysInfo and mavlink (see below).
    A handler's `cmp $N,%edx` threshold means payload length = N+1.

## Record types (firmware v2.12.0)

| type | class (handler used) | payload len | CSV topic |
|---|---|---|---|
| 0x01 | Msg_IMU_v2_8_0 | 109 | imu |
| 0x02 | Msg_Baro_v2_9_0 | 45 | baro |
| 0x03 | Msg_PWM_v2_0_0 instance 1 | 8 | pwm (PWM1 column) |
| 0x04 | Msg_PWM_v2_0_0 instance 2 | 8 | pwm (PWM2 column) |
| 0x05 | Msg_GNSS_v2_9_0 | 49 | gnss (not in samples) |
| 0x06 | Msg_Temperature_v2_0_0 | 8 | temp |
| 0x07 | Msg_Text_v2_9_0 | 4 + NUL-terminated string | text |
| 0x08 | Msg_Parameter_v2_12_0 (handler = Parameter_v2_0_0) | 12 | param |
| 0x09 | Msg_DeplInd_v2_10_0 | 12 | deplInd |
| 0x0a | Msg_DrsState_v2_0_0 | 12 | state |
| 0x0b | Msg_Error_v2_2_0 | 20 | error |
| 0x0c | Msg_Power_v2_0_0 | 8 | power |
| 0x0d | Msg_SysInfo_v2_10_0 | 27 + 15·n, n = payload[26] (≤ 0x12) | sysInfo (no vendor CSV) |
| 0x0f | Msg_Mavlink_v2_11_0 | depends on event byte payload[4] (table below) | mavlink |
| 0x10 | Msg_sbus_v2_4_0 | 22 | sbus |
| 0x11 | Msg_sbusTelemetry_v2_5_0 | variable (not analysed) | sbusTelemetry |
| 0x12 | Msg_extAttitude_v2_5_0 | 16 | extAttitude |
| 0x13 | Msg_extVelocity_v2_10_0 | 17 | extVelocity |
| 0x14 | Msg_IMUsingle_v2_8_0 (handler = IMUsingle_v2_6_0) | 68 | imuSingle (not verified) |
| 0x15 | Msg_sensorFusion_v2_10_0 | 45 | sensorFusion |
| 0x16 | Msg_GnssRfInfo_v2_10_0 | 6 | gnssInfo |
| 0xb0 | Msg_Boob_v2_0_0 | 1 (value must be 0x0b) | marker, no CSV |
| 0x00 | Msg_IMU_v2_0_0 (legacy) | – | not expected in v2.12 logs |

**Timestamp printing:** the vendor converts the µs int64 (`ts*10`) to *float32*, divides by 1e6f,
then prints `%.5f`. Reproduce with `float32(ts*10)/float32(1e6)` (0 mismatches on all topics);
`ts/1e5` in double differs in the 5th decimal for ~half the rows. The plugin uses the double
(more precise); only the CSV comparison applies the float32 model.

## Payload layouts (verified against vendor CSVs unless marked otherwise)

Offsets are within the payload (after the 2-byte record header); all values little-endian;
`f32` = IEEE float. The authoritative form is `bin_log_decoder.cpp`.

- **Text 0x07**: `ts u32`, then bytes `~c` (bitwise NOT) until `0x00`.
- **Baro 0x02** (45): ts, `flags u8 @4` (bit0 = baro1_valid, bit1 = baro2_valid), 10 × f32 @5 in
  file order: altitude1, altitude1_lpf, pressure1, temperature1, vertical velocity1, altitude2,
  altitude2_lpf, pressure2, temperature2, vertical velocity2. (Vendor CSV column order differs:
  temperature1,pressure1,altitude1,altitude1_lpf,vertical velocity1, same for 2, baro1_valid,
  baro2_valid.)
- **IMU 0x01** (109): ts, `flags u8 @4` (bit0 = calibrated, bit1 = imu1_valid, bit2 = imu2_valid;
  from the byteHandler — the samples only contain 0x0 and 0x7 so data cannot distinguish them),
  26 × f32 @5 in file order: acc1_xyz, gyro1_xyz, mag1_xyz, acc1_norm, acc2_xyz, gyro2_xyz,
  mag2_xyz, acc2_norm, roll_rev, pitch_rev, yaw_rev, roll_old, pitch_old, yaw_old.
  CSV order: acc1..mag2 (18), acc1_norm, acc2_norm, rpy_rev, rpy_old, imu1_valid, imu2_valid,
  calibrated.
- **sensorFusion 0x15** (45): ts, `flags u8 @4`, 10 × f32 @5: roll, pitch, yaw, altitude,
  vertical velocity, roll_GDC, pitch_GDC, yaw_GDC, altitude_GDC, vertical velocity_GDC.
  Vendor prints empty cells for roll/pitch/yaw (+_GDC) unless flags bit0, and for
  altitude/vertical velocity (+_GDC) unless flags bit1.
- **extAttitude 0x12** (16): ts, roll, pitch, yaw (f32).
- **extVelocity 0x13** (17): ts, vel_x, vel_y, vel_z (f32), kind u8 @16.
- **Temperature 0x06** (8): ts, f32 temperature.
- **Power 0x0c** (8): ts, u32 `power state` (0 initializing, 1 power on, 2 power off).
- **DrsState 0x0a** (12): ts, u32 DRS_state @4, u32 alarmlevel @8. CSV order: alarmlevel, DRS_state.
- **PWM 0x03/0x04** (8): ts, u32 pwm. Type 3 → column PWM1, type 4 → PWM2; the vendor writes one
  row per record with the other column empty.
- **Parameter 0x08** (12): ts, u32 paramID, int32 raw value. Names (index = id, 21 entries):
  TAKEOFF_HEIGHT, MAX_BANK_ANGLE, MAX_SINKRATE, MAX_YAWRATE, MIN_ACCELERATION, POWER_MONITOR,
  MANUAL_DEPLOY_INPUT, INTERFACE, UART_BAUDRATE, GNSS_MODE, GNSS_TIMEOUT_DEPLOY, DEPLOY_DELAY,
  LOG_MODE, INST_ANGLE_ROLL, INST_ANGLE_PITCH, INST_ANGLE_YAW, MAX_TEMP_CELSIUS,
  PWM_MOTORS_ENABLE, PWM_MOTORS_DISABLE, MAX_CAL_TIME, CUSTOM_NUMBER. Value scaling: id0 /10
  (`%.1f`), id2 and id4 /100 (`%.2f`), id8 ×100 (`%d`), id11 /1000 (`%.3f`), all others as-is.
- **Error 0x0b** (20): ts, u32 code @4, errorflags0 @8, errorflags1 @12, errorflags2 @16.
  CSV order: code, errorflags2 (hex), errorflags1 (hex), errorflags0 (hex).
- **DeplInd 0x09** (12): ts; s8 pwm1 @4; s8 pwm2 @5; s8 powerState @6; u16 flags @8;
  s8 mavlinkAPI_commandMode @10; u8 sbus_deployMode @11. Flag bits → columns (from the snprintf
  argument order in `Msg_DeplInd_v2_10_0::getCSV`, motion confirmed by data): bit0 deployTimer,
  bit1 yawrate, bit2 sinkrate, bit3 outside_geofence, bit4 gnss_lost, bit5 freefall, bit6 banking,
  bit7 takeoff, bit8 maxtemp, bit9 motion, bit10 gnss_jammed. CSV order: takeoff, motion, banking,
  freefall, gnss_lost, gnss_jammed, powerState, outside_geofence, sinkrate, yawrate, maxtemp, pwm1,
  pwm2, deployTimer, mavlinkAPI_commandMode, sbus_deployMode. Labels: bools "false"/"true";
  pwm1/pwm2: invalid, do not deploy, deploy; powerState: initializing, power on, power off;
  mavlinkAPI_commandMode: uncontrolled, disabled, enabled, released; sbus_deployMode: invalid,
  do not deploy, deploy.
- **Mavlink 0x0f**: ts, event u8 @4, sequence u8 @5, sender sysID u8 @6, sender compID u8 @7,
  then event-specific fields @8. Payload length by event: 0:17 1:21 2:23 3:26 4:8 5:31 6:29 7:12
  8:12 9:12 10:12 11:12 12:14 13:36 14:18. Event names 0–14: HEARTBEAT, SYSTEM_TIME, PING,
  PARAM_REQUEST_READ, PARAM_REQUEST_LIST, PARAM_VALUE, PARAM_SET, CMD_DO_PARACHUTE, CMD_REBOOT,
  CMD_REQUEST_MESSAGE, FTP, AUTOPILOT_VERSION, MESSAGE_INTERVAL, CMD_USER_1, ACK.
  Verified field layouts (offsets from @8): HEARTBEAT type u8, autopilot u8, base_mode u8,
  custom_mode u32 @3, system_status u8 @7, mavlink_version u8 @8; SYSTEM_TIME time_unix_usec u64,
  time_boot_ms u32 @8; PARAM_VALUE id char[16], value f32 @16, type u8 @20, count u8 @21,
  index u8 @22; CMD_DO_PARACHUTE parachute_action i32; AUTOPILOT_VERSION vendor_id u16,
  product_id u16; MESSAGE_INTERVAL interval_us i32, message_id u16 @4; ACK command u16,
  result u8 @2, progress u8 @3, result_param2 i32 @4, target_system u8 @8, target_component u8 @9.
  Unverified guesses (no samples): PING, PARAM_REQUEST_READ, PARAM_SET, CMD_USER_1 (7 × f32).
  Vendor CSV columns: sequence, sender sysID, sender compID, MAVLink event, info (formatted
  string); the decoder emits the info fields as separate numeric columns instead.
- **Boob 0xb0**: one byte, must be 0x0b (session marker, no CSV).

Not verified (no samples in the two logs; layouts read from the byteHandlers only):

- **GNSS 0x05** (49): ts, iTOW u32 @4, valid u8 @8, fixtype u8 @12, numSV u8 @16, lon i32 @20 (1e-7
  deg), lat i32 @24, alt i32 @28 (mm), hAcc i32 @32, vAcc i32 @36, pDOP u16 @40 (1e-3),
  horVel f32 @44, validity u8 @48 (&3).
- **GnssRfInfo 0x16** (6): ts, jammingState u8, cwSuppression u8.
- **sbus 0x10** (22): ts, 9 × u16 channel values.
- **IMUsingle 0x14** (68): ts, 16 × f32.
- **SysInfo 0x0d**: ts, free heap current u32 @4, free heap min. ever u32 @8, free KiB on SD-card
  u32 @12, maxBytesInLogBuf u16 @16, maxBytesInInterfaceBuf @18, maxBytesInManDepBuf @20,
  maxBytesInGnssBuf @22, logQueueLength @24, task count n u8 @26, then n × 15 bytes @27: task id
  u8, state u8, prio u8, stack high water mark u32, CPU utilization current f32, total f32.
  Present in both logs (3 and 51 records) but the vendor writes no CSV for it by default.
- **sbusTelemetry 0x11**: variable length, not analysed; absent from the samples.
