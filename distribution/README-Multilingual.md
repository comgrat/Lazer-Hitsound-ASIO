# LazerASIO — Tachyon 1110

[简体中文](#简体中文) · [繁體中文](#繁體中文) · [English](#english) · [Français](#français) · [Español](#español)

Based on [Mother-Ship/osu-lazer-External-ASIO-Sound](https://github.com/Mother-Ship/osu-lazer-External-ASIO-Sound), whose upstream is [XTXTMTXTX/osu-External-ASIO-Sound](https://github.com/XTXTMTXTX/osu-External-ASIO-Sound). This is an independently maintained modification, not an official upstream release. Original authorship and third-party rights remain with their respective owners.

This package does not include the game, account data or beatmaps. `Start-ASIO.cmd` opens the same public launcher. Reviewed references include Game `2026.1026.0-tachyon` / `2026.1110.0-tachyon` and Framework `2026.1019.0` / `2026.1030.0`.

## 简体中文

1. 首次打开 `config.exe` 或 `osu!asio_sound.exe`，选择界面语言。以后双击 `Change-Language.cmd` 可重新选择；重开已有窗口后生效。
2. 用 `config.exe` 选择自己的ASIO驱动并保存。发布默认：16 samples、Driver ID 0、48000 Hz、独立音量50%、Debug Log 0；0号只是初始占位，不代表已为你选好声卡。
3. 运行 `osu!asio_sound.exe`，保持等待窗口，照常启动自己的osu!lazer。工具显示附加就绪后进入谱面。它不启动、复制或修改游戏客户端。
4. 游戏退出后ASIO设备释放，工具继续等待。关闭工具后若要重新接管，请重启游戏。EXE改名的客户端可使用 `osu!asio_sound (select osu exename).exe`，输入进程文件名。

游戏内Hitsound=0%仍可有ASIO击打声。调节ASIO音量请修改同目录 `config.ini` 的 `Output Volume`（0～100），保存后约0.5秒生效；0会静音ASIO。驱动、buffer和采样率变更需重启游戏。BGM与UI音效保留原生，谱面增益和声像保留。`language.ini` 单独保存语言，不改变原五项音频配置。

本包不包含游戏客户端、账号数据或谱面。`managed`、`locales` 与DLL/后端都是运行依赖，请保留。可自由改最外层文件夹名。仅检查了.NET10的Game2026.1026/1110、Framework2026.1019/1030音频实现族；未知实现可能被拒绝。工具会向游戏进程注入Hook并安装运行时适配，请自行了解客户端规则。软件buffer时长不是实测端到端延迟。

`Test-ASIO-Device.cmd` 是可选左右声道试听，先退出游戏和其他ASIO程序并降低监听音量。运行生成的日志、`.asio-device-configured` 与 `language.ini` 属于本机状态；分享请使用原始干净ZIP。

## 繁體中文

1. 首次開啟 `config.exe` 或 `osu!asio_sound.exe`，選擇介面語言。之後可按兩下 `Change-Language.cmd` 重新選擇；請重新開啟原有視窗。
2. 使用 `config.exe` 選擇自己的ASIO驅動程式並儲存。發佈預設：16 samples、Driver ID 0、48000 Hz、獨立音量50%、Debug Log 0。編號0只是初始值，仍須確認自己的裝置。
3. 執行 `osu!asio_sound.exe`，保持等待視窗開啟，照常啟動自己的osu!lazer。顯示附加就緒後再進入譜面。工具不會啟動、複製或修改遊戲用戶端。
4. 遊戲結束後會釋放ASIO裝置，工具繼續等待。關閉工具後若要重新接管，請重新啟動遊戲。若遊戲EXE已改名，可使用含 `select osu exename` 的入口並輸入處理程序檔名。

遊戲Hitsound=0%時ASIO仍可播放。ASIO音量由同一資料夾 `config.ini` 的 `Output Volume`（0～100）控制，儲存後約0.5秒更新；0會靜音。驅動程式、緩衝區及取樣率變更需要重開遊戲。BGM與UI音效維持原生，保留譜面增益與聲像。語言偏好另存於 `language.ini`。

請保留 `managed`、`locales` 及必要DLL/後端；最外層資料夾可改名。僅檢查過.NET10的Game2026.1026/1110及Framework2026.1019/1030音訊實作族，未知實作可能被拒絕。工具會向遊戲注入Hook及安裝執行階段配接器，請自行了解用戶端規則。軟體緩衝區時長並非實測端對端延遲。

`Test-ASIO-Device.cmd` 可選擇性測試左右聲道；請先退出遊戲與其他ASIO程式並調低監聽音量。分享時使用原始ZIP，勿夾帶執行記錄、裝置標記或語言偏好。

## English

1. Open `config.exe` or `osu!asio_sound.exe` and choose a language on first use. Double-click `Change-Language.cmd` to change it later; reopen existing tool windows.
2. Use `config.exe` to choose your own ASIO driver and save. Clean defaults: 16 samples, Driver ID 0, 48000 Hz, independent volume 50%, Debug Log 0. Device 0 is an initial value, not an automatically verified selection.
3. Run `osu!asio_sound.exe`, keep the waiting window open, and start osu!lazer normally. Enter a beatmap after attachment is ready. The tool does not start, copy or modify the game client.
4. The ASIO device is released when the game exits; the tool keeps waiting. Restart the game before reattaching after closing the tool. For a renamed game EXE, use the `select osu exename` launcher and enter the process file name.

The game Hitsound slider at 0% still allows ASIO hitsounds. Set `Output Volume` (0–100) in the tool-side `config.ini` to control ASIO; saving updates it in about 0.5 seconds, and 0 mutes it. Driver/buffer/sample-rate changes require a game restart. Music and UI sounds retain native output; beatmap gain and pan are preserved. Language is saved separately in `language.ini`, keeping the original five audio fields.

Keep `managed`, `locales`, the DLLs and the backend. The outer folder may be renamed. Reviewed audio implementations: .NET10 Game2026.1026/1110 and Framework2026.1019/1030; unknown implementations may be rejected. This tool injects a hook and installs runtime adapters into the game process; understand the client's rules before use. Software buffer duration is not measured end-to-end latency.

`Test-ASIO-Device.cmd` optionally plays left/right test tones. Close the game and other ASIO applications and lower your listening volume first. Share the original clean ZIP, without generated logs, device markers or language preferences.

## Français

1. Au premier lancement de `config.exe` ou `osu!asio_sound.exe`, choisissez la langue. Pour la changer ensuite, ouvrez `Change-Language.cmd`, puis rouvrez les fenêtres déjà actives.
2. Dans `config.exe`, choisissez votre propre pilote ASIO et enregistrez. Valeurs de publication : 16 échantillons, Driver ID 0, 48000 Hz, volume indépendant 50 %, Debug Log 0. L’identifiant 0 est une valeur initiale à vérifier.
3. Lancez `osu!asio_sound.exe`, gardez la fenêtre d’attente ouverte et démarrez osu!lazer comme d’habitude. Attendez la confirmation du rattachement avant d’ouvrir une beatmap. L’outil ne démarre pas, ne copie pas et ne modifie pas le client du jeu.
4. Le périphérique ASIO est libéré à la fermeture du jeu et l’outil continue d’attendre. Redémarrez le jeu avant un nouveau rattachement après fermeture de l’outil. Si l’EXE du jeu a été renommé, utilisez l’entrée `select osu exename` et indiquez son nom de fichier.

Le curseur Hitsound du jeu à 0 % permet toujours les sons ASIO. Modifiez `Output Volume` (0–100) dans le `config.ini` de l’outil : la valeur est appliquée environ 0,5 seconde après l’enregistrement, et 0 coupe ces sons. Un changement de pilote, tampon ou fréquence nécessite un redémarrage du jeu. La musique et les sons d’interface restent sur la sortie native ; gain de beatmap et panoramique sont conservés. La langue est enregistrée séparément dans `language.ini`.

Conservez `managed`, `locales`, les DLL et le backend. Le dossier extérieur peut être renommé. Implémentations vérifiées : .NET10 Game2026.1026/1110 et Framework2026.1019/1030 ; une implémentation inconnue peut être refusée. L’outil injecte un hook et installe des adaptateurs à l’exécution : renseignez-vous sur les règles du client. La durée du tampon logiciel n’est pas une mesure de latence de bout en bout.

`Test-ASIO-Device.cmd` permet un test gauche/droite facultatif. Fermez le jeu et les autres applications ASIO, puis baissez le volume d’écoute. Pour partager, utilisez le ZIP propre d’origine, sans journaux, marqueurs de périphérique ni préférences de langue.

## Español

1. Al abrir `config.exe` u `osu!asio_sound.exe` por primera vez, elige el idioma. Para cambiarlo después, abre `Change-Language.cmd` y vuelve a abrir las ventanas de la herramienta.
2. En `config.exe`, elige tu propio controlador ASIO y guarda. Valores de publicación: 16 muestras, Driver ID 0, 48000 Hz, volumen independiente 50%, Debug Log 0. El dispositivo 0 es un valor inicial que debes comprobar.
3. Ejecuta `osu!asio_sound.exe`, deja abierta la ventana de espera e inicia osu!lazer de forma normal. Entra en el mapa cuando se confirme el acoplamiento. La herramienta no inicia, copia ni modifica el cliente del juego.
4. Al cerrar el juego, se libera el dispositivo ASIO y la herramienta sigue esperando. Reinicia el juego antes de volver a acoplar tras cerrar la herramienta. Para un EXE del juego renombrado, usa la entrada `select osu exename` e introduce el nombre del archivo del proceso.

El control Hitsound del juego al 0% permite que ASIO siga sonando. Ajusta `Output Volume` (0–100) en el `config.ini` de la herramienta: se actualiza aproximadamente 0,5 segundos después de guardar y 0 silencia los sonidos ASIO. Cambiar el controlador, el búfer o la frecuencia requiere reiniciar el juego. La música y los sonidos de interfaz mantienen la salida nativa; se conservan la ganancia del mapa y el panorama. El idioma se guarda por separado en `language.ini`.

Conserva `managed`, `locales`, las DLL y el backend. Puedes renombrar la carpeta exterior. Implementaciones revisadas: .NET10 Game2026.1026/1110 y Framework2026.1019/1030; una implementación desconocida puede rechazarse. La herramienta inyecta un hook e instala adaptadores en tiempo de ejecución: conoce las reglas del cliente antes de usarla. La duración del búfer de software no es una latencia de extremo a extremo medida.

`Test-ASIO-Device.cmd` reproduce tonos izquierdo/derecho opcionales. Cierra antes el juego y otras aplicaciones ASIO y baja el volumen de escucha. Comparte el ZIP limpio original, sin registros, marcadores de dispositivo ni preferencias de idioma.
