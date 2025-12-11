#!/usr/bin/env python3
"""
RiffText song server with simple file-based persistence.

Endpoints:
  GET  /songs?q=...        -> list songs (basic search)
  GET  /song/{id}          -> get one song
  PUT  /song/{id}          -> update one song (full replace)

Data is stored in songs.json in the same directory as this script.
"""

from typing import List, Dict, Optional
from fastapi import FastAPI, HTTPException
from fastapi.responses import HTMLResponse
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import uvicorn
import json
from pathlib import Path

# ---------- Config ----------

DATA_FILE = Path(__file__).with_name("songs.json")

# ---------- Data models ----------

class Song(BaseModel):
    id: str
    title: str
    artist: Optional[str] = None
    tags: List[str] = []
    rifftext: str

class SongUpdate(BaseModel):
    title: str
    artist: Optional[str] = None
    tags: List[str] = []
    rifftext: str

# ---------- App setup ----------

app = FastAPI(title="RiffText Song API (file-backed)")

# Allow browser app from file:// or any origin (simple for dev)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# In-memory cache; persisted to DATA_FILE
songs: Dict[str, Song] = {}


# ---------- Persistence helpers ----------

def load_songs_from_disk() -> Dict[str, Song]:
    """Load songs from DATA_FILE into a dict[id -> Song]."""
    if not DATA_FILE.exists():
        # Seed with a few demo songs on first run
        seed = {
            "1": Song(
                id="1",
                title="Blues in A",
                artist="Demo Artist",
                tags=["blues", "A"],
                rifftext="KEY A\nTEMPO 100\n[verse]\nA7 D7 A7 E7 D7 A7\n",
            ),
            "2": Song(
                id="2",
                title="Funky Jam in E",
                artist="Demo Artist",
                tags=["funk", "E"],
                rifftext="KEY E\nTEMPO 110\n[groove]\nE7 E7 E7 E7\n",
            ),
            "3": Song(
                id="3",
                title="Ballad in C",
                artist="Someone Else",
                tags=["ballad", "C"],
                rifftext="KEY C\nTEMPO 70\n[verse]\nC F G C\n",
            ),
        }
        save_songs_to_disk(seed)
        return seed

    try:
        raw = json.loads(DATA_FILE.read_text(encoding="utf-8"))
        # raw is expected to be a dict id -> plain dict
        loaded: Dict[str, Song] = {}
        for sid, sdict in raw.items():
            loaded[sid] = Song(**sdict)
        return loaded
    except Exception as e:
        print(f"Error loading {DATA_FILE}: {e}")
        # Fall back to empty if something is wrong with the file
        return {}


def save_songs_to_disk(songs_map: Dict[str, Song]) -> None:
    """Persist songs_map to DATA_FILE as JSON."""
    # Convert Pydantic models to plain dicts
    raw = {sid: s.model_dump() for sid, s in songs_map.items()}
    tmp_file = DATA_FILE.with_suffix(".json.tmp")
    tmp_file.write_text(json.dumps(raw, indent=2, ensure_ascii=False), encoding="utf-8")
    tmp_file.replace(DATA_FILE)


# Load initial data at import time
songs = load_songs_from_disk()


# ---------- Endpoints ----------
@app.get("/", response_class=HTMLResponse)
def root():
    html_file = Path(__file__).with_name("demo.html")
    if not html_file.exists():
        return "<h1>demo.html not found</h1>"
    return html_file.read_text(encoding="utf-8")


@app.get("/songs", response_model=List[Song])
def list_songs(q: Optional[str] = None):
    """
    List all songs, optionally filtered by simple substring query on
    title / artist / tags.
    """
    result = list(songs.values())
    if q:
        q_low = q.lower()
        filtered = []
        for s in result:
            if (
                q_low in s.title.lower()
                or (s.artist and q_low in s.artist.lower())
                or any(q_low in t.lower() for t in s.tags)
            ):
                filtered.append(s)
        result = filtered
    return result


@app.get("/song/{song_id}", response_model=Song)
def get_song(song_id: str):
    """
    Get a single song by ID.
    """
    if song_id not in songs:
        raise HTTPException(status_code=404, detail="Song not found")
    return songs[song_id]


@app.put("/song/{song_id}", response_model=Song)
def update_song(song_id: str, payload: SongUpdate):
    """
    Replace or create a song's contents by ID.
    If the song does not exist yet, it is created.
    """
    updated = Song(
        id=song_id,
        title=payload.title,
        artist=payload.artist,
        tags=payload.tags,
        rifftext=payload.rifftext,
    )
    songs[song_id] = updated
    save_songs_to_disk(songs)
    return updated

if __name__ == "__main__":
    # Run: python server.py
    uvicorn.run("server:app", host="0.0.0.0", port=8000, reload=True)
