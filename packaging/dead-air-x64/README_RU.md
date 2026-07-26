# Dead Air x64

64-битный движок для Dead Air 0.98b и Dead Air Revolution II. Он использует
существующие `database`, `gamedata`, сохранения и обычный порядок загрузки
аддонов. Перепаковывать игру или моды не нужно.

## Установка

1. Убедись, что установлен и запускается Dead Air 0.98b или Dead Air
   Revolution II.
2. Закрой игру.
3. Распакуй архив аддона в любую отдельную папку.
4. Открой PowerShell в корневой папке игры, где лежат `xrEngine.exe`,
   `fsgame.ltx` и папка `database`.
5. Выполни:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "ПУТЬ_К_АДДОНУ\Install-DeadAir-x64.ps1" -GameRoot .
```

После установки запускай игру как раньше: старый ярлык и `xrEngine.exe`
остаются точкой входа. JSGME для установки движка не требуется. Аддоны через
JSGME устанавливаются и включаются в прежнем порядке.

Установщик сохраняет заменяемые x86-файлы в `.dead-air-x64`, но не меняет
`database`, `gamedata`, `appdata`, сохранения, `MODS` или состояние JSGME.

## Удаление

Закрой игру и из её корневой папки выполни:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "ПУТЬ_К_АДДОНУ\Uninstall-DeadAir-x64.ps1" -GameRoot .
```

Исходные x86-файлы будут восстановлены автоматически.

## Ручная установка

Если PowerShell-установщик использовать нельзя, заранее сохрани копию
корневых EXE/DLL игры, затем скопируй все файлы из папки `runtime` в корень
игры с заменой. Не удаляй и не заменяй `database`, `gamedata`, `appdata`,
`MODS` и `fsgame.ltx`.

