// Older readings on their way out, and the flash mark that says how far they got.
//
// These exist so that "did that event actually go out" is answered by a test
// rather than by reasoning about when NVS writes happen.

#include <cassert>
#include <cstdint>
#include <vector>

#include "omron_history_queue.h"
#include "test_support.h"

using namespace esphome::omron;

namespace {

class FakeConsumer : public OmronHistoryQueueHost {
 public:
  uint32_t now{1000};
  std::vector<uint16_t> emitted;
  std::vector<std::pair<uint8_t, int64_t>> saved;

  uint32_t history_now_ms() override { return this->now; }
  void history_emit(const HistoryEvent &event) override { this->emitted.push_back(event.slot); }
  void history_save_watermark(uint8_t user_index, int64_t epoch) override {
    this->saved.emplace_back(user_index, epoch);
  }
};

HistoryEvent slot(uint16_t number, uint8_t user = 0) {
  HistoryEvent event{};
  event.slot = number;
  event.user_index = user;
  return event;
}

// Runs the clock forward until the queue says it has nothing left.
int drain(OmronHistoryQueue &queue, FakeConsumer &consumer, int max_passes = 500) {
  int passes = 0;
  while (passes < max_passes && queue.tick(consumer.now)) {
    consumer.now += OmronHistoryQueue::EMIT_INTERVAL_MS;
    passes++;
  }
  return passes;
}

}  // namespace

void test_history_queue_saves_the_watermark_only_once_it_is_earned() {
  FakeConsumer consumer;
  OmronHistoryQueue queue;
  queue.set_host(&consumer);

  for (uint16_t i = 1; i <= 5; i++)
    assert(queue.push(slot(i)));
  queue.note_watermark(0, 5000);

  // This is the invariant. While anything is queued the mark stays in RAM: a
  // save here would record five events as sent, and a reboot before they went
  // out would lose them with no symptom - nobody looks for readings they do not
  // know exist.
  assert(queue.watermark_pending());
  for (int i = 0; i < 4; i++) {
    queue.tick(consumer.now);
    consumer.now += OmronHistoryQueue::EMIT_INTERVAL_MS;
    assert(consumer.saved.empty());
  }
  assert(!consumer.emitted.empty() && consumer.emitted.size() < 5);

  drain(queue, consumer);
  assert(consumer.emitted.size() == 5);
  assert(consumer.saved.size() == 1);
  assert(consumer.saved[0].first == 0 && consumer.saved[0].second == 5000);
  assert(!queue.watermark_pending());

  // Oldest first, whatever order a consumer appends them in.
  for (size_t i = 0; i < consumer.emitted.size(); i++)
    assert(consumer.emitted[i] == static_cast<uint16_t>(i + 1));

  // Saved once, not once per pass.
  drain(queue, consumer);
  assert(consumer.saved.size() == 1);
}

void test_history_queue_paces_and_bounds_itself() {
  FakeConsumer consumer;
  OmronHistoryQueue queue;
  queue.set_host(&consumer);

  assert(queue.push(slot(1)));
  assert(queue.push(slot(2)));
  queue.tick(consumer.now);
  assert(consumer.emitted.size() == 1);

  // One per interval. Firing a whole ring in one loop pass is what this spacing
  // exists to prevent, and the API connection is what pays for it.
  consumer.now += OmronHistoryQueue::EMIT_INTERVAL_MS - 1;
  queue.tick(consumer.now);
  assert(consumer.emitted.size() == 1);
  consumer.now += 1;
  queue.tick(consumer.now);
  assert(consumer.emitted.size() == 2);

  // A queue nobody drains must not grow without bound. Home Assistant being away
  // is the case: readings keep arriving and nothing consumes them.
  FakeConsumer stuck;
  OmronHistoryQueue bounded;
  bounded.set_host(&stuck);
  size_t accepted = 0;
  for (size_t i = 0; i < OmronHistoryQueue::CAPACITY * 2; i++) {
    if (bounded.push(slot(static_cast<uint16_t>(i))))
      accepted++;
  }
  assert(accepted == OmronHistoryQueue::CAPACITY);
  assert(bounded.size() == OmronHistoryQueue::CAPACITY);
  assert(bounded.room() == 0);

  // room() is what a session asks before it decodes, so it can stop reading
  // rather than build events it would have to throw away.
  FakeConsumer fresh;
  OmronHistoryQueue counted;
  counted.set_host(&fresh);
  assert(counted.room() == OmronHistoryQueue::CAPACITY);
  counted.push(slot(1));
  assert(counted.room() == OmronHistoryQueue::CAPACITY - 1);
}

void test_history_queue_watermark_only_moves_forward() {
  FakeConsumer consumer;
  OmronHistoryQueue queue;
  queue.set_host(&consumer);

  queue.note_watermark(0, 5000);
  queue.tick(consumer.now);
  assert(consumer.saved.size() == 1 && consumer.saved[0].second == 5000);

  // A session that read less deeply than an earlier one must not walk the mark
  // back: everything between would be sent a second time, and these events carry
  // real measurements to somebody's history.
  queue.note_watermark(0, 4000);
  assert(!queue.watermark_pending());
  queue.tick(consumer.now);
  assert(consumer.saved.size() == 1);

  // Equal is not forward either.
  queue.note_watermark(0, 5000);
  queue.tick(consumer.now);
  assert(consumer.saved.size() == 1);

  // Forward is.
  queue.note_watermark(0, 6000);
  queue.tick(consumer.now);
  assert(consumer.saved.size() == 2 && consumer.saved[1].second == 6000);

  // Per user, and a slot that does not exist is ignored rather than written past
  // the end of the array.
  queue.note_watermark(1, 7000);
  queue.note_watermark(OmronHistoryQueue::USER_SLOTS, 9999);
  queue.note_watermark(200, 9999);
  queue.tick(consumer.now);
  assert(consumer.saved.size() == 3);
  assert(consumer.saved[2].first == 1 && consumer.saved[2].second == 7000);
}

void test_history_queue_edge_cases() {
  // Without a host nothing may be attempted and nothing may crash. Unlike the
  // subscription queue this one is not fatal when unwired: it holds readings,
  // and refusing to hold them would lose more than it saved.
  OmronHistoryQueue hostless;
  hostless.push(slot(1));
  assert(!hostless.tick(9999));
  assert(hostless.size() == 1);

  // An empty queue with nothing pending has no work and says so, which is what
  // lets the caller put its loop back to sleep.
  FakeConsumer consumer;
  OmronHistoryQueue queue;
  queue.set_host(&consumer);
  assert(!queue.tick(consumer.now));
  assert(consumer.emitted.empty() && consumer.saved.empty());

  // A mark restored from flash is not written straight back. It is already
  // durable, and treating it as new would spend a flash write on every boot.
  FakeConsumer booted;
  OmronHistoryQueue restored;
  restored.set_host(&booted);
  restored.seed_watermark(0, 4242);
  assert(restored.watermark(0) == 4242);
  assert(!restored.watermark_pending());
  restored.tick(booted.now);
  assert(booted.saved.empty());
  // And it still blocks a backwards move, so a seeded node does not resend.
  restored.note_watermark(0, 4000);
  restored.tick(booted.now);
  assert(booted.saved.empty());

  // A mark noted with an empty queue is durable on the next pass, with no event
  // needed to carry it.
  queue.note_watermark(0, 1234);
  assert(queue.tick(consumer.now) == false);
  assert(consumer.saved.size() == 1);

  // A full ring drains in the time the spacing implies rather than all at once.
  FakeConsumer bulk;
  OmronHistoryQueue full;
  full.set_host(&bulk);
  for (uint16_t i = 0; i < 60; i++)
    full.push(slot(i));
  const uint32_t started = bulk.now;
  drain(full, bulk);
  assert(bulk.emitted.size() == 60);
  // Stated as elapsed time rather than as loop passes: the property is that
  // sixty readings are spread over seven seconds instead of arriving as one
  // burst, and that holds however often the caller ticks.
  assert(bulk.now - started >= 59 * OmronHistoryQueue::EMIT_INTERVAL_MS);
}
