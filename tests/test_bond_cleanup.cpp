// Dropping this node's own bond record, driven against a fake stack.
//
// The rule this machine has to hold to - only a cleanup still running may block
// anything - is the first test below, rather than a comment on
// bond_cleanup_blocking() that everyone has to remember.

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "omron_bond_cleanup.h"
#include "omron_diagnostics.h"
#include "omron_measurement.h"
#include "test_support.h"

using namespace esphome::omron;

namespace {

// A bond list that answers whatever the test sets, and counts what it was
// asked. Removal is deliberately not instant: ESP-IDF accepts the request and
// says nothing about the outcome, which is the whole reason the machine polls.
class FakeBondStack : public OmronBondCleanupHost {
 public:
  uint32_t now_ms{1000};
  BondLookupResult lookup_answer{BondLookupResult::PRESENT};
  bool remove_accepted{true};
  int lookup_error{0};
  int remove_error{0};

  // How many further lookups the record survives once a removal is accepted.
  // Removal is asked for exactly once - that is the behaviour, here and on the
  // node - so a stack that needs a moment has to be modelled on the lookup
  // side, which is also how the real one behaves: ESP_OK means the request was
  // taken, and the record goes when it goes.
  int lookups_before_vanishing{1};
  bool removal_requested{false};

  int lookups{0};
  int removals{0};
  int cache_drops{0};

  uint32_t bond_now_ms() override { return this->now_ms; }

  BondLookupResult bond_lookup(int &error) override {
    this->lookups++;
    error = this->lookup_error;
    const BondLookupResult answer = this->lookup_answer;
    if (this->removal_requested && this->lookups_before_vanishing > 0) {
      this->lookups_before_vanishing--;
      if (this->lookups_before_vanishing == 0)
        this->lookup_answer = BondLookupResult::ABSENT;
    }
    return answer;
  }

  bool bond_remove(int &error) override {
    this->removals++;
    error = this->remove_error;
    if (this->remove_accepted)
      this->removal_requested = true;
    return this->remove_accepted;
  }

  void bond_forget_attribute_cache() override { this->cache_drops++; }

  const char *bond_address() override { return "fake"; }
};

// Runs the machine forward, one poll interval at a time, up to a bound. Returns
// how many ticks it took to stop being pending.
int run_until_settled(OmronBondCleanup &cleanup, FakeBondStack &stack, int max_ticks = 200) {
  for (int tick = 0; tick < max_ticks; tick++) {
    if (!cleanup.pending())
      return tick;
    cleanup.tick(stack.now_ms);
    stack.now_ms += 250;
  }
  return max_ticks;
}

}  // namespace

void test_bond_cleanup_never_blocks_after_it_gives_up() {
  FakeBondStack stack;
  OmronDiagnostics diagnostics{};
  OmronBondCleanup cleanup;
  cleanup.set_host(&stack);
  cleanup.set_diagnostics(&diagnostics);
  cleanup.set_timeout_ms(2000);

  // A stack that will not let go: the record is there and every removal is
  // refused.
  stack.lookup_answer = BondLookupResult::PRESENT;
  stack.remove_accepted = false;

  cleanup.begin("test");
  assert(cleanup.pending());

  const int ticks = run_until_settled(cleanup, stack);
  assert(ticks < 200);  // it gave up rather than polling forever

  // The rule. A failed cleanup must not block: blocking makes connect and
  // disconnect unreachable, and those are the only two paths that can start
  // another cleanup and clear it. A stale bond is recoverable - the peer
  // refuses the encryption and we bond again. A blocked client is not.
  assert(cleanup.failed());
  assert(!cleanup.pending());
  assert(cleanup.phase() == OmronBondCleanup::Phase::FAILED);
  assert(diagnostics.cleanup_failures == 1);
  assert(diagnostics.phase == SessionPhase::IDLE);

  // And it can be asked again, which is what recovery depends on.
  stack.remove_accepted = true;
  cleanup.begin("second attempt");
  assert(cleanup.pending() && !cleanup.failed());
  run_until_settled(cleanup, stack);
  assert(cleanup.phase() == OmronBondCleanup::Phase::IDLE);
}

void test_bond_cleanup_waits_for_the_record_to_actually_go() {
  FakeBondStack stack;
  OmronDiagnostics diagnostics{};
  OmronBondCleanup cleanup;
  cleanup.set_host(&stack);
  cleanup.set_diagnostics(&diagnostics);
  cleanup.set_timeout_ms(10000);

  // The stack accepts the request and keeps listing the record for two more
  // polls. ESP-IDF returns ESP_OK for "request taken", never for "record gone",
  // and ESPHome does not deliver REMOVE_BOND_DEV_COMPLETE_EVT to clients at all
  // - so the disappearance of the address is the only proof available.
  stack.lookup_answer = BondLookupResult::PRESENT;
  stack.lookups_before_vanishing = 3;

  cleanup.begin("test");
  cleanup.tick(stack.now_ms);
  assert(cleanup.phase() == OmronBondCleanup::Phase::REMOVE_REQUESTED);
  assert(stack.removals == 1);
  assert(cleanup.pending());  // requested is not done
  // The cached attribute table goes only once the record is confirmed gone.
  // Dropping it while the removal is still in flight would discard a cache that
  // still has a live bond behind it.
  assert(stack.cache_drops == 0);

  run_until_settled(cleanup, stack);
  assert(cleanup.phase() == OmronBondCleanup::Phase::IDLE);
  assert(!cleanup.failed());
  assert(diagnostics.cleanup_failures == 0);
  // Asked to remove exactly once, and then kept looking. One acceptance is not
  // an outcome, so the polling is what actually decides this is done.
  assert(stack.removals == 1);
  assert(stack.lookups >= 4);
  assert(stack.cache_drops == 1);  // exactly once, on completion

  // A bond that was never there completes without asking anyone to remove it.
  FakeBondStack empty;
  empty.lookup_answer = BondLookupResult::ABSENT;
  OmronBondCleanup second;
  second.set_host(&empty);
  second.begin("nothing to do");
  second.tick(empty.now_ms);
  assert(second.phase() == OmronBondCleanup::Phase::IDLE);
  assert(empty.removals == 0);
  assert(empty.lookups == 1);
}

void test_bond_cleanup_clock_starts_at_the_first_tick() {
  FakeBondStack stack;
  OmronBondCleanup cleanup;
  cleanup.set_host(&stack);
  cleanup.set_timeout_ms(2000);
  stack.lookup_answer = BondLookupResult::PRESENT;

  // Armed here, and then nothing runs it for a minute. On the node that gap is
  // a teardown: the client only ticks this while the link is idle, and a slow
  // disconnect must not spend the whole budget before the first question is
  // asked. A clock started in begin() would have this time out having never
  // looked at the bond list once.
  cleanup.begin("armed during teardown");
  assert(cleanup.phase() == OmronBondCleanup::Phase::ARMED);
  stack.now_ms += 60000;

  cleanup.tick(stack.now_ms);
  assert(stack.lookups == 1);
  assert(!cleanup.failed());

  run_until_settled(cleanup, stack);
  assert(cleanup.phase() == OmronBondCleanup::Phase::IDLE);
}

void test_bond_cleanup_survives_a_list_that_will_not_answer() {
  FakeBondStack stack;
  OmronDiagnostics diagnostics{};
  OmronBondCleanup cleanup;
  cleanup.set_host(&stack);
  cleanup.set_diagnostics(&diagnostics);
  cleanup.set_timeout_ms(2000);

  // The list itself errors. Neither present nor absent, so there is nothing to
  // remove and nothing to conclude: keep asking until the timeout, then give
  // up without ever having requested a removal.
  stack.lookup_answer = BondLookupResult::ERROR;
  stack.lookup_error = -7;

  cleanup.begin("test");
  run_until_settled(cleanup, stack);

  assert(cleanup.failed());
  assert(!cleanup.pending());
  assert(stack.removals == 0);
  assert(stack.lookups >= 2);
  assert(diagnostics.cleanup_failures == 1);
  // A cleanup that gave up must not drop the cache either: the bond it belongs
  // to is still there.
  assert(stack.cache_drops == 0);

  // A second reason arriving while one is already in flight does not restart
  // the clock. A teardown asks twice - a failed OPEN and then the disconnect
  // behind it - and handing the second request a fresh timeout would let a
  // stuck list keep the client waiting indefinitely.
  FakeBondStack slow;
  slow.lookup_answer = BondLookupResult::ERROR;
  OmronBondCleanup second;
  second.set_host(&slow);
  second.set_timeout_ms(1000);
  // The loop stops at the failure on purpose: begin() on a machine that has
  // already given up is a new request and is meant to restart it. What must not
  // extend the clock is a begin() while one is still running - and if it did,
  // every pass here would re-arm and this would never reach the timeout at all.
  second.begin("first");
  second.tick(slow.now_ms);
  for (int i = 0; i < 8 && !second.failed(); i++) {
    slow.now_ms += 250;
    second.begin("again");  // ignored while pending
    second.tick(slow.now_ms);
  }
  assert(second.failed());
}

void test_datetime_formatting() {
  // The cuff stores local wall time with no zone at all, so appending "+00:00"
  // would assert UTC over it and shift every published reading by the local
  // offset.
  expect_string(format_datetime(OmronDateTime{2026, 8, 12, 15, 34, 35}).c_str(), "2026-08-12T15:34:35");
  expect_string(format_date(OmronDateTime{1955, 11, 5, 0, 0, 0}).c_str(), "1955-11-05");

  // Every field zero-padded to its width, which is what makes the result sort
  // and parse. A single-digit month printed bare would still look like a date.
  expect_string(format_datetime(OmronDateTime{2019, 1, 1, 0, 0, 0}).c_str(), "2019-01-01T00:00:00");
  expect_string(format_date(OmronDateTime{1900, 1, 1, 0, 0, 0}).c_str(), "1900-01-01");

  // The date form drops the time rather than zeroing it: a birth date has no
  // hour, and printing midnight would be invented precision.
  expect_string(format_date(OmronDateTime{1962, 7, 3, 13, 45, 59}).c_str(), "1962-07-03");

  // No zone suffix on either, which is what the paragraph above turns on.
  const std::string stamped = format_datetime(OmronDateTime{2026, 12, 31, 23, 59, 59});
  expect_string(stamped.c_str(), "2026-12-31T23:59:59");
  assert(stamped.find('+') == std::string::npos && stamped.find('Z') == std::string::npos);
}
