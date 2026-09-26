// Exercise the private time exchange with clocks that cannot change host time.
#define clock_gettime test_clock_gettime
#define clock_settime test_clock_settime
#include "../src/protocol/unigcs.cpp"
#undef clock_gettime
#undef clock_settime
#include <assert.h>
#include <stdio.h>

static time_t realtime;
static uint64_t monotonic=10000;
static unsigned set_calls;
static bool fail_set;

int test_clock_gettime(clockid_t clock,timespec *value)
{
    assert(clock==CLOCK_REALTIME || clock==CLOCK_MONOTONIC);
    *value=clock==CLOCK_REALTIME ? timespec{realtime,0} :
        timespec{time_t(monotonic/1000),long(monotonic%1000)*1000000};
    return 0;
}
int test_clock_settime(clockid_t clock,const timespec *value)
{
    assert(clock==CLOCK_REALTIME && value->tv_nsec==0);
    ++set_calls;
    if(fail_set) { errno=EPERM; return -1; }
    realtime=value->tv_sec;
    return 0;
}
void ca_log(const char *,...) {}
void ca_binlog_packet(bool,uint8_t,uint8_t,uint32_t,uint16_t,const uint8_t *,size_t,int32_t) {}
int ca_backend_set_zoom_rate(ca_backend *,float) { return 0; }
int ca_media_manual_focus(ca_media *,int) { return 0; }

static unsigned requests;
static void check_request(void *opaque,const ca_private_frame *f)
{
    auto *s=static_cast<ca_unigcs *>(opaque);
    assert(f->command==0x91 && f->control==0x09);
    assert(f->payload_length==1 && f->payload[0]==1);
    if(!s->long_format) {
        assert(f->source==0x34 && f->destination==s->source && f->link==0x16);
    }
    ++requests;
}
static void read_request(ca_unigcs &s,int peer,bool expected)
{
    uint8_t wire[256];
    const ssize_t n=recv(peer,wire,sizeof(wire),MSG_DONTWAIT);
    if(!expected) { assert(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)); return; }
    assert(n>0);
    ca_private_parser parser {};
    const unsigned before=requests;
    ca_private_network_parser_feed(&parser,wire,n,check_request,&s);
    assert(requests==before+1 && parser.length==0);
}
static void deliver(void *opaque,const ca_private_frame *f)
{
    receive_time(static_cast<ca_unigcs *>(opaque),f);
}
static void send_time(ca_unigcs &s,const uint8_t *date,size_t n,uint8_t control=0x0a,
                      uint8_t source=0xd0,uint8_t dest=0x34,uint8_t link=0x16,bool wrong_format=false)
{
    uint8_t wire[64];
    const size_t size=(s.long_format!=wrong_format) ?
        ca_long_build(wire,sizeof(wire),control&3,123,0x91,date,n) :
        ca_private_build(wire,sizeof(wire),control,123,source,dest,link,0x91,date,n);
    assert(size);
    ca_private_parser parser {};
    // Include stream fragmentation rather than bypassing the CRC parser.
    ca_private_network_parser_feed(&parser,wire,5,deliver,&s);
    ca_private_network_parser_feed(&parser,wire+5,size-5,deliver,&s);
    assert(parser.length==0);
}
static void test_exchange(bool old)
{
    int sockets[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    ca_unigcs s;
    s.client=sockets[0]; s.long_format=old;
    realtime=0; set_calls=0; fail_set=false;
    const uint8_t valid[]={0xea,0x07,9,25,0,45,31,0}; // 2026-09-25T00:45:31Z
    request_time(&s); read_request(s,sockets[1],false); // Wait for a known peer/format.
    s.source=0xd0;
    send_time(s,valid,8); assert(set_calls==0); // No unsolicited clock changes.
    request_time(&s); read_request(s,sockets[1],true);
    assert(s.time_pending);
    request_time(&s); read_request(s,sockets[1],false);
    monotonic+=999; request_time(&s); read_request(s,sockets[1],false);
    ++monotonic; request_time(&s); read_request(s,sockets[1],true);
    send_time(s,valid,8,0x0a,0xd0,0x34,0x16,true); assert(set_calls==0);
    if(!old) {
        send_time(s,valid,8,0x0a,0xd1);
        send_time(s,valid,8,0x0a,0xd0,0x2e);
        send_time(s,valid,8,0x0a,0xd0,0x34,0x11);
        send_time(s,valid,8,0x0b);
        assert(set_calls==0);
    }
    for(size_t n: {size_t(0),size_t(1),size_t(6),size_t(9)}) {
        uint8_t bad[9] {}; memcpy(bad,valid,sizeof(valid)); send_time(s,bad,n);
    }
    const uint8_t invalid[][8]={
        {0xea,7,0,25,0,45,31,0}, {0xea,7,13,25,0,45,31,0},
        {0xea,7,9,0,0,45,31,0}, {0xea,7,9,31,0,45,31,0},
        {0xeb,7,2,29,0,45,31,0}, {0xea,7,9,25,24,45,31,0},
        {0xea,7,9,25,0,60,31,0}, {0xea,7,9,25,0,45,60,0},
        {0xb2,7,1,1,0,0,0,0}, {0xea,7,8,31,23,59,59,0},
        {0xff,0xff,9,25,0,45,31,0}};
    for(const auto &bad:invalid) send_time(s,bad,sizeof(bad));
    assert(set_calls==0 && s.time_pending);
    fail_set=true; send_time(s,valid,8);
    assert(set_calls==1 && realtime==0 && s.time_pending);
    monotonic+=1000; request_time(&s); read_request(s,sockets[1],true);
    fail_set=false; send_time(s,valid,8);
    assert(set_calls==2 && realtime==1790297131 && !s.time_pending);
    monotonic+=1000; request_time(&s); read_request(s,sockets[1],false);
    send_time(s,valid,8); assert(set_calls==2);
    // MAVLink/public SDK may set the clock while a request is outstanding.
    realtime=0; request_time(&s); read_request(s,sockets[1],true);
    realtime=1800000000; send_time(s,valid,8);
    assert(realtime==1800000000 && set_calls==2 && !s.time_pending);
    // Also accept a packed reply sent using request/no-ACK framing.
    for(uint8_t control: {uint8_t(0x08),uint8_t(0x09)}) {
        realtime=0; request_time(&s); read_request(s,sockets[1],true);
        send_time(s,valid,7,control); assert(realtime==1790297131);
    }
    // A real leap day is valid, including when a reserved byte is nonzero.
    const uint8_t leap[]={0xec,7,2,29,12,34,56,0xa5};
    realtime=0; request_time(&s); read_request(s,sockets[1],true);
    send_time(s,leap,8); assert(realtime==1835440496);
    realtime=0; request_time(&s); read_request(s,sockets[1],true);
    disconnect(&s); assert(!s.time_pending && s.time_request_ms==0 && s.source==0);
    close(sockets[1]);
}
int main()
{
    // A configured camera timezone must not shift received UTC calendar data.
    for(const char *zone: {"UTC0","GMT-10","GMT+8"}) {
        assert(setenv("TZ",zone,1)==0); tzset();
        test_exchange(false); test_exchange(true);
    }
    puts("PASS UniGCS time: v3/legacy framing, peer checks, UTC dates, retries and existing clock preservation");
}
