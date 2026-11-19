# Config schema

Stored in internal flash (LittleFS) at `/littlefs/config.json`. Holds no
secret: NVS holds the Wi-Fi credentials and the hashed web page password.

## Schema (version 1)

```json
{
  "schema_version": 1,
  "device": { "name": "" },
  "webradios": [
    { "id": 1, "name": "FIP", "url": "https://icecast.example/fip", "skip_preroll": 0 }
  ],
  "audio": { "volume": 50, "volume_max": 100, "output": "mono", "eq": [] },
  "playback": { "pause_timeout_min": 60 }
}
```

- `device.name`: shown in Music Assistant and on the page. Empty means
  "Brimbelle XXXX" (device id from the MAC).
- `webradios[]`: up to 32. `id` is stable and unique (renumbered on
  collision), `url` is required, `skip_preroll` 0/1.
- `audio.volume`: volume at boot, 0 to 100.
- `audio.volume_max`: ceiling 1 to 100 applied to every volume request.
- `audio.output`: what the two amp outputs play. `mono` (default): the mix
  (L + R) / 2 on both, for one speaker on OUTL. `stereo`: left on OUTL, right
  on OUTR, for two speakers on one board. `left` or `right`: that channel
  alone on both outputs, for a unit that is one side of a pair. Absent or
  unknown means `mono`. Applied when the config is saved, no reboot needed.
- `audio.eq[]`: speaker correction, up to 6 filters applied to everything
  that plays, empty by default. Each is `{"type", "hz", "db", "q"}` with
  `type` one of `highpass` (12 dB/octave below `hz`, `db` unused), `lowshelf`
  (`db` below `hz`), `highshelf` (`db` above `hz`) or `peak` (`db` around
  `hz`, width set by `q`). `db` is limited to -12..+12, `q` to 0.3..10
  (default 0.707). A boost does not make the speaker louder: the whole signal
  is lowered by the highest point of the combined curve, so the volume
  ceiling still holds. Edited in the "Sound correction" card of the page.
  Applied when the config is saved.
- `playback.pause_timeout_min`: a pause (web radio or Music Assistant) that
  lasts longer than this many minutes ends in a stop, so the speaker goes
  back to idle. 0 means never, the maximum is 1440. Default 60. Checked every
  30 s. Applied when the config is saved, also to a pause already running.

Not in config.json: the Wi-Fi networks, the page password and the MQTT broker
(url, user, password) live in NVS. The broker is set with `POST /api/mqtt`
(`{"url":"mqtt://host:1883","user":"..","pass":".."}`; a blank `pass` keeps
the stored one, an empty `url` turns MQTT off) and read with `GET /api/mqtt`
(url and user only).

A POST to `/api/config` replaces the whole file. An absent key keeps its
default; a present array replaces the list.
