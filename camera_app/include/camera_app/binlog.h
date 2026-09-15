#ifndef CAMERA_APP_BINLOG_H
#define CAMERA_APP_BINLOG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct ca_config;
struct ca_gimbal_attitude;
/* DataFlash records are packed little-endian, as on all supported targets. */
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error BIN logging requires little endian
#endif
enum ca_binlog_id { CA_LOG_PARM=129, CA_LOG_MSG, CA_LOG_POS, CA_LOG_ATT,
    CA_LOG_GIMB, CA_LOG_PIDP, CA_LOG_PIDY, CA_LOG_MODE, CA_LOG_CMD,
    CA_LOG_CAM, CA_LOG_VID, CA_LOG_GCMD, CA_LOG_STAT, CA_LOG_TIME, CA_LOG_ROI,
    CA_LOG_PRMA, CA_LOG_VEND };
struct __attribute__((packed)) ca_log_vendor { uint64_t time_us; uint8_t opcode; uint16_t length; char payload[64]; };
struct __attribute__((packed)) ca_log_parm { uint64_t time_us; char name[16]; float value; };
struct __attribute__((packed)) ca_log_msg { uint64_t time_us; char text[64]; };
struct __attribute__((packed)) ca_log_pos { uint64_t time_us; uint32_t boot_ms; int32_t lat, lon; float alt, relalt, vn, ve, vd; };
struct __attribute__((packed)) ca_log_att { uint64_t time_us; uint32_t boot_ms; uint8_t source; float roll,pitch,yaw,rollrate,pitchrate,yawrate; };
struct __attribute__((packed)) ca_log_gimb { uint64_t time_us, sample_us; float roll,pitch,yaw,rollrate,pitchrate,yawrate; };
struct __attribute__((packed)) ca_log_pid { uint64_t time_us; float target,actual,rate,ff,error,p,i,d,output,dt,age; };
struct __attribute__((packed)) ca_log_mode { uint64_t time_us; uint32_t flight_mode; uint8_t armed,mode,method,yawlock,recording,system; };
struct __attribute__((packed)) ca_log_cmd { uint64_t time_us; uint16_t command; uint8_t system,component,result; float p1,p2,p3,p4,p5,p6,p7; };
struct __attribute__((packed)) ca_log_cam { uint64_t time_us; uint8_t scope; int32_t result,lat,lon; float alt,roll,pitch,yaw; };
struct __attribute__((packed)) ca_log_vid { uint64_t time_us; uint8_t active; int32_t result; char path[64]; };
struct __attribute__((packed)) ca_log_gcmd { uint64_t time_us; uint8_t mode; float pitch,yaw,wirep,wirey; int32_t result; };
struct __attribute__((packed)) ca_log_stat { uint64_t time_us; uint32_t dropped,queued,errors; };
struct __attribute__((packed)) ca_log_time { uint64_t time_us, utc_us; };
struct __attribute__((packed)) ca_log_roi { uint64_t time_us; int32_t lat,lon; float alt; uint8_t active; };
#define CA_BINLOG(id, type, ...) do { struct type r_ = { .time_us=ca_binlog_time_us(), __VA_ARGS__ }; ca_binlog_emit(id, &r_, sizeof(r_)); } while (0)
int ca_binlog_init(const char *root);
void ca_binlog_close(void);
bool ca_binlog_active(void);
/* Returns true on a new logging session. Startup parameters precede samples. */
bool ca_binlog_start(const struct ca_config *config);
void ca_binlog_stop(void);
void ca_binlog_emit(uint8_t id, const void *data, size_t size);
uint64_t ca_binlog_time_us(void);
void ca_binlog_message(const char *text);
void ca_binlog_parameter(const char *name, float value, bool applied);
void ca_binlog_feedback(const struct ca_gimbal_attitude *attitude);
void ca_binlog_stats(void);
void ca_binlog_vendor(uint8_t opcode, const uint8_t *payload, uint16_t length);
#endif
