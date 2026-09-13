#include <unity.h>
#include <algorithm>
#include <cmath>
#include "servo_motion.h"
#include "servo_profile.h"
#include "command_freshness.h"
#include "control_cadence.h"
using servo_motion::Controller;
void setUp() {}
void tearDown() {}

void test_shared_default_and_persistent_speed_value() {
  Controller c;TEST_ASSERT_EQUAL(30,c.speed());
  for(double speed:{1.,29.5,30.,90.}) {
    double decoded=0;TEST_ASSERT_TRUE(c.setSpeed(speed));
    TEST_ASSERT_TRUE(servo_motion::decodeSpeed(servo_motion::encodeSpeed(speed),decoded));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9,speed,decoded);
  }
  for(double speed:{0.,.99,90.001,INFINITY+0.,NAN+0.})TEST_ASSERT_FALSE(c.setSpeed(speed));
  double decoded=30;TEST_ASSERT_FALSE(servo_motion::decodeSpeed(UINT32_MAX,decoded));
  TEST_ASSERT_FALSE(servo_motion::decodeSpeed(0,decoded));TEST_ASSERT_EQUAL(30,decoded);
}
void test_elapsed_speed_irregular_ticks_and_wrap() {
  Controller c(0);uint32_t now=UINT32_MAX-500;c.initializeUs(0,now);c.target(180);
  uint64_t elapsed=0;const uint32_t intervals[]={0,17,250,1000,3701,19000,50000};
  for(int i=0;i<1000;++i) {
    const uint32_t dt=intervals[i%7];const double before=c.position();now+=dt;elapsed+=dt;c.tickUs(now);
    TEST_ASSERT_DOUBLE_WITHIN(1e-7,std::min(180.,30*elapsed/1000000.0),c.position());
    TEST_ASSERT_TRUE(c.position()-before<=30*dt/1000000.0+1e-8);
    TEST_ASSERT_FALSE(c.faulted());
  }
  TEST_ASSERT_FALSE(c.moving());TEST_ASSERT_EQUAL(180,c.position());
}
void test_latest_target_reversal_and_no_deadband() {
  Controller c;c.initializeUs(90,0);c.target(90.001);c.tickUs(100);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9,90.001,c.position());TEST_ASSERT_FALSE(c.moving());
  c.target(180);c.tickUs(50100);const double before=c.position();
  c.target(170);c.target(120);c.target(0);c.tickUs(60100);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9,before-.3,c.position());TEST_ASSERT_EQUAL(0,c.requested());
  TEST_ASSERT_EQUAL(-30,c.velocity());
  TEST_ASSERT_FALSE(c.target(NAN));TEST_ASSERT_FALSE(c.target(-1));TEST_ASSERT_FALSE(c.target(181));
}
void test_live_speed_change_preserves_target_and_position() {
  Controller c;c.target(180);c.tickUs(50000);const double before=c.position();
  TEST_ASSERT_TRUE(c.setSpeed(10));TEST_ASSERT_EQUAL(180,c.requested());TEST_ASSERT_EQUAL(before,c.position());
  c.tickUs(60000);TEST_ASSERT_DOUBLE_WITHIN(1e-9,before+.1,c.position());
  c.setSpeed(90);c.tickUs(70000);TEST_ASSERT_DOUBLE_WITHIN(1e-9,before+1,c.position());
}
void test_stall_hold_and_fault_never_bank_time() {
  Controller c;c.target(180);c.tickUs(1000000);TEST_ASSERT_EQUAL(91.5,c.position());
  c.tickUs(1000250);TEST_ASSERT_DOUBLE_WITHIN(1e-8,91.5075,c.position());
  c.holdUs(1000250);c.tickUs(4000250);TEST_ASSERT_FALSE(c.moving());
  c.target(0);c.tickUs(4000500);TEST_ASSERT_DOUBLE_WITHIN(1e-8,91.5,c.position());
  c.faultUs(4000500);TEST_ASSERT_FALSE(c.target(180));c.tickUs(6000000);TEST_ASSERT_EQUAL(91.5,c.position());
  TEST_ASSERT_TRUE(c.faulted());c.initializeUs(90,6000000);TEST_ASSERT_FALSE(c.faulted());
}
void test_pwm_precision_and_gps_independent_cadence() {
  TEST_ASSERT_EQUAL(2728,servo_profile::dutyForAngle(0));TEST_ASSERT_EQUAL(8183,servo_profile::dutyForAngle(90));
  TEST_ASSERT_EQUAL(13639,servo_profile::dutyForAngle(180));
  TEST_ASSERT_NOT_EQUAL(servo_profile::dutyForAngle(90),servo_profile::dutyForAngle(90.02));
  control_cadence::GpsCadence gps;const uint32_t t=UINT32_MAX-25;
  TEST_ASSERT_FALSE(gps.poll(t));TEST_ASSERT_FALSE(gps.poll(t+49));TEST_ASSERT_TRUE(gps.poll(t+50));
  TEST_ASSERT_FALSE(gps.poll(t+50));TEST_ASSERT_TRUE(gps.poll(t+160));TEST_ASSERT_FALSE(gps.poll(t+161));
}
void test_http_generation_sequence_age_and_wrap() {
  command_freshness::HttpGate gate;gate.reset(55);
  TEST_ASSERT_TRUE(gate.accept(55,1,100,101));TEST_ASSERT_FALSE(gate.accept(55,1,100,102));
  TEST_ASSERT_FALSE(gate.accept(55,2,100,2100));TEST_ASSERT_FALSE(gate.accept(55,2,103,102));
  gate.reset(56);TEST_ASSERT_FALSE(gate.accept(55,3,100,103));const uint32_t start=UINT32_MAX-10;
  TEST_ASSERT_TRUE(gate.accept(56,1,start,start+20));TEST_ASSERT_FALSE(gate.accept(56,2,start,start+250,250));
  uint32_t value=0;TEST_ASSERT_FALSE(command_freshness::parseUint32("4294967296",10,value));
}
void test_radio_sequence_and_reboot_candidates() {
  command_freshness::RadioSequence seq;TEST_ASSERT_TRUE(seq.accept(65535,0));TEST_ASSERT_TRUE(seq.accept(0,1000));
  TEST_ASSERT_FALSE(seq.accept(65535,1100));TEST_ASSERT_FALSE(seq.accept(0,1200));TEST_ASSERT_TRUE(seq.accept(10,2000));
  TEST_ASSERT_FALSE(seq.accept(0,5000));TEST_ASSERT_FALSE(seq.accept(0,6000));TEST_ASSERT_TRUE(seq.accept(1,6000));
}
int main(int,char**){UNITY_BEGIN();
 RUN_TEST(test_shared_default_and_persistent_speed_value);
 RUN_TEST(test_elapsed_speed_irregular_ticks_and_wrap);
 RUN_TEST(test_latest_target_reversal_and_no_deadband);
 RUN_TEST(test_live_speed_change_preserves_target_and_position);
 RUN_TEST(test_stall_hold_and_fault_never_bank_time);
 RUN_TEST(test_pwm_precision_and_gps_independent_cadence);
 RUN_TEST(test_http_generation_sequence_age_and_wrap);
 RUN_TEST(test_radio_sequence_and_reboot_candidates);
 return UNITY_END();}
