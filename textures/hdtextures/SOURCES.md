# Источники и авторы

Подборка объединяет существующие изображения для локального использования. Авторство изображений остаётся у исходных авторов. `catalog.csv` содержит источник каждого назначения и точный путь исходного файла.

- **Decked Edition PSP — DankaishinProductionsModding.** [Архив пользователя](https://drive.google.com/file/d/1P6sFCw86Lv4Pa7aBsg9QiRHcQJkf5Tux/view). В credits связанных архивов названы TareqGamer (карта и значки бизнеса), Parallellines, Djdarko, Ryadica926, SonOfUgly и Solidcal. SHA256 RAR: `0a09b1bc1339cc11921988d4e1665a9e8693021acd34fc58b6431c5c082d76b3`.
- **efonte VCS AI Upscaled.** [Публикация автора](https://forums.ppsspp.org/showthread.php?tid=26729), архив `ULES00502_v20200807.zip`. SHA256: `d6e2222e9bddeade75986d915950ffea2f2798325d5588b33d34586cae64db90`. Из него выбрана только небольшая часть, прошедшая проверки без коррекции alpha: 14 прежних назначений и 63 городских дополнения, всего 77. Подробные результаты последнего отбора — в [Sources/CityAI/](Sources/CityAI/README.md); точные пути внутри архива записаны в основном каталоге.
- **Modernize PSP Games — ItzAntonis2012.** [Репозиторий](https://github.com/ItzAntonis2012/modernizepspgames), commit `f0718078245f65305922f80bef43a595414c0da9`, файл `Texture Packs/Grand Theft Auto - Vice City Stories/gtavcs_texturepack_psp.zip`. SHA256 ZIP: `b87686eb02a2e2db165d1aa91c09351ef92e41673313b3c47faa332fb012c2d4`. Использованы три иконки оружия.
- **ASI-Factory — PC Vice City textures for VCS PSP.** [Репозиторий](https://github.com/ASI-Factory/GTA-VCS-Texture-Pack-PSP), commit `d4524c33e47a73d6c6661bce90968f241bdea486`. Выбраны готовые PNG из `source/FromPC/`; соответствие файлам репозитория проверено также по Git blob SHA1. Входящий в репозиторий исполняемый файл не использовался.

Также исследован **Decked Edition PS2**, [архив](https://drive.google.com/file/d/1zKvAJ3SUG6oqSh-2x_BtbWSazwEORDwy/view), SHA256 `d510dd88cd99de7c0ecbd09d4e4bbe873ade911e607f3fb0a06f06c9b1a601e9`. Его файлы не попали в итог: для пригодных изображений нашлись PSP-версии с соответствующей прозрачностью, а оставшиеся варианты требуют адаптации или дополнительного подтверждения.

Уже установленный [SonofUgly/VCS-Texture-Pack](https://github.com/SonofUgly/VCS-Texture-Pack), commit `93944f237e1c348c2d61645b4b859a7f4b5cd73e`, теперь включён целиком: все 207 PNG и 230 существующих назначений скопированы из локального установленного пакета без изменения изображений. Его исходные документы находятся в `Sources/InstalledHD/`.

## Escobar International Airport R-TXD 2025

[Пакет автора](https://www.gtainside.com/vicecity/mods/modifications/210563-escobar-international-airport-r-txd-2025) — Ilya Kostygov aka Till Lindermann, материалы Vice Cry ReBorn Team. Добавлены 207 назначений из 151 декодированных PNG. [Происхождение и проверка](Sources/Escobar2025/README.md). SHA256 архива: `a06d736bf3ef73a2aacd4a67e49fe0f50f3c3efc2db845061225fc995d498ebf`.

## Original PC Vice City textures

[Исходные PC PNG из ASI-Factory](https://github.com/ASI-Factory/GTA-VCS-Texture-Pack-PSP/tree/d4524c33e47a73d6c6661bce90968f241bdea486/source/FromPC): 1513 новых назначений, 1431 файлов. Изображения скопированы побайтово; новые AI-текстуры не добавлялись. [Происхождение и проверка](Sources/PCOriginals/README.md).
