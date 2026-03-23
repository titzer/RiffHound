# Riffhound Beatmap Editor Specification

## Purpose
The beatmap editor is an application and related tools to help users quickly produce metadata for a song or track, including a *beat map* and a *section map* which capture the timing and structure of a song.
Here, a track will mean a single .mp3 (or other format, as specified below) file storing audio data that represents a self-contained song.
A *beatmap* stores the time offset (in seconds) of every downbeat (typically quarter note) from the beginning of the track.
Beats correspond to events in the physical timeline of the track, not specific sounds, drum hits, voices, or rhythm patterns.
Instead, musical elements are mapped to track time indirectly via the beatmap, and depending on artist expression, can be early or late relative to a beat's track time.
From the beatmap, the average beats per minute (BPM) of the whole track, section, or run can be computed.
The beatmap and BPM indicator allow playback tools that increase or decrease speed for practice, study, performance, mixing, etc to do so mostly independently of the underlying physical time.
The *section map* stores the start and end of every song section, such as verse, chorus, bridge, etc.
The section map simplifies mapping musical elements to parts of a song and is intended to allow a multiple of tools.
E.g. other tools create and use metadata such as a chord chart, sheet music, guitar tablature, etc.
These tools can directly synchronize to track time or use the beat map and section map to denote beat numbers, or sections.
The beat map and section map therefore serve as guides for higher-level applications to express musical information that is relative to sections or beats decoupled from a track.

## Formats And Storage

### Input
The beatmap editor can load audio files in .mp3 and .wav format, with modularity to support other formats in the future.

### Output
The beatmap and section map will be stored as timeseries text files (see: format-spec.md).
Normally, these will be stored as separate files, but it is possible to also store them in metadata tags in the track itself (e.g. ID3).

### Storage
For local-first applications, tracks, beatmaps, section maps, and other metadata will be stored in per-song directories.
Directories will have relatively short, lower-case names separated by underscores, such as "hey_jude/" and "stairway/" that are sufficient to disambiguate them from other tracks.
Directory names are not primarily used for organization of tracks, but separation of tracks.
The metadata (i.e. tags) of mp3 files in directories will serve as the canonical source of artist, album, track number, track name, cover art, etc information.

### Indexing
The tracks stored in a given set of directories will be indexed to allow efficient querying of an entire library without having to search directories.
For example, to support fuzzy search, search by artist, track name, or to navigate amongst related tracks, indexes will be created that allow efficient queries that return track identifiers (i.e. directory names).

## GUI Beatmap Editor

The GUI beatmap editor allows a user to easily load a track as well as any pre-existing beatmap or section map.
It then includes a main display of the track's audio data (e.g. a spectrogram, waveform, etc), plus visual representations of beats and sections.
The user can add, move or otherwise edit both the beat and section map by directly selecting and dragging either beats or the start and end of sections.
In the editor, sections will always be delimited in terms of beats (though the section map format allows seconds).
The editor includes a beat finder tool that, given a selection of the track's audio, will perform audio analysis to determine suggested beats.
The beat finder's suggested beats can be selected independently and then committed to the main working beatmap.
The editor includes several useful tools for bulk-placing beats, such as selecting the beginning and end beat and then a beat-finder-assisted interpolation, plus a simple interpolation helper based on counting.
Rather than relying on zooming / panning, the editor shows the track in multiple zoom levels simultaneously, with the widest zoom being the entire track and 2-4 levels between, with the tightest zoom level appropriately-sized for manual adjustment (e.g. 20-50 pixels for a beat).
The editor uses the beatmap to show appropriate grids at each zoom level, and a time signature for different sections (default 4/4, with easy way to get to 3/4 6/8, etc) makes measures visually distinguishable.
Mouseover adds subtle highlights to measures / beats to emphasize the underlying measure and section.
The editor optimizes the in-memory representation of the beatmap to use repeated beats in order to compress the stored representation.
A quantize tool allows selecting a range of beats and then re-interpolating their positions based on the average BPM between the start and end, which allows a user to quickly mark all the beats (based on visualization of the spectrogram), select them, and have them quantized.
A nudge tool allows selecting a range of beats and using the beat finder tool to adjust the beats to more exactly match detected beats.
A cue tool allows creating count-ins for tricky sections and complex rhythm parts

TODO: think of additional tools, like quantization, rulers, etc, and form implementation-plan.md

## App ideas for beatmaps

### A basic list of apps that will use beatmaps and section maps:

- Track player with faster / slower / pitch up / pitch down
  - loop section, phrase, measure, etc
- Chord buddy
- Karaoke program with lyrics
- Synced guitar tab player
- Extraction and remix for looping
