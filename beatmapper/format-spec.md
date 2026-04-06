### Timeseries File Format
Timeseries files are line-oriented text files.
Comments in timeseries files start with # and extend to the end of the line.
Blank lines are ignored.
Times are stored in one of two formats: *seconds* from the beginning of the track, expressed in fractional decimal, with up to 6 fractional digits of precision, or *beats*, which are positive decimal integers prefixed with "B", indicating a beat number.
Each line consists of a *start time*, *end time*, and *event name*, separated by at least one space or tab character.
An event is invalid and should be discarded if its end time is less than its start time.
Events with the end time equal to the start time are valid and are considered *instantaneous*.
The event name is a string consisting of the characters following the whitespace after the end time until the end of the line, not including the newline character(s).
Canonical time series files should be sorted by increasing start time and then increasing end time.
A simple sorting tool can take an unsorted timeseries file and sort it without interpreting the events, while preserving comments on lines immediately preceding an event or at the end of the line as if they were attached to the event.

### Beatmap File Format
The format of a beatmap file consists of beat events one per line as a timeseries file, where times are always in seconds.
Beats are events that have the names "B", "BM", "Bx<N>", or "BMx<N>, where <N> is a positive decimal integer up to 10000.
A "B" indicates a singular beat at the start time (ignoring the end time), and "BxN" indicates N beats, with the first starting at the start time, and the last at the end time, and the intervening beats evenly spaced between (i.e. interpolated).
A beat labeled "BM" indicates a beat which starts a measure, which is used by some tools and can be used to compute a local time signature.
Beats are numbered in increasing order starting from 1.

#### Beatmap File Examples

---beatmap1.txt--------
# A comment
7.890   8.890   Bx3
-----------------------

beatmap1.txt three beats B1=7.890, B2=8.390, and B3=8.890.

---beatmap2.txt--------
3.010   3.010   B
3.510   3.510   B # A comment
4.010   5.010   Bx3
-----------------------

beatmap2.txt has five beats at B1=3.010, B2=3.510, B3=4.010, B4=4.510, and B5=5.010.

Note that each of these has a calculated average BPM of 120 over the span from the first beat to the last.

### Section File Format
The format of a section file consists of section ranges as events as a timeseries file with times in either seconds or beats.
If stored as beats, then an external beatmap is needed to map beats to seconds.
Sections are recognized as events that start with the given keywords:

intro         (Opening section; sets mood, often instrumental)
verse         (Main storytelling section; lyrics often change)
pre-chorus    (Transitional lift into the chorus)
chorus        (Repeating core hook; main message of the song)
post-chorus   (Tag or hook extension after the chorus)
bridge        (Contrasting section, often later in the song)
breakdown     (Stripped-down or rhythm-focused section)
instrumental  (Section without vocals, can include solos)
solo          (Featured instrumental lead, guitar, piano, etc.)
interlude     (Short connecting passage between sections)
outro         (Closing section of the song)
refrain       (Repeated lyrical/melodic line)

A section name may be followed by a colon and additional information, such as a number.

---sectionmap1.txt------
# Hey Jude
B45    B65        verse: 1
B67    B77        chorus
------------------------

sectionmap1.txt has two sections, with a verse from beat 45 to 64 and a chorus from beat 67 to 77.

### Combining beatmaps and section maps

Timeseries files can be merged together, in which case all events are sorted together and duplicate events can either be preserved or discarded according to application.
For example, beat map files and section map files can be combined for easier file management.
Events whose times are expressed in beats must follow the definitions of those beats from the beatmap.
