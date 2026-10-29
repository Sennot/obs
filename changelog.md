# v0.2.0

* `frame` now matches xdBot's macro frames (step counter + 1, minus xdBot's Frame Offset) and `tick` has the raw step count. `tps` is measured instead of always 240.
* Sessions without OBS video time are no longer saved, and `latest.json` is only replaced by sessions that have it. Setting `Save without OBS` brings the old behaviour back.
* Toast when OBS starts recording and when a file is saved (setting `Notifications`).

# v0.1.0

* First version: obs-websocket v5 client, input logging, JSON export with video timestamps.
