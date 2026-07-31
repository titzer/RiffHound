# RiffHound
Various code and data related to riffing and jamming

# Jam-along
Idea: easily connect to a jam with your friends in a room or across town

 - in-room:
    - go to:
      URL, e.g. riffhound.com/fuggity-foo
      QR code?
      password?
    - choose role

 - commands:
   - do new song
   - restart, retry, retrain
   - speed up, slow down
   - change keys
   - mashup songs
   - take piece of song
   - drummer can implicitly set the tempo

- take personal notes
  - I need to learn/train this section

- Riffhound virtual band members
  - we want to play a song, missing bass or keyboardist
  - AI band member does the job
  - might just be a pre-synthesized track with warping to fit the performance
  - might be creative and embellish, riff along

# Scenarios

Sketches to pressure-test the ideas above. Each ends with what it demands.

## Four in a garage

Four friends, one song, four phones on stands. Everyone joins
`riffhound.com/fuggity-foo` and picks a role. The guitarist sees chords and a
strum pattern; the singer sees lyrics with a syllable guide; the bassist sees
root motion; the drummer sees the count and the section map. Same song, same
bar, four different screens.

*Demands:* shared position, private view. The session syncs one thing — where
we are — and nothing about layout. A guitarist who wants lyrics only for
position-keeping is a local preference, not a session setting.

## Someone shows up at bar 47

A fifth player wanders in, scans the QR code, picks harmonica, and lands on
the current bar with a blow pattern already showing.

*Demands:* position is explicit shared state, not something each client
accumulates by counting from the top. Same property lets a phone that slept,
dropped Wi-Fi, or reloaded rejoin without restarting the song.

## Who is the clock?

"Drummer can implicitly set the tempo" is the interesting note, because it
inverts the practice model. In a trainer the app owns the clock and the human
follows. In a room with four people playing, the band owns the clock and the
app follows. Two different systems:

 - **app-leads** — metronomic, app authoritative, everyone plays to it. Cheap,
   and correct for practice, click work, and any virtual band member.
 - **band-leads** — the app tracks live audio to find the beat and follows.
   Much harder (humans rush and drag), but it is the difference between
   jamming and playing karaoke.

Build app-leads first; treat band-leads as a distinct mode rather than a
better version of the same one.

## Across town

"In a room or across town" hides a large gap. In a room sound crosses in ~3 ms
and screens need only agree within a few tens of ms. Across town, 20–80 ms
round trips put real-time playing together in dedicated low-latency audio
territory — a different product.

Remote scenarios that work without solving that:

 - **shared practice** — same position, same marked sections, one person
   drives transport, one plays at a time. Trading fours by turns.
 - **shared click** — everyone hears the app rather than each other; takes are
   aligned afterward.
 - **asynchronous** — pass the session on, layer parts, leave notes.

"Jam together across town in real time" over-claims. "Learn the same song
together from anywhere" is honest and covers most of the value.

## The missing bassist

The warped-stem version is the tractable one and it fits app-leads: everyone
follows the app's clock, and the virtual player is a stem stretched to the
current tempo and key.

The creative version has a *following* problem, not a generation problem — an
AI bassist that embellishes still has to know where the band actually is,
which is band-leads again. Warped stems now; embellishment after live
tracking exists.

## After the jam

The valuable artifact of a jam is not the recording, it is the list of places
somebody struggled. "I need to learn this section" should turn bars 33–48 into
a practice loop that can be slowed, looped, and simplified, then played back
into the same song next week.

*Demands:* sections are first-class — a named span of bars, not a timestamp
range.

# Implications

 - **Sync a position, not a UI.** Session state is song, position, tempo, key,
   and section marks. Views stay local. Small protocol, and two people can
   watch the same moment through different instruments.
 - **Transport needs an owner.** If everyone can slow the song down, nobody
   can play it. For friends in a room: anyone may change it, everyone sees who
   did. Permissions later, if ever.
 - **Sections before mashups.** Change keys, take a piece of a song, mash two
   together, mark one to practise — all operations on named spans of bars.
 - **Chords as data, not strings.** Transposition and mashups need root,
   quality, and bass, not `G/B` as a display string.
 - **Smallest real version:** one host, others join read-only with their own
   view. No guest commands, no live tracking, no virtual players. That is
   already a jam-along.

