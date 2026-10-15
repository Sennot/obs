# ClickSync

Geode-мод для Geometry Dash 2.2081 (Windows). Пока OBS пишет видео, мод записывает каждое нажатие и отжатие
и сохраняет их **с точным временем в видео**. Плагин Click Sampler для Vegas читает этот файл.

Mod for Geometry Dash 2.2081 (Windows). While OBS records, it logs every press/release together with its
exact **video timestamp** into a JSON file for the Click Sampler Vegas plugin.

## Как это работает

1. Мод подключается к OBS по obs-websocket v5 (встроен в OBS 28+).
2. Когда запись начинается, мод раз в секунду спрашивает у OBS `GetRecordStatus` и запоминает, на какой
   секунде видео сейчас находится OBS и в какой момент пришёл ответ (середина сетевого обмена).
   Из самых быстрых обменов берётся медианное смещение — это связь «время игры → время видео».
3. Каждый вызов `GJBaseGameLayer::handleButton` (в том числе от бота, например xdBot) пишется в журнал:
   нажатие/отжатие, кнопка, игрок, время уровня, кадр (при 240 TPS), попытка, практика.
4. Когда запись в OBS остановлена, мод сохраняет `clicksync_ГГГГММДД_ЧЧММСС.json` и копию `latest.json`.
   Если OBS не подключён, файл сохраняется при выходе из уровня (тогда `video_s` будет `null`).

## Настройка

1. В OBS: **Tools → WebSocket Server Settings** → включить сервер (порт 4455). Пароль можно оставить пустым
   или вписать его в настройки мода.
2. Установить `.geode` в папку модов GD.
3. Записывать как обычно. Файлы лежат в `Документы\ClickSampler\Sync\` (папку можно сменить в настройках).

## Формат `clicksync/1`

```json
{
  "format": "clicksync/1",
  "tps": 240.0,
  "obs": { "recording": "D:\\videos\\2026-10-16.mkv", "sync_method": "anchor", "anchors": 61, "best_rtt_ms": 1.8 },
  "attempts": [ { "n": 1, "level": "Name", "level_id": 123, "video_s": 12.0312 } ],
  "events": [
    { "video_s": 15.2041, "level_time": 3.17, "frame": 761, "down": true, "button": 1, "p2": false,
      "practice": false, "attempt": 1, "level": "Name", "level_id": 123 }
  ]
}
```

`video_s` — секунда в видео; `frame` = `level_time × tps`; `sync_method`: `anchor` (по опросам OBS),
`start` (по моменту старта записи, грубее) или `none`.

## Точность

Время берётся в момент, когда игра обработала ввод: ошибка порядка одного кадра рендера. Для точной подгонки
в плагине можно сдвинуть всё на фиксированное смещение. Сопоставление с кадрами макроса идёт по `frame` и `level_time`.

## Сборка

GitHub Actions (`.github/workflows/build.yml`): тесты ядра на Linux и сборка `ClickSync.geode` через
`geode-sdk/build-geode-mod`. Артефакт `ClickSync.geode` лежит в результатах запуска.
