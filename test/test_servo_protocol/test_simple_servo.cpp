#include <unity.h>
#include <initializer_list>
#include "uart_target.h"
#include "uart_set_parser.h"
using uart_set_parser::Parser;
using uart_set_parser::Result;
void setUp() {} void tearDown() {}
void test_uart_mailbox_latest_and_timeout() {
  uart_target::Mailbox m;
  TEST_ASSERT_FALSE(m.ready());TEST_ASSERT_FALSE(m.set(-1,0));TEST_ASSERT_FALSE(m.set(180001,0));
  m.set(120000,10);m.set(30000,20);TEST_ASSERT_EQUAL_INT32(30000,m.target());
  m.expire(269);TEST_ASSERT_TRUE(m.ready());m.expire(270);TEST_ASSERT_FALSE(m.ready());
  TEST_ASSERT_EQUAL_STRING("watchdog_hold",m.state());
  m.set(90000,300);TEST_ASSERT_TRUE(m.ready());m.hold();TEST_ASSERT_FALSE(m.ready());
}
void test_uart_mailbox_wrap() {
  uart_target::Mailbox m;uint32_t start=UINT32_MAX-20;
  m.set(90000,start);m.expire(start+249);TEST_ASSERT_TRUE(m.ready());
  m.expire(start+250);TEST_ASSERT_FALSE(m.ready());
}
Result line(Parser &p, const char *text, uint32_t now, int32_t &angle) {
  Result result = Result::None;
  while (*text) { const auto next = p.push(*text++, now, angle); if (next != Result::None) result = next; }
  return result;
}

void test_parser_accepts_set_only() {
  Parser p; int32_t angle = -1;
  TEST_ASSERT_TRUE(line(p, "SET 0\r\n", 0, angle) == Result::Target);
  TEST_ASSERT_EQUAL_INT32(0, angle);
  TEST_ASSERT_TRUE(line(p, "SET 180000\n", 0, angle) == Result::Target);
  TEST_ASSERT_EQUAL_INT32(180000, angle);
  for (const char *bad : {"ARM\n", "STOP\n", "SET -1\n", "SET 180001\n", "SET 9x\n", "SET 99999999999\n"})
    TEST_ASSERT_TRUE(line(p, bad, 1, angle) == Result::Rejected);
  TEST_ASSERT_TRUE(line(p, "SET 90000\n", 2, angle) == Result::Target);
}

void test_parser_rejects_expired_partial_and_embedded_nul() {
  Parser p; int32_t angle = -1;
  const uint32_t start = UINT32_MAX - 100;
  line(p, "SET 9", start, angle);
  TEST_ASSERT_TRUE(line(p, "0000\n", start + 250, angle) == Result::Rejected);
  TEST_ASSERT_TRUE(line(p, "SET 90000\n", start + 251, angle) == Result::Target);
  line(p, "SET ", 1000, angle);
  p.push('\0', 1000, angle);
  TEST_ASSERT_TRUE(line(p, "90\n", 1000, angle) == Result::Rejected);
}

void test_parser_discards_suffix_after_stalled_poll() {
  Parser p; int32_t angle = -1;
  p.reset(true);
  TEST_ASSERT_TRUE(line(p, "SET 180000\n", 0, angle) == Result::Rejected);
  TEST_ASSERT_EQUAL_INT32(-1, angle);
  TEST_ASSERT_TRUE(line(p, "SET 90000\n", 1, angle) == Result::Target);
}


void test_parser_sequenced_command_and_sync() {
  Parser p;int32_t angle=0;
  TEST_ASSERT_TRUE(line(p,"SYNC\n",0,angle)==Result::Sync);
  TEST_ASSERT_TRUE(line(p,"SET2 42 3 100 91000\n",110,angle)==Result::Target);
  TEST_ASSERT_EQUAL_INT32(91000,angle);TEST_ASSERT_TRUE(p.frame().sequenced);
  TEST_ASSERT_EQUAL_UINT32(42,p.frame().epoch);TEST_ASSERT_EQUAL_UINT32(100,p.frame().stamp);
  for(const char *bad:{"SET2 42 3 100 180001\n","SET2 42 3 100 -1\n","SET2 42 3 100 90000 extra\n","SET2 42 3 4294967296 90000\n"})
    TEST_ASSERT_TRUE(line(p,bad,120,angle)==Result::Rejected);
  TEST_ASSERT_TRUE(line(p,"SET 90000\n",130,angle)==Result::Target);TEST_ASSERT_FALSE(p.frame().sequenced);
}
int main(int,char**){UNITY_BEGIN();
 RUN_TEST(test_uart_mailbox_latest_and_timeout);RUN_TEST(test_uart_mailbox_wrap);
 RUN_TEST(test_parser_accepts_set_only);RUN_TEST(test_parser_rejects_expired_partial_and_embedded_nul);
 RUN_TEST(test_parser_discards_suffix_after_stalled_poll);RUN_TEST(test_parser_sequenced_command_and_sync);
 return UNITY_END();}
