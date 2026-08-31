"""The codegen layer's copies of the profile catalog, against each other.

components/omron/__init__.py keeps three sets that restate what the C++ catalog
knows, because the schema answers at `esphome config` time and the catalog is
not readable from there. Each can drift on its own: a profile missing from
OMRON_PROFILES cannot be configured at all, one wrongly in SINGLE_USER_PROFILES
refuses `user: 2` on a cuff that has one, and one missing from
CUSTOM_KEY_PROFILES accepts a config with no bindkey and then cannot unlock.

    python -m unittest discover -s tests/python
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "components"))

import omron


class ProfileSets(unittest.TestCase):
    def test_custom_key_profiles_are_configurable(self):
        self.assertLessEqual(set(omron.CUSTOM_KEY_PROFILES), set(omron.OMRON_PROFILES))

    def test_single_user_profiles_are_configurable(self):
        self.assertLessEqual(set(omron.SINGLE_USER_PROFILES), set(omron.OMRON_PROFILES))

    def test_auto_is_not_a_profile_key(self):
        self.assertNotIn(omron.PROFILE_AUTO, omron.OMRON_PROFILES)

    def test_keys_are_lowercase(self):
        for name in omron.OMRON_PROFILES:
            self.assertEqual(name, name.lower(), name)
        for name in omron.CUSTOM_KEY_PROFILES:
            self.assertEqual(name, name.lower(), name)
        for name in omron.SINGLE_USER_PROFILES:
            self.assertEqual(name, name.lower(), name)

    def test_unsupported_is_not_offered(self):
        self.assertNotIn("unsupported", omron.OMRON_PROFILES)

    def test_the_hardware_verified_profile_is_offered(self):
        self.assertIn("hem_7155t_mw3", omron.OMRON_PROFILES)

    def test_every_profile_resolves_to_a_distinct_enum_member(self):
        values = [str(value) for value in omron.OMRON_PROFILES.values()]
        self.assertEqual(len(values), len(set(values)))


class UserCeiling(unittest.TestCase):
    def test_single_user_profiles_answer_one(self):
        for name in omron.SINGLE_USER_PROFILES:
            self.assertEqual(omron.profile_user_count(name), 1, name)

    def test_every_other_profile_answers_the_ceiling(self):
        for name in omron.OMRON_PROFILES:
            if name in omron.SINGLE_USER_PROFILES:
                continue
            count = omron.profile_user_count(name)
            self.assertEqual(count, omron.OMRON_MAX_USERS, name)

    def test_the_ceiling_matches_the_entity_slot_count(self):
        self.assertEqual(omron.OMRON_MAX_USERS, 2)


if __name__ == "__main__":
    unittest.main()
