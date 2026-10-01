#include <unity.h>
#include <cstring>
#include "gnss_diagnostics.h"
using namespace gnss_diagnostics;
void setUp() {} void tearDown() {}
Report report() {
  Report r;r.sourceAgeMs=5077;r.utcMs=43201000;r.byteAgeMs=4;r.sentenceAgeMs=8;r.advanceAgeMs=12;
  r.epochs=65535;r.resyncs=2;r.missingTime=3;r.backwards=4;r.duplicates=5;r.rejected=6;r.checksum=7;r.flags=27;r.satellites=8;return r;
}
void test_complete_golden_report_strict_decode() {
  const uint8_t golden[]={0x53,0x56,0x1c,0xe9,7,0,0xd5,0x13,0,0,0xe8,0x31,0x93,2,
    4,0,8,0,12,0,0xff,0xff,2,0,3,0,4,0,5,0,6,0,7,0,27,8};
  uint8_t buf[37];memset(buf,0xa5,sizeof(buf));PacketHeader h{0xe91c,7,MSG_GNSS_DIAGNOSTIC},out{};Report r;
  TEST_ASSERT_EQUAL(36,encode(buf,sizeof(buf),h,report()));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(golden,buf,36);TEST_ASSERT_EQUAL_HEX8(0xa5,buf[36]);
  TEST_ASSERT_TRUE(decode(buf,36,out,r));TEST_ASSERT_EQUAL(65535,r.epochs);TEST_ASSERT_EQUAL(7,r.checksum);
  for(unsigned n=0;n<=37;++n) if(n!=36) TEST_ASSERT_FALSE(decode(buf,n,out,r));
  TEST_ASSERT_FALSE(decode(nullptr,36,out,r));TEST_ASSERT_EQUAL(0,encode(buf,35,h,report()));
  buf[1]=0x46;TEST_ASSERT_FALSE(decode(buf,36,out,r));buf[1]=0x56;
  buf[34]|=0x80;TEST_ASSERT_FALSE(decode(buf,36,out,r));buf[34]=27;
  put32(buf+10,gnss_snapshot::kDayMs);TEST_ASSERT_FALSE(decode(buf,36,out,r));
}
void test_reboot_report_does_not_wait_for_sequence_or_pages() {
  Latest latest;const uint32_t start=UINT32_MAX-10;
  TEST_ASSERT_FALSE(latest.received());TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,latest.rxAgeMs(start));
  TEST_ASSERT_TRUE(latest.accept({0xe91c,60000,MSG_GNSS_DIAGNOSTIC},report(),start));
  TEST_ASSERT_EQUAL_UINT32(100,latest.rxAgeMs(start+100));
  auto r=report();r.epochs=2;
  TEST_ASSERT_TRUE(latest.accept({0xe91c,0,MSG_GNSS_DIAGNOSTIC},r,start+200));
  TEST_ASSERT_EQUAL(2,latest.report().epochs);TEST_ASSERT_EQUAL(0,latest.rxAgeMs(start+200));
}
void test_state_uses_observed_progress_not_estimated_source_age() {
  gnss_snapshot::Collector c;auto r=capture(c,1000);
  TEST_ASSERT_EQUAL_STRING("no_uart",state(r));r.byteAgeMs=0;TEST_ASSERT_EQUAL_STRING("no_nmea",state(r));
  r.sentenceAgeMs=0;TEST_ASSERT_EQUAL_STRING("no_epoch",state(r));
  r.flags=1;r.advanceAgeMs=0;TEST_ASSERT_EQUAL_STRING("no_fix",state(r));
  r.flags=3;r.sourceAgeMs=5077;TEST_ASSERT_EQUAL_STRING("fresh_fix",state(r));
  r.advanceAgeMs=2000;TEST_ASSERT_EQUAL_STRING("stale_epoch",state(r));
  TEST_ASSERT_EQUAL(65535,counter(90000));TEST_ASSERT_EQUAL(65534,age(90000));
}
int main(){UNITY_BEGIN();RUN_TEST(test_complete_golden_report_strict_decode);RUN_TEST(test_reboot_report_does_not_wait_for_sequence_or_pages);RUN_TEST(test_state_uses_observed_progress_not_estimated_source_age);return UNITY_END();}
