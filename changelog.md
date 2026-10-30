# v0.3.0

* Clicks were about 1 s too early in the video. OBS's `outputDuration` lags the picture by its audio buffering (up to 960 ms) plus the encoder, so video time is now counted from the moment the recording started. The polls only measure that lag (`pipeline_lag_ms`) and are the fallback when the recording was already running.
* Recording pauses are subtracted; clicks during a pause get `video_s: null`.
* `frame` uses xdBot's own TPS (240 unless its TPS setting is on), not the game's step counter: at 480 game TPS it was double.

# v0.2.0

* `frame` now matches xdBot's macro frames (step counter + 1, minus xdBot's Frame Offset) and `tick` has the raw step count. `tps` is measured instead of always 240.
* Sessions without OBS video time are no longer saved, and `latest.json` is only replaced by sessions that have it. Setting `Save without OBS` brings the old behaviour back.
* Toast when OBS starts recording and when a file is saved (setting `Notifications`).

# v0.1.0

* First version: obs-websocket v5 client, input logging, JSON export with video timestamps.
