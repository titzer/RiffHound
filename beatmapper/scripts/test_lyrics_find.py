#!/usr/bin/env python3
"""Offline tests for lyrics_find.py parsing and cleanup: python3 scripts/test_lyrics_find.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lyrics_find as L  # noqa: E402


class T(unittest.TestCase):
    def test_filename(self):
        self.assertEqual(L.parse_filename("03 - Queen - Bohemian Rhapsody (Official Video).mp3"),
                         ("Queen", "Bohemian Rhapsody"))
        self.assertEqual(L.parse_filename("Cripple Creek 110 BPM in A.mp3")[1], "Cripple Creek")
        self.assertEqual(L.parse_filename("CrippleCreek-G-100.mp3")[1], "Cripple Creek")
        self.assertEqual(L.parse_filename("Friday I'm in Love.m4a"), ("", "Friday I'm in Love"))
        self.assertEqual(L.clean_title("Love in an Elevator (Live)"), "Love in an Elevator")
        self.assertTrue(L.looks_like_backing("Blues", "Blues-Am-Slow.mp3"))
        self.assertFalse(L.looks_like_backing("Nowhere Man", "Nowhere Man.mp3"))

    def test_lrc(self):
        lrc = ("[ar:X]\n[offset:+500]\n[00:00.00] 作词 : Somebody\n[00:01.00][00:20.00]Hook line\n"
               "[00:05.50]<00:05.50>Word <00:06.00>sync\n[00:08.00]♪\n[00:09.00][Chorus]\n"
               "[00:10.00]Sing it (x2)\n")
        rows = L.parse_lrc(lrc)
        self.assertEqual([t for _, t in rows], ["Hook line", "Word sync", "Sing it", "Hook line"])
        self.assertAlmostEqual(rows[0][0], 0.5)

    def test_plain(self):
        txt = "[Verse 1]\nOne  \n\n\n[Chorus]\nTwo\n(Instrumental)\nTwo\n"
        self.assertEqual(L.clean_plain(txt), ["One", "Two", "Two"])

    def test_lyricsfile(self):
        y = ("version: '1.0'\nmetadata:\n  title: T\nlines:\n- text: Hello there\n  start_ms: 1000\n"
             "  end_ms: 2500\n- text: '\"Quoted'' line\"'\n  start_ms: 3000\n  end_ms: 4000\n"
             "  words:\n  - text: x\n    start_ms: 3000\n")
        self.assertEqual(L.parse_lyricsfile(y),
                         [(1.0, 2.5, "Hello there"), (3.0, 4.0, "\"Quoted' line\"")])

    def test_sim(self):
        self.assertGreater(L.sim("Tom Petty & The Heartbreakers", "Tom Petty and the Heartbreakers"), 0.95)
        self.assertLess(L.sim("Hotel California", "Highway to Hell"), 0.6)


if __name__ == "__main__":
    unittest.main()
