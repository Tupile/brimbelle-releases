"""Tests of the single stream rules. Run: python3 test/addon/test_single_stream.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "addon", "ma_single_stream"))
from single_stream import Arbiter, plays_provider

BRIM, SALON, SDJ, PHONE = "brim", "salon", "sdj", "phone"
ALL = [BRIM, SALON, SDJ, PHONE]


def queue(state="idle", provider="ytmusic--abc"):
    return {"state": state, "current_item": {"streamdetails": {"provider": provider}}}


def players(**over):
    out = {p: {"group_members": [], "synced_to": None, "can_group_with": [o for o in ALL if o != p]} for p in ALL}
    for pid, fields in over.items():
        out[pid].update(fields)
    return out


class Rules(unittest.TestCase):
    def setUp(self):
        self.a = Arbiter(BRIM)
        self.q = {p: queue() for p in ALL}

    def run_at(self, now, pl=None):
        return self.a.evaluate(self.q, pl or players(), now)

    def test_provider_detection(self):
        self.assertTrue(plays_provider(queue("playing")))
        self.assertFalse(plays_provider(queue("paused")))
        self.assertFalse(plays_provider(queue("playing", "filesystem_local--x")))
        library_track = {"state": "playing", "current_item": {"streamdetails": None, "media_item": {
            "provider_mappings": [{"provider_domain": "ytmusic"}]}}}
        self.assertTrue(plays_provider(library_track))

    def test_single_stream_is_left_alone(self):
        self.q[SALON] = queue("playing")
        self.assertEqual(self.run_at(0), [])

    def test_other_sources_are_left_alone(self):
        self.q[SALON] = queue("playing")
        self.q[SDJ] = queue("playing", "filesystem_local--x")
        self.q[PHONE] = queue("playing", "podcastfeed--y")
        self.assertEqual(self.run_at(0), [])

    def test_new_playback_wins(self):
        self.q[SALON] = queue("playing")
        self.run_at(0)
        self.q[PHONE] = queue("playing")
        self.assertEqual(self.run_at(10), [("stop", SALON), ("group", PHONE, [SALON])])

    def test_brimbelle_playing_switches_to_new_playback(self):
        self.q[BRIM] = queue("playing")
        self.run_at(0)
        self.q[SALON] = queue("playing")
        self.assertEqual(self.run_at(10), [("stop", BRIM), ("group", SALON, [BRIM])])

    def test_brimbelle_started_from_app_wins(self):
        self.q[SALON] = queue("playing")
        self.run_at(0)
        self.q[BRIM] = queue("playing")
        self.assertEqual(self.run_at(10), [("stop", SALON), ("group", BRIM, [SALON])])

    def test_brimbelle_click_joins_existing_stream(self):
        self.q[SALON] = queue("playing")
        self.run_at(0)
        self.a.button(100)
        self.q[BRIM] = queue("playing")
        self.assertEqual(self.run_at(104), [("stop", BRIM), ("group", SALON, [BRIM])])

    def test_old_click_does_not_count(self):
        self.q[SALON] = queue("playing")
        self.run_at(0)
        self.a.button(100)
        self.q[BRIM] = queue("playing")
        self.assertEqual(self.run_at(200), [("stop", SALON), ("group", BRIM, [SALON])])

    def test_click_then_playback_elsewhere(self):
        self.a.button(0)
        self.q[BRIM] = queue("playing")
        self.run_at(2)
        self.q[SALON] = queue("playing")
        self.assertEqual(self.run_at(5), [("stop", BRIM), ("group", SALON, [BRIM])])

    def test_group_members_follow_their_leader(self):
        self.q[SALON] = queue("playing")
        pl = players(salon={"group_members": [SALON, SDJ]})
        self.run_at(0, pl)
        self.q[PHONE] = queue("playing")
        self.assertEqual(self.run_at(10, pl), [("stop", SALON), ("group", PHONE, [SALON, SDJ])])

    def test_player_that_cannot_group_is_only_stopped(self):
        self.q[SALON] = queue("playing")
        pl = players(salon={"can_group_with": []})
        self.run_at(0, pl)
        self.q[PHONE] = queue("playing")
        self.assertEqual(self.run_at(10, pl), [("stop", SALON)])

    def test_no_repeat_while_music_assistant_applies(self):
        self.q[SALON] = queue("playing")
        self.run_at(0)
        self.q[PHONE] = queue("playing")
        self.assertTrue(self.run_at(10))
        self.assertEqual(self.run_at(11), [])

    def test_ungroup_once_the_stream_stops(self):
        self.q[SALON] = queue("playing")
        self.run_at(0)
        self.q[PHONE] = queue("playing")
        self.run_at(10)
        self.q[SALON] = queue("idle")
        grouped = players(salon={"synced_to": PHONE})
        self.assertEqual(self.run_at(20, grouped), [])
        self.q[PHONE] = queue("paused")
        self.assertEqual(self.run_at(100, grouped), [])
        self.q[PHONE] = queue("idle")
        self.assertEqual(self.run_at(200, grouped), [])
        self.assertEqual(self.run_at(206, grouped), [("ungroup", SALON)])
        self.assertEqual(self.run_at(300, grouped), [])

    def test_click_in_a_group_takes_only_the_brimbelle_out(self):
        # The phone plays, the Brimbelle follows it. The click pauses the group.
        grouped = players(brim={"synced_to": PHONE}, phone={"group_members": [PHONE, BRIM]})
        self.q[PHONE] = queue("playing")
        for now in (0, 1, 2, 3):
            self.assertEqual(self.run_at(now, grouped), [])
        self.a.button(3.5)
        self.q[PHONE] = queue("paused")
        self.assertEqual(self.run_at(4, grouped), [("ungroup", BRIM), ("play", PHONE)])
        self.q[PHONE] = queue("playing")
        self.assertEqual(self.run_at(5), [])
        self.assertEqual(self.run_at(20), [])

    def test_click_seen_one_poll_after_the_pause(self):
        grouped = players(brim={"synced_to": PHONE})
        self.q[PHONE] = queue("playing")
        for now in (0, 1, 2):
            self.run_at(now, grouped)
        self.q[PHONE] = queue("paused")
        self.assertEqual(self.run_at(3, grouped), [])
        self.a.button(3.5)
        self.assertEqual(self.run_at(4, grouped), [("ungroup", BRIM), ("play", PHONE)])

    def test_click_in_a_paused_group_keeps_it_paused(self):
        grouped = players(brim={"synced_to": PHONE})
        self.q[PHONE] = queue("paused")
        for now in (0, 1, 2, 3):
            self.run_at(now, grouped)
        self.a.button(3.5)
        self.q[PHONE] = queue("playing")
        self.assertEqual(self.run_at(4, grouped), [("ungroup", BRIM), ("pause", PHONE)])

    def test_pause_that_shows_late_is_still_undone(self):
        grouped = players(brim={"synced_to": PHONE})
        self.q[PHONE] = queue("playing")
        for now in (0, 1, 2):
            self.run_at(now, grouped)
        self.a.button(2.5)
        self.assertEqual(self.run_at(3, grouped), [("ungroup", BRIM)])
        self.q[PHONE] = queue("paused")
        self.assertEqual(self.run_at(4), [("play", PHONE)])

    def test_refused_group_is_asked_again(self):
        # The Brimbelle plays with a follower of its own: Music Assistant
        # refuses the first group. It is asked again once the Brimbelle stopped.
        self.q[BRIM] = queue("playing")
        self.run_at(0)
        self.q[PHONE] = queue("playing")
        self.assertEqual(self.run_at(10), [("stop", BRIM), ("group", PHONE, [BRIM])])
        self.q[BRIM] = queue("idle")
        self.assertEqual(self.run_at(11), [("group", PHONE, [BRIM])])
        # It holds: nothing more.
        self.assertEqual(self.run_at(12, players(brim={"synced_to": PHONE})), [])
        # Never holds: no more attempts after the window.
        self.assertEqual(self.run_at(20), [("group", PHONE, [BRIM])])
        self.assertEqual(self.run_at(21), [])

    def test_refused_group_is_dropped_when_the_player_plays_again(self):
        self.q[BRIM] = queue("playing")
        self.run_at(0)
        self.q[PHONE] = queue("playing")
        self.run_at(10)
        self.q[BRIM] = queue("playing", "filesystem_local--x")
        self.assertEqual(self.run_at(11), [])

    def test_click_outside_a_group_changes_nothing(self):
        self.q[BRIM] = queue("playing")
        self.run_at(0)
        self.a.button(1)
        self.q[BRIM] = queue("paused")
        self.assertEqual(self.run_at(2), [])


if __name__ == "__main__":
    unittest.main()
