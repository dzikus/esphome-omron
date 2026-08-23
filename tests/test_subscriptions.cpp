// Turning on the notify channels, driven against a fake stack.
//
// The failure this exists for: a CCCD write that never happens, costs a whole
// pairing session and produces no error anywhere. It is the first test below.

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "omron_subscriptions.h"
#include "test_support.h"

using namespace esphome::omron;

namespace {

// A stack that accepts every request and then does exactly what the test tells
// it to - including nothing, which is the interesting case.
class FakeStack : public OmronSubscriptionHost {
 public:
  uint32_t now{1000};
  bool accept_register{true};
  int register_error{0};
  bool encryption_available{false};

  std::vector<uint16_t> registrations;
  std::vector<uint16_t> dropped;
  std::vector<uint16_t> retried;
  int ready_count{0};
  int failure_count{0};
  int encryption_requests{0};
  std::string last_failure;

  uint32_t subscription_now_ms() override { return this->now; }

  bool subscription_register(uint16_t characteristic_handle, int &error) override {
    this->registrations.push_back(characteristic_handle);
    error = this->register_error;
    return this->accept_register;
  }

  void subscription_ready() override { this->ready_count++; }

  void subscription_failed(const char *reason, int error) override {
    this->failure_count++;
    this->last_failure = reason;
    (void)error;
  }

  void subscription_dropped_optional(uint16_t characteristic_handle, const char *why, int error) override {
    this->dropped.push_back(characteristic_handle);
    (void)why;
    (void)error;
  }

  bool subscription_needs_encryption(int status) override {
    (void)status;
    if (!this->encryption_available)
      return false;
    this->encryption_requests++;
    return true;
  }

  const char *subscription_address() override { return "fake"; }

  void subscription_retrying(uint16_t characteristic_handle, uint8_t attempt, uint8_t of) override {
    this->retried.push_back(characteristic_handle);
    (void)attempt;
    (void)of;
  }
};

// Runs the retry clock forward far enough for one retry to be due.
void wait_out_the_retry(OmronSubscriptions &subs, FakeStack &stack) {
  stack.now += OmronSubscriptions::RETRY_INTERVAL_MS;
  subs.tick(stack.now);
}

}  // namespace

void test_subscriptions_retry_the_cccd_that_never_arrives() {
  FakeStack stack;
  OmronSubscriptions subs;
  subs.set_host(&stack);
  subs.add(0x0021, 0x0022, true);

  subs.begin(stack.now);
  assert(subs.phase() == OmronSubscriptions::Phase::SUBSCRIBING);
  assert(stack.registrations.size() == 1);

  // The peer accepting the registration is NOT the CCCD having been written.
  // Nothing may advance here: the write is a separate event, and on this cuff
  // it is the one that sometimes never comes.
  subs.on_register_result(0x0021, true, 0);
  assert(subs.pending());
  assert(stack.registrations.size() == 1);
  assert(stack.ready_count == 0);

  // The stack accepted the request and then did nothing. There is no event to
  // wait on: ESPHome's base class swallows a NOT_FOUND descriptor lookup with a
  // warning, so no CCCD write is ever issued and no WRITE_DESCR_EVT is ever
  // delivered. Asking again is the only recovery there is.
  wait_out_the_retry(subs, stack);
  assert(stack.registrations.size() == 2);
  assert(stack.retried.size() == 1 && stack.retried[0] == 0x0021);
  assert(subs.pending());

  // And it must stop asking. A retry loop with no bound holds the connection
  // slot for as long as the peer stays connected.
  for (int i = 0; i < 10 && subs.pending(); i++)
    wait_out_the_retry(subs, stack);
  assert(subs.phase() == OmronSubscriptions::Phase::FAILED);
  assert(stack.failure_count == 1);
  assert(stack.registrations.size() == OmronSubscriptions::MAX_ATTEMPTS);
  assert(stack.ready_count == 0);

  // Nothing is retried before the interval is up: a stack answering in tens of
  // milliseconds must never be interrupted mid-answer.
  FakeStack patient;
  OmronSubscriptions second;
  second.set_host(&patient);
  second.add(0x0031, 0x0032, true);
  second.begin(patient.now);
  patient.now += OmronSubscriptions::RETRY_INTERVAL_MS - 1;
  second.tick(patient.now);
  assert(patient.registrations.size() == 1);
}

void test_subscriptions_give_up_on_an_optional_target_and_carry_on() {
  FakeStack stack;
  OmronSubscriptions subs;
  subs.set_host(&stack);
  // The shape of a real session: required RX channel, optional standard blood
  // pressure, required unlock. The optional one sits in the middle on purpose -
  // dropping it must not strand what comes after it.
  subs.add(0x0021, 0x0022, true);
  subs.add(0x0029, 0x002A, false);
  subs.add(0x0031, 0x0032, true);

  subs.begin(stack.now);
  subs.on_descriptor_written(0x0022, true, 0);
  assert(stack.registrations.size() == 2 && stack.registrations[1] == 0x0029);

  // The optional one hangs. It costs its attempts and is then abandoned.
  for (int i = 0; i < 10 && stack.dropped.empty(); i++)
    wait_out_the_retry(subs, stack);
  assert(stack.dropped.size() == 1 && stack.dropped[0] == 0x0029);
  assert(stack.failure_count == 0);

  // The required target after it is now being asked for, with a fresh attempt
  // budget - the attempts the optional one burned are not evidence about this
  // one.
  assert(stack.registrations.back() == 0x0031);
  subs.on_descriptor_written(0x0032, true, 0);
  assert(subs.ready());
  assert(stack.ready_count == 1);
}

void test_subscriptions_park_until_the_link_is_encrypted() {
  FakeStack stack;
  stack.encryption_available = true;
  OmronSubscriptions subs;
  subs.set_host(&stack);
  subs.add(0x0021, 0x0022, true);
  subs.begin(stack.now);

  // The cuff refuses the CCCD write until the link is encrypted. That is this
  // device asking to be paired at the moment it matters, not a failure.
  subs.on_descriptor_written(0x0022, false, 5 /* INSUF_AUTHENTICATION */);
  assert(subs.phase() == OmronSubscriptions::Phase::AWAITING_ENCRYPTION);
  assert(stack.encryption_requests == 1);
  assert(stack.failure_count == 0);

  // Parked is not pending: the retry clock must not run here. Encryption has its
  // own timeout on the host, and two clocks on one wait is how a phase gets
  // silently retuned by somebody tightening the other one.
  assert(!subs.pending());
  const size_t before = stack.registrations.size();
  for (int i = 0; i < 5; i++)
    wait_out_the_retry(subs, stack);
  assert(stack.registrations.size() == before);

  // Once the host has encrypted, the same target is asked again.
  subs.resume(stack.now);
  assert(subs.pending());
  assert(stack.registrations.size() == before + 1);
  assert(stack.registrations.back() == 0x0021);
  subs.on_descriptor_written(0x0022, true, 0);
  assert(subs.ready());

  // A host that cannot start encryption turns the same refusal into a failure
  // rather than parking forever.
  FakeStack no_encryption;
  no_encryption.encryption_available = false;
  OmronSubscriptions second;
  second.set_host(&no_encryption);
  second.add(0x0021, 0x0022, true);
  second.begin(no_encryption.now);
  second.on_descriptor_written(0x0022, false, 5);
  assert(second.phase() == OmronSubscriptions::Phase::FAILED);
  assert(no_encryption.failure_count == 1);
}

void test_subscriptions_ignore_events_meant_for_something_else() {
  FakeStack stack;
  OmronSubscriptions subs;
  subs.set_host(&stack);
  subs.add(0x0021, 0x0022, true);
  subs.add(0x0031, 0x0032, true);
  subs.begin(stack.now);

  // Both handles belong to this machine, but only one of them is the target in
  // flight. Acting on the other would advance the queue past a channel that was
  // never subscribed - and every read after it would wait for notifications
  // that cannot arrive.
  subs.on_descriptor_written(0x0032, true, 0);
  assert(stack.registrations.size() == 1);
  subs.on_register_result(0x0031, false, 3);
  assert(stack.failure_count == 0);

  // A handle from another component entirely.
  subs.on_descriptor_written(0x00FF, true, 0);
  subs.on_register_result(0x00FF, false, 3);
  assert(stack.registrations.size() == 1);
  assert(stack.failure_count == 0);
  assert(subs.pending());

  // The right one still works.
  subs.on_descriptor_written(0x0022, true, 0);
  assert(stack.registrations.size() == 2);
}

void test_subscriptions_edge_cases() {
  // A session that needs no notifications is ready by definition, and must say
  // so rather than waiting for an event that has no source.
  FakeStack empty;
  OmronSubscriptions none;
  none.set_host(&empty);
  none.begin(empty.now);
  assert(none.ready());
  assert(empty.ready_count == 1);
  assert(empty.registrations.empty());

  // A stack that refuses the request outright is not the hang this class exists
  // for: there will be no event either way, so it is decided immediately.
  FakeStack refusing;
  refusing.accept_register = false;
  refusing.register_error = -7;
  OmronSubscriptions required;
  required.set_host(&refusing);
  required.add(0x0021, 0x0022, true);
  required.begin(refusing.now);
  assert(required.phase() == OmronSubscriptions::Phase::FAILED);
  assert(refusing.failure_count == 1);

  // The same refusal on an optional target skips it without a single tick, and
  // a run of them does not recurse per target.
  FakeStack skipping;
  skipping.accept_register = false;
  OmronSubscriptions optional_only;
  optional_only.set_host(&skipping);
  for (uint16_t handle = 0x0021; handle < 0x0031; handle += 2)
    optional_only.add(handle, static_cast<uint16_t>(handle + 1), false);
  optional_only.begin(skipping.now);
  assert(optional_only.ready());
  assert(skipping.dropped.size() == 8);
  assert(skipping.failure_count == 0);

  // A machine nobody wired to a host must FAIL, not idle. Idling costs a whole
  // session: no registration, no retry, and the only symptom ten seconds later
  // is the phase timeout blaming the peer for notifications nothing ever asked
  // it for.
  //
  // Failing here is what makes the difference visible - the client's own timeout
  // then has something to report against, and begin() says outright that it is a
  // wiring bug.
  OmronSubscriptions hostless;
  hostless.add(0x0021, 0x0022, true);
  hostless.begin(1000);
  assert(hostless.phase() == OmronSubscriptions::Phase::FAILED);
  assert(!hostless.pending());
  assert(!hostless.ready());
  // And nothing may crash on the events that arrive afterwards.
  hostless.tick(9999);
  hostless.on_descriptor_written(0x0022, true, 0);
  hostless.on_register_result(0x0021, true, 0);
  hostless.resume(9999);
  assert(hostless.phase() == OmronSubscriptions::Phase::FAILED);

  // clear() puts a used machine back where it started, which is what a new
  // session gets.
  FakeStack stack;
  OmronSubscriptions reused;
  reused.set_host(&stack);
  reused.add(0x0021, 0x0022, true);
  reused.begin(stack.now);
  reused.clear();
  assert(reused.empty() && reused.size() == 0);
  assert(reused.phase() == OmronSubscriptions::Phase::IDLE);
  assert(!reused.pending() && !reused.ready());
}
