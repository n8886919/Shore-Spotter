#include <unity.h>
#include "station_position.h"
#include "gnss_rate.h"
void setUp() {}
void tearDown() {}
void test_window_counts_epochs_not_polls_and_expires() {
  station_position::Average a;
  a.observe(0,true,0,24,121);
  for (unsigned i=1;i<500;++i) a.observe(i,true,0,24.1,121);
  TEST_ASSERT_EQUAL_UINT(1,a.count()); TEST_ASSERT_DOUBLE_WITHIN(1e-9,24,a.latitude());
  a.observe(500,true,500,24.0001,121);
  TEST_ASSERT_EQUAL_UINT(2,a.count()); TEST_ASSERT_DOUBLE_WITHIN(1e-9,24.00005,a.latitude());
  TEST_ASSERT_DOUBLE_WITHIN(.01,5.566,a.rmsM()); TEST_ASSERT_TRUE(a.warning());
  a.observe(30000,false,0,0,0); TEST_ASSERT_EQUAL_UINT(1,a.count());
  TEST_ASSERT_DOUBLE_WITHIN(1e-9,24.0001,a.latitude()); TEST_ASSERT_FALSE(a.warning());
  a.observe(30500,false,0,0,0); TEST_ASSERT_EQUAL_UINT(0,a.count());
}
void test_outliers_retained_and_wrap_safe() {
  station_position::Average a; const uint32_t t=UINT32_MAX-2000;
  for(unsigned i=0;i<100;++i) a.observe(t+i*500,true,i*500,24,121);
  TEST_ASSERT_EQUAL_UINT(60,a.count());
  a.observe(t+50000,true,50000,24.001,121);
  TEST_ASSERT_EQUAL_UINT(60,a.count()); TEST_ASSERT_TRUE(a.warning());
  TEST_ASSERT_TRUE(a.latitude()>24); // do not silently reject the outlier
  a.observe(t+80000,false,0,0,0); TEST_ASSERT_EQUAL_UINT(0,a.count());
}
void test_rate_observed_not_configured_and_missing_sentences() {
  gnss_rate::Monitor m;
  TEST_ASSERT_FALSE(m.observe(0,0,0,0)); TEST_ASSERT_FALSE(m.ready());
  TEST_ASSERT_TRUE(m.observe(5000,10,10,10)); TEST_ASSERT_EQUAL_STRING("observed_2hz",m.state());
  TEST_ASSERT_TRUE(m.observe(10000,15,15,15)); TEST_ASSERT_EQUAL_STRING("rate_mismatch",m.state());
  TEST_ASSERT_TRUE(m.observe(15000,25,25,15)); TEST_ASSERT_EQUAL_STRING("missing_gga",m.state());
  TEST_ASSERT_TRUE(m.observe(20000,25,25,15)); TEST_ASSERT_EQUAL_STRING("no_position_sentences",m.state());
}
int main(){UNITY_BEGIN();RUN_TEST(test_window_counts_epochs_not_polls_and_expires);RUN_TEST(test_outliers_retained_and_wrap_safe);RUN_TEST(test_rate_observed_not_configured_and_missing_sentences);return UNITY_END();}
