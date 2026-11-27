# LazerASIO — v1.3.1

[简体中文](#简体中文) · [繁體中文](#繁體中文) · [English](#english) · [Français](#français) · [Español](#español)

Based on [Mother-Ship/osu-lazer-External-ASIO-Sound](https://github.com/Mother-Ship/osu-lazer-External-ASIO-Sound), whose upstream is [XTXTMTXTX/osu-External-ASIO-Sound](https://github.com/XTXTMTXTX/osu-External-ASIO-Sound). This is an independently maintained modification, not an official upstream release. Original authorship and third-party rights remain with their respective owners.

This package does not include the game, account data or beatmaps. `LazerASIO.exe` is the only entry point: first-use setup, settings, live volume and the game watcher share one window. Reviewed references include Game `2026.1026.0-tachyon` / `2026.1110.0-tachyon` and Framework `2026.1019.0` / `2026.1030.0`.

## 简体中文

契约模式固定为Adaptive（`Contract Mode = 1`），界面不提供Strict，手动改成 `0` 也会被恢复为Adaptive。Adaptive要求结构与关键语义通过，精确哈希未匹配时显示 `[Compat/Adaptive]` 并允许附加；匹配显示 `[Verified Contract]`，失败会拒绝并在managed日志列出Tier1/2/3详情。旧五字段INI同样按Adaptive处理。Adaptive只容忍外围方法或NOP等有限变化，不保证任意新版本或账号安全。


1. 打开 `LazerASIO.exe`。首次使用会进入初始设置向导：选择界面语言、自己的ASIO设备（可试听左右声道）、缓冲大小和采样率，以及ASIO打击音音量。完成时工具会实际打开一次设备检测参数，再保存到 `config.ini`。发布默认：16 samples、Driver ID 0、48000 Hz、独立音量50%、Debug Log 0、Contract Mode 1（Adaptive）；0号只是初始占位，不代表已为你选好声卡。
2. 主界面默认自动开始监听（可在「高级设置」关闭），显示「等待 osu!lazer」时照常启动自己的osu!lazer。状态变为「已附加就绪」并显示契约标签后再进入谱面。工具不启动、复制或修改游戏客户端。
3. 游戏退出后ASIO设备释放，工具继续等待。监听中关闭窗口时可选择最小化到托盘；真正退出或「停止监听」会让打击音回到游戏原生输出，若要重新接管请重启游戏。工具按游戏文件（.NET运行时与 osu.Game/osu.Framework）识别客户端，EXE改名也无需设置，并会等游戏的 .NET 运行时启动完成后才附加。
4. 右上角可随时切换语言，界面立即生效。

在已核查的音频实现中，游戏内Hitsound=0%仍可有ASIO击打声。1115的HitsoundVolume绑定到GameplayMixer.Volume，桥接器将其作为用户音量排除；这不保证任意未来版本。工具退出、故障或单通道降级后会回到游戏原生输出，Hitsound=0%时该回退会无声。ASIO音量用主界面的音量条调节（0～100）：附加期间拖动即实时生效，松手后写回 `config.ini`；也可直接编辑 `config.ini` 的 `Output Volume`，保存后约0.5秒生效；0会静音ASIO。驱动、buffer和采样率变更需重启游戏，界面会显示「需重启游戏生效」。BGM与UI音效保留原生，谱面增益和声像保留。`language.ini` 单独保存语言，`gui.ini` 保存窗口偏好，二者都不改变六项音频配置。

本包不包含游戏客户端、账号数据或谱面。`managed`、`locales` 与DLL/后端都是运行依赖，请保留。可自由改最外层文件夹名。已审查的是.NET10的Game2026.1026/1110、Framework2026.1019/1030音频实现族；精确契约按音频实现哈希比对，音频实现未变的更新版本同样显示 `[Verified Contract]`，未知实现可能被拒绝。「高级设置」的Debug日志对应 `Debug Log = 1`，记录5秒状态与错误；「逐事件追踪」会在工具目录创建 `playback-trace.enabled`。工具会向游戏进程注入Hook并安装运行时适配，请自行了解客户端规则。软件buffer时长不是实测端到端延迟。

「试听左声道/右声道」会播放轻声测试音，先退出游戏和其他ASIO程序并降低监听音量；附加期间设备由后端占用，试听不可用。运行生成的日志、`.asio-device-configured`、`language.ini` 与 `gui.ini` 属于本机状态；分享请使用原始干净ZIP。

## 繁體中文

契約模式固定為Adaptive（`Contract Mode = 1`），介面不提供Strict，手動改成 `0` 也會恢復為Adaptive。Adaptive要求結構與關鍵語意通過；精確雜湊不同時顯示 `[Compat/Adaptive]` 並允許附加。符合時顯示 `[Verified Contract]`；失敗則拒絕並在managed記錄列出Tier1/2/3。舊五欄位INI同樣以Adaptive處理。此模式僅容許外圍方法或NOP等有限變動，不保證任意新版或帳號安全。


1. 開啟 `LazerASIO.exe`。首次使用會進入初始設定精靈：選擇介面語言、自己的ASIO裝置（可試聽左右聲道）、緩衝區大小與取樣率，以及ASIO打擊音音量。完成時工具會實際開啟一次裝置檢測參數，再儲存至 `config.ini`。發佈預設：16 samples、Driver ID 0、48000 Hz、獨立音量50%、Debug Log 0、Contract Mode 1（Adaptive）。編號0只是初始值，仍須確認自己的裝置。
2. 主畫面預設自動開始監聽（可在「進階設定」關閉），顯示「等待 osu!lazer」時照常啟動自己的osu!lazer。狀態變為「已附加就緒」並顯示契約標籤後再進入譜面。工具不會啟動、複製或修改遊戲用戶端。
3. 遊戲結束後會釋放ASIO裝置，工具繼續等待。監聽中關閉視窗時可選擇縮小到系統匣；真正結束或「停止監聽」會讓打擊音回到遊戲原生輸出，若要重新接管請重新啟動遊戲。工具依遊戲檔案（.NET執行階段與 osu.Game/osu.Framework）辨識用戶端，EXE改名也無需設定，並會等遊戲的 .NET 執行階段啟動完成後才附加。
4. 右上角可隨時切換語言，介面立即生效。

在已核查的音訊實作中，遊戲Hitsound=0%時ASIO仍可播放。1115的HitsoundVolume綁定至GameplayMixer.Volume，配接器會排除此使用者音量；這不保證任意未來版本。工具退出、故障或單聲道降級後改回遊戲原生輸出，Hitsound=0%時該回退將無聲。ASIO音量以主畫面的音量條調整（0～100）：附加期間拖曳即時生效，放開後寫回 `config.ini`；也可直接編輯 `config.ini` 的 `Output Volume`，儲存後約0.5秒更新；0會靜音。驅動程式、緩衝區及取樣率變更需要重開遊戲，介面會顯示「需重新啟動遊戲」。BGM與UI音效維持原生，保留譜面增益與聲像。語言偏好另存於 `language.ini`，視窗偏好存於 `gui.ini`。

請保留 `managed`、`locales` 及必要DLL/後端；最外層資料夾可改名。已審查的是.NET10的Game2026.1026/1110及Framework2026.1019/1030音訊實作族；精確契約依音訊實作雜湊比對，音訊實作未變的較新版本同樣顯示 `[Verified Contract]`，未知實作可能被拒絕。「進階設定」的Debug記錄對應 `Debug Log = 1`，記錄5秒狀態與錯誤；「逐事件追蹤」會在工具資料夾建立 `playback-trace.enabled`。工具會向遊戲注入Hook及安裝執行階段配接器，請自行了解用戶端規則。軟體緩衝區時長並非實測端對端延遲。

「試聽左聲道/右聲道」會播放輕聲測試音；請先退出遊戲與其他ASIO程式並調低監聽音量。附加期間裝置由後端佔用，無法試聽。分享時使用原始ZIP，勿夾帶執行記錄、裝置標記、語言或視窗偏好。

## English

The contract mode is fixed to Adaptive (`Contract Mode = 1`); the window does not offer Strict and restores a hand-edited `0` to Adaptive. Adaptive requires structural and critical semantic checks. Exact-hash drift is allowed only after they pass and displays `[Compat/Adaptive]`. Exact matches display `[Verified Contract]`; failures reject attachment and list Tier1/2/3 details in the managed log. Old five-field INIs are treated as Adaptive. Adaptive tolerates only bounded peripheral/NOP changes and is not a general compatibility or account-safety guarantee.


1. Open `LazerASIO.exe`. On first use a setup wizard asks for the interface language, your own ASIO device (with left/right test tones), buffer size and sample rate, and the ASIO hitsound volume. Finishing opens the device once to check the settings, then saves them to `config.ini`. Clean defaults: 16 samples, Driver ID 0, 48000 Hz, independent volume 50%, Debug Log 0, Contract Mode 1 (Adaptive). Device 0 is an initial value, not an automatically verified selection.
2. The main window starts monitoring automatically (this can be turned off under Advanced). While it shows "Waiting for osu!lazer", start osu!lazer normally. Enter a beatmap once the status reads "Attached and ready" with a contract badge. The tool does not start, copy or modify the game client.
3. The ASIO device is released when the game exits; the tool keeps waiting. Closing the window while monitoring offers to keep running in the notification area; quitting or pressing Stop returns hitsounds to native output, and the game must be restarted before attaching again. The client is recognized by its game files (the .NET runtime plus osu.Game/osu.Framework), so a renamed EXE needs no setting, and attachment waits until the game's .NET runtime has started.
4. The language menu in the top-right corner switches the interface immediately.

On the inspected implementations, the game Hitsound slider at 0% still allows ASIO hitsounds. In 1115, HitsoundVolume binds GameplayMixer.Volume, which the bridge excludes as user volume; arbitrary future clients are not guaranteed. When the tool exits, fails, or rejects a single channel, that sound returns to native output, which is silent if game Hitsound is 0%. Set the ASIO volume (0–100) with the slider in the main window: while attached it applies live as you drag and is written back to `config.ini` when you release. Editing `Output Volume` in `config.ini` directly still works and applies about 0.5 seconds after saving; 0 mutes ASIO. Driver/buffer/sample-rate changes require a game restart, which the window flags as "Restart game to apply". Music and UI sounds retain native output; beatmap gain and pan are preserved. Language is saved separately in `language.ini` and window preferences in `gui.ini`, keeping all six audio fields.

Keep `managed`, `locales`, the DLLs and the backend. The outer folder may be renamed. Reviewed audio implementation families: .NET10 Game2026.1026/1110 and Framework2026.1019/1030. The exact contract hashes the audio implementation, so a later build with unchanged audio code still shows `[Verified Contract]`; unknown implementations may be rejected. The Debug log switch under Advanced is `Debug Log = 1` and records five-second status and errors; Per-event trace creates `playback-trace.enabled` beside the tool. This tool injects a hook and installs runtime adapters into the game process; understand the client's rules before use. Software buffer duration is not measured end-to-end latency.

"Test left/right" plays quiet test tones. Close the game and other ASIO applications and lower your listening volume first; while attached the backend owns the device and tests are disabled. Share the original clean ZIP, without generated logs, device markers, language or window preferences.

## Français

Le mode de contrat est fixé sur Adaptive (`Contract Mode = 1`) ; la fenêtre ne propose pas Strict et remet un `0` saisi à la main sur Adaptive. Adaptive exige une structure et une sémantique critique conformes. Une différence de hachage exact n’est acceptée qu’après ces contrôles et affiche `[Compat/Adaptive]`. Une correspondance affiche `[Verified Contract]` ; un échec refuse le rattachement et détaille Tier1/2/3 dans le journal géré. Les anciens INI à cinq champs sont traités comme Adaptive. Adaptive ne tolère que certaines modifications périphériques/NOP et ne garantit ni toute nouvelle version ni la sécurité du compte.


1. Ouvrez `LazerASIO.exe`. Au premier lancement, un assistant demande la langue, votre propre périphérique ASIO (avec sons de test gauche/droite), la taille du tampon et la fréquence, puis le volume ASIO des hitsounds. À la fin, l’outil ouvre une fois le périphérique pour vérifier ces réglages, puis les enregistre dans `config.ini`. Valeurs de publication : 16 échantillons, Driver ID 0, 48000 Hz, volume indépendant 50 %, Debug Log 0, Contract Mode 1 (Adaptive). L’identifiant 0 est une valeur initiale à vérifier.
2. La fenêtre principale démarre la surveillance automatiquement (désactivable dans Avancé). Pendant « En attente d’osu!lazer », lancez osu!lazer normalement. Attendez « Rattaché et prêt » avec le badge de contrat avant d’ouvrir une beatmap. L’outil ne démarre pas, ne copie pas et ne modifie pas le client du jeu.
3. Le périphérique ASIO est libéré à la fermeture du jeu et l’outil continue d’attendre. Fermer la fenêtre pendant la surveillance propose de rester dans la zone de notification ; quitter ou Arrêter renvoie les hitsounds à la sortie native, et un nouveau rattachement exige de redémarrer le jeu. Le client est reconnu à ses fichiers de jeu (runtime .NET, osu.Game/osu.Framework) : un EXE renommé ne demande aucun réglage, et le rattachement attend que le runtime .NET du jeu ait démarré.
4. Le menu de langue en haut à droite change l’interface immédiatement.

Sur les implémentations inspectées, le curseur Hitsound à 0 % permet les sons ASIO. Dans la version 1115, HitsoundVolume est lié à GameplayMixer.Volume, exclu comme volume utilisateur par l’adaptateur ; cela ne garantit pas les futures versions. À la fermeture ou en cas de panne de l’outil, ou si un canal est refusé, ses sons reviennent à la sortie native, silencieuse si Hitsound est à 0 %. Réglez le volume ASIO (0–100) avec le curseur de la fenêtre principale : pendant le rattachement il s’applique en direct pendant le glissement, puis est écrit dans `config.ini` au relâchement. Modifier `Output Volume` dans `config.ini` fonctionne toujours et s’applique environ 0,5 seconde après l’enregistrement ; 0 coupe ces sons. Un changement de pilote, tampon ou fréquence nécessite un redémarrage du jeu, signalé par « Redémarrer le jeu pour appliquer ». La musique et les sons d’interface restent sur la sortie native ; gain de beatmap et panoramique sont conservés. La langue est enregistrée dans `language.ini` et les préférences de fenêtre dans `gui.ini`.

Conservez `managed`, `locales`, les DLL et le backend. Le dossier extérieur peut être renommé. Familles d’implémentation audio vérifiées : .NET10 Game2026.1026/1110 et Framework2026.1019/1030. Le contrat exact compare l’empreinte de l’implémentation audio : une version plus récente dont le code audio est inchangé affiche aussi `[Verified Contract]` ; une implémentation inconnue peut être refusée. Le journal de débogage d’Avancé correspond à `Debug Log = 1` (état toutes les cinq secondes et erreurs) ; la trace par événement crée `playback-trace.enabled` à côté de l’outil. L’outil injecte un hook et installe des adaptateurs à l’exécution : renseignez-vous sur les règles du client. La durée du tampon logiciel n’est pas une mesure de latence de bout en bout.

« Tester gauche/droite » joue des sons de test discrets. Fermez le jeu et les autres applications ASIO, puis baissez le volume d’écoute ; pendant le rattachement, le backend occupe le périphérique et les tests sont désactivés. Pour partager, utilisez le ZIP propre d’origine, sans journaux, marqueurs de périphérique ni préférences.

## Español

El modo de contrato está fijado en Adaptive (`Contract Mode = 1`); la ventana no ofrece Strict y devuelve a Adaptive un `0` editado a mano. Adaptive exige estructura y semántica crítica válidas. Las diferencias del hash exacto solo se aceptan tras esos controles y muestran `[Compat/Adaptive]`. Una coincidencia muestra `[Verified Contract]`; si falla, se rechaza el acoplamiento y se detallan Tier1/2/3 en el registro administrado. Los INI antiguos de cinco campos se tratan como Adaptive. Adaptive solo tolera cambios periféricos/NOP limitados y no garantiza cualquier versión nueva ni la seguridad de la cuenta.


1. Abre `LazerASIO.exe`. La primera vez, un asistente pide el idioma, tu propio dispositivo ASIO (con tonos de prueba izquierda/derecha), el tamaño del búfer y la frecuencia, y el volumen ASIO de los hitsounds. Al finalizar, la herramienta abre el dispositivo una vez para comprobar los ajustes y los guarda en `config.ini`. Valores de publicación: 16 muestras, Driver ID 0, 48000 Hz, volumen independiente 50%, Debug Log 0, Contract Mode 1 (Adaptive). El dispositivo 0 es un valor inicial que debes comprobar.
2. La ventana principal inicia la supervisión automáticamente (se puede desactivar en Avanzado). Mientras muestre «Esperando osu!lazer», inicia osu!lazer de forma normal. Entra en el mapa cuando el estado indique «Acoplado y listo» con la insignia del contrato. La herramienta no inicia, copia ni modifica el cliente del juego.
3. Al cerrar el juego, se libera el dispositivo ASIO y la herramienta sigue esperando. Cerrar la ventana durante la supervisión ofrece seguir en el área de notificación; salir o pulsar Detener devuelve los hitsounds a la salida nativa y volver a acoplar exige reiniciar el juego. El cliente se reconoce por sus archivos de juego (runtime .NET, osu.Game/osu.Framework), así que un EXE renombrado no necesita ajustes, y el acoplamiento espera a que el runtime .NET del juego haya arrancado.
4. El menú de idioma de la esquina superior derecha cambia la interfaz al instante.

En las implementaciones inspeccionadas, Hitsound al 0% permite que ASIO siga sonando. En 1115, HitsoundVolume se vincula a GameplayMixer.Volume y el adaptador lo excluye como volumen de usuario; no se garantiza cualquier versión futura. Al cerrar o fallar la herramienta, o al rechazar un canal, sus sonidos vuelven a la salida nativa, que queda en silencio si Hitsound está al 0%. Ajusta el volumen ASIO (0–100) con el control de la ventana principal: mientras está acoplado se aplica en vivo al arrastrar y se escribe en `config.ini` al soltar. Editar `Output Volume` en `config.ini` sigue funcionando y se aplica unos 0,5 segundos después de guardar; 0 silencia los sonidos ASIO. Cambiar el controlador, el búfer o la frecuencia requiere reiniciar el juego, y la ventana lo indica con «Reinicia el juego para aplicar». La música y los sonidos de interfaz mantienen la salida nativa; se conservan la ganancia del mapa y el panorama. El idioma se guarda en `language.ini` y las preferencias de ventana en `gui.ini`.

Conserva `managed`, `locales`, las DLL y el backend. Puedes renombrar la carpeta exterior. Familias de implementación de audio revisadas: .NET10 Game2026.1026/1110 y Framework2026.1019/1030. El contrato exacto compara la huella de la implementación de audio: una versión posterior con el código de audio sin cambios también muestra `[Verified Contract]`; una implementación desconocida puede rechazarse. El registro de depuración de Avanzado corresponde a `Debug Log = 1` (estado cada cinco segundos y errores); el seguimiento por evento crea `playback-trace.enabled` junto a la herramienta. La herramienta inyecta un hook e instala adaptadores en tiempo de ejecución: conoce las reglas del cliente antes de usarla. La duración del búfer de software no es una latencia de extremo a extremo medida.

«Probar izquierda/derecha» reproduce tonos de prueba suaves. Cierra antes el juego y otras aplicaciones ASIO y baja el volumen de escucha; mientras está acoplado, el backend ocupa el dispositivo y las pruebas están desactivadas. Comparte el ZIP limpio original, sin registros, marcadores de dispositivo ni preferencias.
