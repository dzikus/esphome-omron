"""Schema helpers from components/omron/__init__.py, run on the host.

Needs an interpreter with esphome importable:

    python -m unittest discover -s tests/python
"""

import datetime
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "components"))

import esphome.config_validation as cv
import omron


class ProfileUserCount(unittest.TestCase):
    def test_two_user_profile(self):
        self.assertEqual(omron.profile_user_count("hem_7155t_mw3"), 2)

    def test_single_user_profile(self):
        single = next(iter(omron.SINGLE_USER_PROFILES))
        self.assertEqual(omron.profile_user_count(single), 1)

    def test_auto_answers_with_the_ceiling(self):
        self.assertEqual(omron.profile_user_count("auto"), omron.OMRON_MAX_USERS)

    def test_case_is_folded(self):
        single = next(iter(omron.SINGLE_USER_PROFILES))
        self.assertEqual(omron.profile_user_count(single.upper()), 1)

    def test_non_string_answers_with_the_ceiling(self):
        self.assertEqual(omron.profile_user_count(None), omron.OMRON_MAX_USERS)


class ProfileOrAuto(unittest.TestCase):
    def test_auto_survives(self):
        self.assertEqual(omron._profile_or_auto("auto"), "auto")
        self.assertEqual(omron._profile_or_auto("AUTO"), "auto")

    def test_known_profile_resolves(self):
        self.assertIsNotNone(omron._profile_or_auto("hem_7155t_mw3"))

    def test_unknown_profile_is_refused(self):
        with self.assertRaises(cv.Invalid):
            omron._profile_or_auto("hem_0000t")

    def test_the_error_names_profiles_rather_than_only_auto(self):
        with self.assertRaises(cv.Invalid) as caught:
            omron._profile_or_auto("hem_0000t")
        message = str(caught.exception)
        self.assertTrue(any(name in message for name in omron.OMRON_PROFILES))


class BirthDate(unittest.TestCase):
    def test_iso_string(self):
        self.assertEqual(
            omron._birth_date("1948-03-09"),
            {"year": 1948, "month": 3, "day": 9},
        )

    def test_date_object(self):
        self.assertEqual(
            omron._birth_date(datetime.date(1955, 11, 5)),
            {"year": 1955, "month": 11, "day": 5},
        )

    def test_datetime_object_keeps_only_the_date(self):
        self.assertEqual(
            omron._birth_date(datetime.datetime(1955, 11, 5, 13, 30)),
            {"year": 1955, "month": 11, "day": 5},
        )

    def test_before_the_epoch_is_accepted(self):
        self.assertEqual(omron._birth_date("1900-01-01")["year"], 1900)

    def test_the_range_ends_where_the_cuff_byte_does(self):
        self.assertEqual(omron._birth_date("2155-12-31")["year"], 2155)
        with self.assertRaises(cv.Invalid):
            omron._birth_date("1899-12-31")
        with self.assertRaises(cv.Invalid):
            omron._birth_date("2156-01-01")

    def test_not_a_date(self):
        with self.assertRaises(cv.Invalid):
            omron._birth_date("the fifth of november")


class RequireExistingCustomKey(unittest.TestCase):
    def test_classic_profile_without_bindkey_is_refused(self):
        classic = next(iter(omron.CUSTOM_KEY_PROFILES))
        with self.assertRaises(cv.Invalid):
            omron._require_existing_custom_key({"profile": classic})

    def test_classic_profile_with_bindkey_passes(self):
        classic = next(iter(omron.CUSTOM_KEY_PROFILES))
        config = {"profile": classic, "bindkey": "0" * 32}
        self.assertIs(omron._require_existing_custom_key(config), config)

    def test_token_profile_needs_no_bindkey(self):
        config = {"profile": "hem_7155t_mw3"}
        self.assertIs(omron._require_existing_custom_key(config), config)

    def test_auto_needs_no_bindkey(self):
        config = {"profile": "auto"}
        self.assertIs(omron._require_existing_custom_key(config), config)


class UsersMustExistOnThisCuff(unittest.TestCase):
    def setUp(self):
        self.single = next(iter(omron.SINGLE_USER_PROFILES))

    def test_register_as_user_two_on_a_single_user_cuff(self):
        with self.assertRaises(cv.Invalid):
            omron._users_must_exist_on_this_cuff(
                {"profile": self.single, "register_as_user": 2}
            )

    def test_users_block_two_on_a_single_user_cuff(self):
        with self.assertRaises(cv.Invalid):
            omron._users_must_exist_on_this_cuff(
                {"profile": self.single, "users": [{"user": 2}]}
            )

    def test_user_one_is_fine_on_a_single_user_cuff(self):
        config = {"profile": self.single, "users": [{"user": 1}], "register_as_user": 1}
        self.assertIs(omron._users_must_exist_on_this_cuff(config), config)

    def test_two_user_cuff_takes_both(self):
        config = {"profile": "hem_7155t_mw3", "users": [{"user": 1}, {"user": 2}]}
        self.assertIs(omron._users_must_exist_on_this_cuff(config), config)

    def test_auto_defers_to_the_device(self):
        config = {"profile": "auto", "users": [{"user": 2}]}
        self.assertIs(omron._users_must_exist_on_this_cuff(config), config)


class BirthDatesMustHaveAWayOut(unittest.TestCase):
    def test_birth_date_with_no_writer_is_refused(self):
        with self.assertRaises(cv.Invalid):
            omron._birth_dates_must_have_a_way_out(
                {"users": [{"user": 2, "birth_date": {"year": 1948}}]}
            )

    def test_write_birth_date_is_a_way_out(self):
        entry = {"user": 2, "birth_date": {"year": 1948}, "write_birth_date": True}
        config = {"users": [entry]}
        self.assertIs(omron._birth_dates_must_have_a_way_out(config), config)

    def test_being_the_registering_user_is_a_way_out(self):
        config = {
            "register_as_user": 2,
            "users": [{"user": 2, "birth_date": {"year": 1948}}],
        }
        self.assertIs(omron._birth_dates_must_have_a_way_out(config), config)

    def test_the_message_names_every_stranded_user(self):
        with self.assertRaises(cv.Invalid) as caught:
            omron._birth_dates_must_have_a_way_out(
                {
                    "users": [
                        {"user": 1, "birth_date": {"year": 1948}},
                        {"user": 2, "birth_date": {"year": 1955}},
                    ]
                }
            )
        self.assertIn("1, 2", str(caught.exception))

    def test_no_users_block(self):
        config = {}
        self.assertIs(omron._birth_dates_must_have_a_way_out(config), config)


class SameId(unittest.TestCase):
    def test_bare_strings(self):
        self.assertTrue(omron._same_id("cuff", "cuff"))
        self.assertFalse(omron._same_id("cuff", "other"))

    def test_object_carrying_an_id_attribute(self):
        class Ref:
            def __init__(self, name):
                self.id = name

        self.assertTrue(omron._same_id(Ref("cuff"), "cuff"))
        self.assertTrue(omron._same_id(Ref("cuff"), Ref("cuff")))
        self.assertFalse(omron._same_id(Ref("cuff"), Ref("other")))


if __name__ == "__main__":
    unittest.main()
