#include "omron_model_id.h"

#include <array>
#include <cstring>
#include <iterator>
#include <tuple>

#include "omron_trade_names.h"

namespace esphome::omron {

namespace {

constexpr size_t TRADE_NAME_COUNT = std::size(TRADE_NAMES);

// DIS strings are fixed-length fields, so they arrive padded. The client already
// trims what it publishes, but this function is given the raw value on purpose:
// an identification that only worked on pre-trimmed input would be one more
// thing a caller could get wrong silently.
std::string_view trimmed_value(std::string_view text) {
  while (!text.empty() && (text.back() == '\0' || text.back() == ' '))
    text.remove_suffix(1);
  return text;
}

// ASCII only, and hand-rolled rather than std::toupper: that one takes an int
// whose value must be representable as unsigned char, and passing it a plain
// char is undefined for anything above 0x7F. Model names are ASCII, but the
// bytes off a GATT read are whatever the peer sent.
char fold(char value) {
  return (value >= 'a' && value <= 'z') ? static_cast<char>(value - 'a' + 'A') : value;
}

bool equals_folded(const char *candidate, std::string_view text) {
  if (candidate == nullptr || std::strlen(candidate) != text.size())
    return false;
  for (size_t i = 0; i < text.size(); i++) {
    if (fold(candidate[i]) != fold(text[i]))
      return false;
  }
  return true;
}

OmronStack stack_of(const OmronProfile &profile) {
  if (profile.gatt == nullptr)
    return OmronStack::UNKNOWN;
  return profile.gatt == &OMRON_CLASSIC_GATT ? OmronStack::CLASSIC : OmronStack::MODERN;
}

// Among the profiles that address memory identically, the one with the most
// evidence behind it.
//
// Needed because a trade name resolves through the public model catalog, which
// does not contain every entry this component ships: "X4 Smart" reaches
// HEM-7155T_K4-ESL but has no route to HEM-7155T-MW3, whose alias appears in no
// catalog at all. Detection on the one measured cuff therefore names the
// unverified twin of the profile that was measured on it.
//
// Preferring the better-evidenced entry is sound only because the map is
// identical: confidence is a statement about a memory layout, not a model
// number. Where the maps differ, nothing is transferred and nothing preferred.
const OmronProfile *best_evidence_for_map(const OmronProfile &resolved) {
  const OmronProfile *best = &resolved;
  for (size_t i = 0; i < profile_count(); i++) {
    const OmronProfile *candidate = profile_at(i);
    if (candidate == nullptr || candidate == best)
      continue;
    if (!same_record_memory_map(*best, *candidate))
      continue;
    // Strictly greater, so the resolved entry wins every tie and the answer does
    // not depend on catalog order.
    if (candidate->confidence > best->confidence)
      best = candidate;
  }
  return best;
}

const OmronTradeName *trade_name_for(std::string_view reported) {
  for (const OmronTradeName &entry : TRADE_NAMES) {
    if (equals_folded(entry.name, reported))
      return &entry;
  }
  return nullptr;
}

}  // namespace

bool same_record_memory_map(const OmronProfile &a, const OmronProfile &b) {
  // Everything that decides which bytes are read and what they mean. Left out
  // on purpose: transmission_block_size, which only frames the reads and whose
  // being wrong shows up as a short or failed reply rather than as a plausible
  // number; and everything about bonding, which cannot misread anything.
  if (a.settings_read_address != b.settings_read_address || a.settings_write_address != b.settings_write_address ||
      a.settings_index_region_size != b.settings_index_region_size || a.user_block_size != b.user_block_size)
    return false;
  if (a.record_size != b.record_size || a.record_format != b.record_format || a.byte_order != b.byte_order ||
      a.cursor_byte_order != b.cursor_byte_order || a.record_sequence_offset != b.record_sequence_offset)
    return false;
  if (a.user_count != b.user_count)
    return false;
  for (uint8_t user = 0; user < a.user_count && user < OMRON_MAX_USERS; user++) {
    const OmronUserMemoryLayout &left = a.users[user];
    const OmronUserMemoryLayout &right = b.users[user];
    if (left.record_start_address != right.record_start_address || left.record_count != right.record_count ||
        left.write_cursor_offset != right.write_cursor_offset ||
        left.unread_counter_offset != right.unread_counter_offset ||
        left.write_cursor_mask != right.write_cursor_mask || left.slot_index_bias != right.slot_index_bias)
      return false;
  }
  return true;
}

ModelIdentification identify_model(std::string_view reported, OmronStack stack) {
  ModelIdentification result{};
  const std::string_view trimmed = trimmed_value(reported);
  if (trimmed.empty())
    return result;

  // A model id beats everything else, and costs one pass over a table we ship
  // regardless. Case-sensitive, because that is what the catalog lookup is
  // everywhere else in this component; a cuff reporting a model id in some other
  // case falls through to UNKNOWN, which is a miss rather than a wrong answer.
  if (const OmronProfile *exact = profile_for_model(trimmed)) {
    result.identity = ModelIdentity::EXACT;
    result.profile = exact;
    result.candidates = 1;
    result.known_candidates = 1;
    return result;
  }

  const OmronTradeName *trade = trade_name_for(trimmed);
  if (trade == nullptr)
    return result;

  // Gather the candidates this catalog actually knows. A trade name whose models
  // we all lack tells us nothing, and must not read as a contradiction of what
  // the user configured.
  // Sized from the table's own row width. Written as a literal it is a second
  // copy of a number the generator owns, and a table that ever carries a fourth
  // model would write past the end of this.
  std::array<const OmronProfile *, std::tuple_size_v<decltype(OmronTradeName::models)>> known{};
  for (const char *model : trade->models) {
    if (model == nullptr)
      continue;
    result.candidates++;
    if (const OmronProfile *profile = profile_for_model(model)) {
      known[result.known_candidates] = profile;
      result.known_candidates++;
    }
  }
  // Unreachable with the table as shipped: the generator drops any trade name
  // whose models this catalog does not carry, and the catalog test asserts every
  // remaining entry resolves. Said plainly because a mutation to this line
  // survives the suite - it is a guard against the table and the catalog
  // drifting apart later, not covered behaviour, and the invariant that makes it
  // dead is enforced in two places that are covered.
  if (result.known_candidates == 0)
    return result;

  // One memory map among the candidates is an answer whatever their names are:
  // three model ids of one cuff sold in three regions are one cuff here.
  bool agree = true;
  for (uint8_t i = 1; i < result.known_candidates; i++) {
    if (!same_record_memory_map(*known[0], *known[i])) {
      agree = false;
      break;
    }
  }
  if (agree) {
    result.identity = ModelIdentity::RESOLVED;
    result.profile = best_evidence_for_map(*known[0]);
    return result;
  }

  // They disagree, so the stack gets a turn. Six trade names in the catalog are
  // one cuff's classic generation and its modern one under a single marketing
  // name, and discovery has already told us which of the two is on the link.
  if (stack != OmronStack::UNKNOWN) {
    const OmronProfile *matched = nullptr;
    bool unique = true;
    for (uint8_t i = 0; i < result.known_candidates; i++) {
      if (stack_of(*known[i]) != stack)
        continue;
      if (matched == nullptr) {
        matched = known[i];
      } else if (!same_record_memory_map(*matched, *known[i])) {
        unique = false;
      }
    }
    if (matched != nullptr && unique) {
      result.identity = ModelIdentity::RESOLVED;
      result.profile = best_evidence_for_map(*matched);
      return result;
    }
  }

  result.identity = ModelIdentity::AMBIGUOUS;
  return result;
}

ProfileVerdict verify_configured_profile(const OmronProfile &configured, const ModelIdentification &identification) {
  if (identification.profile == nullptr)
    return ProfileVerdict::UNVERIFIED;
  if (identification.profile == &configured)
    return ProfileVerdict::CONFIRMED;
  return same_record_memory_map(configured, *identification.profile) ? ProfileVerdict::COMPATIBLE
                                                                     : ProfileVerdict::MISMATCH;
}

const char *model_identity_to_string(ModelIdentity identity) {
  switch (identity) {
    case ModelIdentity::EXACT:
      return "model id";
    case ModelIdentity::RESOLVED:
      return "trade name";
    case ModelIdentity::AMBIGUOUS:
      return "ambiguous trade name";
    case ModelIdentity::UNKNOWN:
      return "unrecognised";
  }
  return "unrecognised";
}

std::string profile_config_key(const OmronProfile &profile) {
  std::string key;
  if (profile.model == nullptr)
    return key;
  for (const char *cursor = profile.model; *cursor != '\0'; cursor++) {
    const char value = *cursor;
    if (value == '-') {
      key.push_back('_');
    } else if (value >= 'A' && value <= 'Z') {
      key.push_back(static_cast<char>(value - 'A' + 'a'));
    } else {
      key.push_back(value);
    }
  }
  return key;
}

}  // namespace esphome::omron
