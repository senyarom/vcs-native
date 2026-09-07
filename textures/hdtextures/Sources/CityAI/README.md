# Городские AI-дополнения

63 PNG из efonte VCS AI Upscaled v20200807 включены в общий пакет 2026-09-06. Все файлы находятся в `Assets/World/AI/` относительно корня hdtextures; назначения уже добавлены в общий `textures.ini`.

Источник: [публикация efonte](https://forums.ppsspp.org/showthread.php?tid=26729). SHA256 архива: `d6e2222e9bddeade75986d915950ffea2f2798325d5588b33d34586cae64db90`.

`catalog.csv` сохраняет подробные измерения alpha, идентификатор исходного RGBA-изображения, точный путь AI-файла внутри ZIP, SHA256 и результат визуального отбора. Поле `path` отсчитывается от корня hdtextures; `ready_path` и `original_png` — исторические пути внутри служебного аудита. Поле `detail_status=visual_review_required` описывает этап численного отбора; итог ручной проверки записан в `decision=accepted_static` и `visual_review`.

`merge-result.json` фиксирует объединение и проверки сохранности файлов. Критерии отбора и ограничения — в [QUALITY.md](../../QUALITY.md). Подробный локальный аудит со скриптами и оригиналами находится в `work/city-texture-audit/` относительно корня VCSNative. Изображения не редактировались; игровой прогон после добавления не выполнялся.
