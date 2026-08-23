// Saying which cuff is on the other end, from the string it reports about
// itself.
//
// The failure this guards against is silent: a profile with the wrong
// addresses does not error, it reads a different EEPROM area and publishes
// numbers in the right range. A false negative costs the only warning there
// is, and a false positive teaches whoever owns the node to ignore that
// warning, which costs the same thing one session later.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include "omron_model_id.h"
#include "omron_profiles.h"
#include "omron_trade_names.h"
#include "test_support.h"

using namespace esphome::omron;

namespace {

ModelIdentification identify(const std::string &reported, OmronStack stack = OmronStack::UNKNOWN) {
  return identify_model(reported, stack);
}

}  // namespace

void test_model_id_does_not_cry_wolf_on_the_verified_cuff() {
  // "X4 Smart" is what DIS 0x2A24 carries on this family. Not a model id - a
  // trade name, and one Omron gives to two devices with different memory maps.
  //
  // hem_7155t_mw3 is the only HARDWARE_VERIFIED entry in the catalog, while the
  // trade name resolves to HEM-7155T-K4, a *different* profile. Comparing
  // identity by name would report a mismatch against the best-evidenced entry
  // there is, and that is exactly the kind of warning that gets configured away.
  const OmronProfile &configured = get_profile(OmronProfileId::HEM_7155T_MW3);

  const ModelIdentification modern = identify("X4 Smart", OmronStack::MODERN);
  assert(modern.identity == ModelIdentity::RESOLVED);
  assert(modern.candidates == 2);
  assert(modern.known_candidates == 2);
  assert(modern.profile != nullptr);

  // The trade name reaches HEM-7155T-K4 and cannot reach HEM-7155T-MW3 at all,
  // since no catalogued trade name lists MW3. Detection still has to answer with
  // the best-evidenced entry, because the two address memory identically and the
  // confidence level is a statement about that layout.
  //
  // Without this, `profile: auto` selects the unverified twin of a profile whose
  // layout carries the stronger evidence.
  assert(modern.profile == &configured);
  assert(modern.profile->confidence == OmronProfileConfidence::HARDWARE_VERIFIED);
  assert(verify_configured_profile(configured, modern) == ProfileVerdict::CONFIRMED);

  // And the twin it resolved through is still the same memory, so configuring
  // either one is correct.
  const OmronProfile &k4 = get_profile(OmronProfileId::HEM_7155T_K4);
  assert(same_record_memory_map(configured, k4));
  assert(verify_configured_profile(k4, modern) == ProfileVerdict::COMPATIBLE);

  // And the addresses really are the verified entry's, rather than the two
  // profiles being called compatible because the comparison is toothless.
  assert(modern.profile->settings_read_address == 0x0260);
  assert(modern.profile->users[0].record_start_address == 0x02E8);
  assert(configured.settings_read_address == 0x0260);

  // A profile compared against itself is confirmed, not merely compatible.
  const ModelIdentification exact = identify("HEM-7155T_ESL1");
  assert(exact.identity == ModelIdentity::EXACT);
  assert(exact.profile == &configured);
  assert(verify_configured_profile(configured, exact) == ProfileVerdict::CONFIRMED);
}

void test_model_id_catches_the_wrong_half_of_a_shared_trade_name() {
  // Same string, other stack. "X4 Smart" is also HEM-7155T_ESL, which is the
  // classic transport and reads settings at 0x0010 and records at 0x0098 - a
  // different part of the chip entirely.
  const ModelIdentification classic = identify("X4 Smart", OmronStack::CLASSIC);
  assert(classic.identity == ModelIdentity::RESOLVED);
  assert(classic.profile != nullptr);
  assert(classic.profile->settings_read_address == 0x0010);
  assert(classic.profile->users[0].record_start_address == 0x0098);

  // Configured modern, cuff answering on the classic stack: this is the whole
  // point of the file.
  const OmronProfile &modern_profile = get_profile(OmronProfileId::HEM_7155T_MW3);
  assert(verify_configured_profile(modern_profile, classic) == ProfileVerdict::MISMATCH);

  // And the reverse, so the verdict is not just "anything that is not the
  // configured profile is a mismatch".
  const OmronProfile &classic_profile = get_profile(OmronProfileId::HEM_7155T);
  assert(verify_configured_profile(classic_profile, classic) == ProfileVerdict::CONFIRMED);
  assert(verify_configured_profile(classic_profile, identify("X4 Smart", OmronStack::MODERN)) ==
         ProfileVerdict::MISMATCH);

  // Without a stack there is nothing to separate them, and the answer must be
  // "I do not know" rather than either half.
  const ModelIdentification blind = identify("X4 Smart", OmronStack::UNKNOWN);
  assert(blind.identity == ModelIdentity::AMBIGUOUS);
  assert(blind.profile == nullptr);
  assert(verify_configured_profile(modern_profile, blind) == ProfileVerdict::UNVERIFIED);
  assert(verify_configured_profile(classic_profile, blind) == ProfileVerdict::UNVERIFIED);
}

void test_model_id_refuses_to_guess() {
  const OmronProfile &configured = get_profile(OmronProfileId::HEM_7155T_MW3);

  // A string that means nothing here is not evidence against the configuration.
  // Anything else would make every unlisted cuff look misconfigured.
  for (const char *nonsense : {"", "   ", "Blood Pressure Monitor", "HEM-9999T", "X4"}) {
    const ModelIdentification unknown = identify(nonsense);
    assert(unknown.identity == ModelIdentity::UNKNOWN);
    assert(unknown.profile == nullptr);
    assert(verify_configured_profile(configured, unknown) == ProfileVerdict::UNVERIFIED);
  }
  assert(identify_model({}, OmronStack::MODERN).identity == ModelIdentity::UNKNOWN);

  // "M7 Intelli IT" is the one trade name the stack cannot save: both of its
  // models are classic and they disagree about where records live. It has to
  // come back ambiguous on every stack, including the right one.
  for (OmronStack stack : {OmronStack::UNKNOWN, OmronStack::CLASSIC, OmronStack::MODERN}) {
    const ModelIdentification ambiguous = identify("M7 Intelli IT", stack);
    assert(ambiguous.identity == ModelIdentity::AMBIGUOUS);
    assert(ambiguous.profile == nullptr);
    assert(ambiguous.candidates == 2);
    assert(verify_configured_profile(configured, ambiguous) == ProfileVerdict::UNVERIFIED);
  }

  // A trade name whose candidates are regional model ids of one cuff is not
  // ambiguous: they share a map, so the name answers the only question being
  // asked. HCR-1901T2 is HEM-1026T2-AJC and HEM-1026T2-AKA, one profile.
  const ModelIdentification regional = identify("HCR-1901T2");
  assert(regional.identity == ModelIdentity::RESOLVED);
  assert(regional.candidates == 2);
  assert(regional.known_candidates == 2);
  assert(regional.profile != nullptr);
}

void test_model_id_prefers_a_model_id_to_a_trade_name() {
  // Some strings are both. "HEM-7156T" is a variant name in the catalog and also
  // the trade name Omron gives three regional variants of that cuff; "HEM-7600T"
  // is a variant name and a trade name covering two variants that disagree about
  // ring depth.
  //
  // The model id wins, deliberately: it is the more specific of the two claims,
  // where the trade name is marketing text that happens to collide with it.
  // Asserted so it stays a decision rather than an artefact of lookup order.
  const ModelIdentification collided = identify("HEM-7156T");
  assert(collided.identity == ModelIdentity::EXACT);
  assert(collided.candidates == 1);
  assert(profile_for_model("HEM-7156T") == collided.profile);

  // The one where it costs something: as a trade name this is ambiguous between
  // a 90-slot and a 100-slot ring, and the model id answers 90 without hedging.
  // The cost of being wrong in this direction is reading fewer old records than
  // exist, not reading the wrong bytes.
  const ModelIdentification deep = identify("HEM-7600T");
  assert(deep.identity == ModelIdentity::EXACT);
  assert(deep.profile != nullptr);
  assert(deep.profile->users[0].record_count == 90);

  // The exact path does NOT prefer a better-evidenced entry over the same map,
  // and that asymmetry is deliberate. A trade name is ambiguous by construction,
  // so choosing the most proven entry among identical layouts fills in an
  // ambiguity. A model id is the cuff naming itself, and answering with a
  // different name would contradict a first-party statement to gain a label.
  const ModelIdentification exact_k4 = identify("HEM-7155T_K4-ESL", OmronStack::MODERN);
  assert(exact_k4.identity == ModelIdentity::EXACT);
  assert(exact_k4.profile == &get_profile(OmronProfileId::HEM_7155T_K4));
  assert(exact_k4.profile->confidence != OmronProfileConfidence::HARDWARE_VERIFIED);
  // Same map as the verified twin, so configuring either is still correct.
  assert(same_record_memory_map(*exact_k4.profile, get_profile(OmronProfileId::HEM_7155T_MW3)));
}

void test_model_id_tolerates_what_comes_off_the_wire() {
  // DIS is a fixed-length field and arrives padded. Identification is handed the
  // raw value on purpose, so this has to hold without the caller tidying up.
  const char padded[] = "X4 Smart\0\0\0";
  const ModelIdentification from_padding = identify_model({padded, sizeof(padded) - 1}, OmronStack::MODERN);
  assert(from_padding.identity == ModelIdentity::RESOLVED);

  const std::string spaced = "X4 Smart   ";
  assert(identify(spaced, OmronStack::MODERN).identity == ModelIdentity::RESOLVED);

  // Whether any cuff varies the casing is unknown. Folding costs nothing and
  // cannot collide - the generator refuses to emit a table where two names
  // differ by case alone.
  assert(identify("x4 smart", OmronStack::MODERN).identity == ModelIdentity::RESOLVED);
  assert(identify("X4 SMART", OmronStack::MODERN).identity == ModelIdentity::RESOLVED);

  // Trailing padding is trimmed; a longer name that merely starts the same is
  // still a different device.
  assert(identify("X4 Smart Plus", OmronStack::MODERN).identity == ModelIdentity::UNKNOWN);
}

void test_model_id_table_is_wired_to_the_catalog() {
  // Every name the table ships resolves to at least one profile. The generator
  // drops names whose models this catalog does not carry, so an entry that
  // resolves to nothing means the table and the catalog have drifted apart -
  // which is what happens when a profile is renamed or an alias moves.
  constexpr size_t TRADE_NAME_COUNT = std::size(TRADE_NAMES);
  assert(TRADE_NAME_COUNT > 100);
  size_t resolved = 0, ambiguous = 0;
  for (const OmronTradeName &entry : TRADE_NAMES) {
    const char *name = entry.name;
    assert(name != nullptr && name[0] != '\0');
    const ModelIdentification identification = identify_model(name, OmronStack::UNKNOWN);
    assert(identification.known_candidates > 0 || identification.identity == ModelIdentity::EXACT);
    if (identification.identity == ModelIdentity::RESOLVED || identification.identity == ModelIdentity::EXACT)
      resolved++;
    else if (identification.identity == ModelIdentity::AMBIGUOUS)
      ambiguous++;
  }
  // Counted, not asserted exactly: the table is regenerated from Omron's catalog
  // and a new model may arrive. What must not happen is the resolving majority
  // quietly collapsing.
  assert(resolved > TRADE_NAME_COUNT - 12);
  assert(ambiguous <= 8);

  // same_record_memory_map is an equivalence over the fields that decide what
  // gets read, not an identity check on profiles.
  const OmronProfile &mw3 = get_profile(OmronProfileId::HEM_7155T_MW3);
  const OmronProfile &k4 = get_profile(OmronProfileId::HEM_7155T_K4);
  const OmronProfile &classic = get_profile(OmronProfileId::HEM_7155T);
  assert(same_record_memory_map(mw3, mw3));
  assert(same_record_memory_map(mw3, k4));  // different entries, one map
  assert(!same_record_memory_map(mw3, classic));
  assert(!same_record_memory_map(k4, classic));
}

void test_model_id_map_comparison_looks_at_every_field() {
  // The pair above differs in almost every address at once, so it cannot show
  // that any *particular* field is compared - switching one comparison off
  // leaves the answer unchanged and the test still green.
  //
  // One field at a time against an otherwise identical copy is the only way to
  // ask the question. A field added to the profile and forgotten here will not
  // be caught by this, which is why the list is written out rather than looped.
  const OmronProfile &base = get_profile(OmronProfileId::HEM_7155T_MW3);
  assert(base.user_count >= 2);

  struct Perturbation {
    const char *what;
    void (*apply)(OmronProfile &);
  };
  static const Perturbation PERTURBATIONS[] = {
      {"settings read base", [](OmronProfile &p) { p.settings_read_address ^= 0x0100; }},
      {"settings write base", [](OmronProfile &p) { p.settings_write_address ^= 0x0100; }},
      {"index region size", [](OmronProfile &p) { p.settings_index_region_size ^= 0x08; }},
      {"user block size", [](OmronProfile &p) { p.user_block_size ^= 0x04; }},
      {"record size", [](OmronProfile &p) { p.record_size ^= 0x02; }},
      {"record format",
       [](OmronProfile &p) {
         p.record_format = p.record_format == RecordFormat::CLASSIC_VITAL_14 ? RecordFormat::PLAIN_DATE_VITAL
                                                                             : RecordFormat::CLASSIC_VITAL_14;
       }},
      {"byte order",
       [](OmronProfile &p) { p.byte_order = p.byte_order == ByteOrder::LITTLE ? ByteOrder::BIG : ByteOrder::LITTLE; }},
      {"cursor byte order",
       [](OmronProfile &p) {
         p.cursor_byte_order =
             p.cursor_byte_order == CursorByteOrder::LITTLE ? CursorByteOrder::BIG : CursorByteOrder::LITTLE;
       }},
      {"record sequence offset", [](OmronProfile &p) { p.record_sequence_offset ^= 0x04; }},
      {"user count", [](OmronProfile &p) { p.user_count = 1; }},
      {"record start address", [](OmronProfile &p) { p.users[0].record_start_address ^= 0x0100; }},
      {"ring depth", [](OmronProfile &p) { p.users[0].record_count ^= 0x04; }},
      {"write cursor offset", [](OmronProfile &p) { p.users[0].write_cursor_offset ^= 0x02; }},
      {"unread counter offset", [](OmronProfile &p) { p.users[0].unread_counter_offset ^= 0x02; }},
      {"write cursor mask", [](OmronProfile &p) { p.users[0].write_cursor_mask ^= 0x0080; }},
      {"slot index bias", [](OmronProfile &p) { p.users[0].slot_index_bias ^= 0x01; }},
      // The second user's block matters as much as the first: a profile that
      // agreed about user 1 and not user 2 would publish one person's readings
      // and somebody else's.
      {"second user record start", [](OmronProfile &p) { p.users[1].record_start_address ^= 0x0100; }},
      {"second user ring depth", [](OmronProfile &p) { p.users[1].record_count ^= 0x04; }},
  };

  for (const Perturbation &perturbation : PERTURBATIONS) {
    OmronProfile altered = base;
    perturbation.apply(altered);
    if (same_record_memory_map(base, altered)) {
      std::printf("    same_record_memory_map ignores %s\n", perturbation.what);
      assert(false);
    }
  }

  // An untouched copy still compares equal, so the loop above is not simply
  // reporting that a copy is never equal to its original.
  const OmronProfile copy = base;
  assert(same_record_memory_map(base, copy));

  // And a field that cannot misread anything is deliberately not compared. The
  // transfer block only frames the reads: wrong, it produces a short reply or
  // none, never a plausible number.
  OmronProfile reframed = base;
  reframed.transmission_block_size = static_cast<uint8_t>(base.transmission_block_size / 2);
  assert(same_record_memory_map(base, reframed));
}

void test_model_id_config_key_is_derived_not_listed() {
  // The whole point of detection, for most people, is finding out what to put in
  // the yaml. That answer has to be the exact key the schema accepts, and the
  // schema's table lives in Python - so this is a derivation from the model id
  // rather than a second list that can drift out of step with it.
  expect_string(profile_config_key(get_profile(OmronProfileId::HEM_7155T_MW3)).c_str(), "hem_7155t_mw3");
  expect_string(profile_config_key(get_profile(OmronProfileId::HEM_7155T_K4)).c_str(), "hem_7155t_k4");
  expect_string(profile_config_key(get_profile(OmronProfileId::HEM_7155T)).c_str(), "hem_7155t");

  // Every profile must produce something a yaml key could be: the schema matches
  // exactly, so a stray character is a key nobody can type.
  for (size_t i = 0; i < profile_count(); i++) {
    const OmronProfile *profile = profile_at(i);
    assert(profile != nullptr);
    const std::string key = profile_config_key(*profile);
    assert(!key.empty());
    for (char value : key)
      assert((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_');
    // Round trip: the key differs from the model id only in case and dashes, so
    // the model has to be recoverable by eye. A derivation that dropped
    // characters would still pass the character test above.
    assert(key.size() == std::strlen(profile->model));
  }
}

void test_model_id_strings() {
  expect_string(model_identity_to_string(ModelIdentity::UNKNOWN), "unrecognised");
  expect_string(model_identity_to_string(ModelIdentity::EXACT), "model id");
  expect_string(model_identity_to_string(ModelIdentity::RESOLVED), "trade name");
  expect_string(model_identity_to_string(ModelIdentity::AMBIGUOUS), "ambiguous trade name");
}
