# Music Assistant single stream (Home Assistant add-on)

The YouTube Music account allows one stream at a time. This add-on reads the
Music Assistant queues every second and keeps a single YouTube Music stream:

- A click on the Brimbelle knob (resume, or start while idle) while another
  player already plays YouTube Music: the Brimbelle joins that stream.
- Any other new YouTube Music playback wins: the players that were already
  playing YouTube Music are grouped to it.
- A click on the knob while the Brimbelle follows another player: only the
  Brimbelle leaves the group. The click pauses or resumes the whole group for
  a moment, the add-on puts it back in the state it had.
- Other sources (local files, podcasts, radios, Bluetooth) are left alone.

Players grouped by the add-on are ungrouped once the stream they joined
stops. A group that Music Assistant refused is asked again for a few
seconds. Two streams overlap for a second or two before the add-on reacts.

## Install

Local add-on: copy this folder to `/addons/ma_single_stream` on the Home
Assistant host, reload the add-on store, install it, set the options and
start it.

## Options

| Option | Meaning |
|---|---|
| `ma_url` | Music Assistant address as seen from an add-on |
| `ma_token` | Long-lived Music Assistant token (API command `auth/token/create`) |
| `brimbelle_player_id` | Music Assistant player id of the Brimbelle |
| `button_entity` | Home Assistant event entity of the Brimbelle knob |
| `button_window` | Seconds after a click during which a Brimbelle start counts as a click |
| `dry_run` | `true`: log what would be done, change nothing |

Tests of the rules: `python3 test/addon/test_single_stream.py`.
