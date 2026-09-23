# Convalida build PyroWave — server Windows

> **PW-X extension status (23 September 2026):** This document records the original SDR 4:2:0 phase at commit `205c510c`. Its limits, hashes and test results are historical. For the extended implementation, use the [capability matrix](pyrowave-capability-matrix.md), [protocol v2](pyrowave-protocol-v2.md) and [extension build report](pyrowave-extension-build.md). The earlier results do not qualify HDR, 4:4:4 or the new transport/runtime paths.


Data: 23 settembre 2026. Repository base: `8bf0ef7d3dbc0402e553deb93bb52e50447c4225`, con modifiche locali dell'implementazione PyroWave. Questo report riguarda compilazione, dipendenze e staging; le prove GPU sono descritte in [pyrowave-gpu-validation.md](pyrowave-gpu-validation.md). Non costituisce una dichiarazione di completamento dell'intero piano server.

## Risultati osservati

| Verifica | Esito |
| --- | --- |
| Configurazione Windows x64, `SUNSHINE_ENABLE_PYROWAVE=OFF` | Superata |
| Configurazione Windows x64, `SUNSHINE_ENABLE_PYROWAVE=ON` | Superata |
| Compilazione e link del target `sunshine`, feature OFF | Superata |
| Compilazione e link del target `sunshine`, feature ON | Superata; DLL copiata accanto a `sunshine.exe` |
| Build isolata della C API upstream e compilazione di un consumatore dei soli header | Superata |
| Build isolata incrementale senza modifiche | Nessun lavoro residuo |
| Dipendenza PyroWave nel percorso OFF | Nessuna directory `_deps/pyrowave`, macro, include o sorgente runtime PyroWave nei comandi del server |
| Tabelle import PE del server OFF e ON | Nessun import PyroWave o Vulkan |
| Tabella import PE della DLL upstream | Solo librerie Windows di sistema; nessun import del loader Vulkan |
| Component test mirati | 8 eseguibili, 71 test individuali superati nell'ultima esecuzione riportata sotto |
| Staging locale del componente `application`, OFF | Superato; nessuna DLL o directory licenze PyroWave |
| Staging locale del componente `application`, ON | Superato; DLL e 8 file di licenze, manifest e patch locale presenti e non vuoti |

L'opzione è disattivata per default. La build abilitata compila PyroWave come progetto CMake isolato e collega al server soltanto un target di header. L'import library della DLL non è collegata al server. La libreria viene caricata dal runtime interno quando serve, con controllo ABI e del contratto locale che identifica revisione e patch di ownership degli handle NT.

Lo staging controllato è una copia in `build/pyrowave-package-check`, tramite le regole `install` del progetto. Non installa il servizio, non avvia il server e non distribuisce un installer. La creazione e validazione di un MSI completo, le firme e il rilascio restano verifiche distinte.

Il solo componente `application` non è autonomo: non include gli shader e l'interfaccia web. È stata inoltre prodotta e controllata una directory sperimentale separata con `application`, `assets` e `Unspecified`, descritta sotto.

## Toolchain e revisioni

- Windows x64, MSYS2 UCRT64, GCC/G++ 15.2.0.
- CMake 4.3.1 e Ninja disponibili in `C:/msys64/ucrt64/bin`.
- Boost 1.89.0, scaricato dal meccanismo già presente nel progetto.
- FFmpeg della baseline `third-party/build-deps`, release `v2026.516.30821`.
- MSVC non è stato usato per questi risultati.

Le dipendenze opzionali sono fissate in `cmake/dependencies/pyrowave-pins.cmake`. Gli archivi sono verificati prima dell'estrazione; Granite corrisponde alla revisione richiesta da `checkout_granite.sh` di PyroWave, volk e Vulkan-Headers ai gitlink di quella revisione di Granite.

| Dipendenza | Commit | SHA-256 archivio |
| --- | --- | --- |
| PyroWave | `d2997ac172bdc00e29c58e3f2938acb7e94580bf` | `f571c94512225509b3b5c73caa99cb79a1100d19facc390553338e47234163f0` |
| Granite | `9d44761debb9ac31d8d800cac8b030a7a0390b7e` | `286823300b7ee8b49694361287e3bf37c36c1e4c3663860d9f09655f1d860498` |
| volk | `47cddf7ed97b94118a08aacb548a411188e016cc` | `ed771f9132ea077af0abc74d29f415a81f181f83c3ac5e0f67b2f2149976fe2d` |
| Vulkan-Headers | `11d6898377797e07dbd543aaaa367e4465074597` | `162e7e95e101dfe3118bef4abd0dd2a38f8308e5c61f60bfcef8c0e8783fc48c` |

La C API è 0.5.0. Gli shader SPIR-V sono già contenuti nel file upstream `shaders/slangmosh.hpp` della revisione fissata; la build non scarica un compilatore shader. Le opzioni upstream impiegano aritmetica FP32, storage a intervallo ridotto, strumenti di sviluppo disattivati e RenderDoc disattivato. Il loader e il driver Vulkan sono forniti dal sistema e non vengono inclusi nel pacchetto.

Il patchset locale `cmake/patches/pyrowave-0.5.0-nt-handle-ownership.patch` ha SHA-256 `ba9f00d2fda290d4fd93d5792b3f08100eb12e9e8fb92add5012a2e76302fc8d`. Corregge il trasferimento degli handle NT soltanto dopo import riuscito, libera la memoria Vulkan se il binding fallisce e aggiunge l'export `pyrowave_vibepollo_runtime_contract`. Il runtime accetta soltanto il valore `d2997ac172bdc00e29c58e3f2938acb7e94580bf;nt-handle-ownership-v1`; una DLL upstream con la sola ABI 0.5.0 non è sufficiente. L'export è incluso anche nella tabella `.def` usata da MinGW. Le firme dell'API upstream e il bitstream restano invariati.

Lo script di applicazione controlla l'hash e l'applicabilità, accetta in modo idempotente una patch già applicata e non modifica l'indice Git. Le revisioni, gli hash degli archivi e l'hash della patch determinano un nuovo percorso sorgenti/build: una variazione del pin non riusa i marker di una precedente estrazione. Sono state provate applicazione, seconda applicazione e rifiuto di hash errato in una directory isolata. La copia della DLL accanto al server è una dipendenza sempre verificata, anche quando il server non richiede un nuovo link.

Sul sorgente finale estratto in `build/pyrowave-server-on/_deps/pyrowave/src-ed2d2563f22d`, `verify_nt_handle_ownership.py` supera 13 casi deterministici di ownership e verifica l'identità esatta e la tabella export. Questa prova usa i corpi C API reali con adattatori di errore senza driver; le prove con driver sono riportate nel documento GPU.

## Comandi di compilazione

Eseguire dalla radice del repository in PowerShell. Le directory sono separate per evitare di confondere i risultati OFF e ON.

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$pyrowaveBuildOptions = @(
    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    '-DSUNSHINE_ENABLE_WEBRTC=OFF', '-DBUILD_TESTS=OFF', '-DBUILD_DOCS=OFF',
    '-DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=OFF',
    '-DBUILD_VIRTUALDISPLAY_PROBE=OFF', '-DBUILD_VIRTUALDISPLAY_TOOLS=OFF',
    '-DBUILD_VIRTUALDISPLAY_VULKAN_LAYER=OFF'
)
cmake -S . -B build/pyrowave-server-off @pyrowaveBuildOptions -DSUNSHINE_ENABLE_PYROWAVE=OFF
cmake --build build/pyrowave-server-off --target sunshine -j 4

cmake -S . -B build/pyrowave-server-on @pyrowaveBuildOptions -DSUNSHINE_ENABLE_PYROWAVE=ON
cmake --build build/pyrowave-server-on --target sunshine -j 4 -- -d keeprsp
```

Durante la verifica, la configurazione ON ha riutilizzato i sorgenti Boost già scaricati mediante `FETCHCONTENT_SOURCE_DIR_BOOST=<repository>/build/pyrowave-server-off/_deps/boost-src`, una copia della cache FFmpeg in `build/pyrowave-server-on/_deps/ffmpeg` e gli archivi PyroWave nella cache `SUNSHINE_PYROWAVE_DOWNLOAD_DIR=<repository>/build/pyrowave-build-check/downloads`. Queste accelerazioni non modificano revisioni o controlli hash. Sono facoltative per una build da zero.

`-d keeprsp` conserva il file di risposta del linker per l'harness interno che riusa gli oggetti del server. Non è richiesto per una build normale. Non eseguire due processi Ninja nella stessa directory di build.

## Test mirati

```powershell
cmake -S . -B build/pyrowave-server-off -DBUILD_TESTS=ON "-DSUNSHINE_TEST_GTEST_SOURCE_DIR=$PWD/third-party/libdisplaydevice/third-party/googletest"
cmake --build build/pyrowave-server-off --target test_component_stream_protocol test_component_rtsp_startup_snapshot test_component_pyrowave_protocol test_component_pyrowave_negotiation test_component_pyrowave_transport test_component_video_policy test_component_encoder_probe_policy test_component_resource_config_catalog -j 4
ctest --test-dir build/pyrowave-server-off --output-on-failure -R '^test_component_(stream_protocol|rtsp_startup_snapshot|pyrowave_protocol|pyrowave_negotiation|pyrowave_transport|video_policy|encoder_probe_policy|resource_config_catalog)$'
```

Prima esecuzione: 7/7 componenti superati, 60 test individuali. Ultima esecuzione, dopo l'integrazione del packetizer realmente riusato dal sender, della fixture degli header esatti, della policy di pacing e della correzione di ownership del desktop Windows: 8/8 componenti, 71 test individuali, 0,25 secondi per CTest. Dettaglio: trasporto base 8, snapshot RTSP 2, protocollo PyroWave 12, negoziazione PyroWave 14, trasporto PyroWave 8, policy video 19, probing encoder 6, catalogo configurazione 2. Il log locale è `build/pyrowave-server-off/Testing/Temporary/LastTest.log`.

Questi test non dimostrano da soli la correttezza di cattura WGC/DXGI, sincronizzazione GPU, decoder o streaming reale. Le verifiche aggiunte successivamente al primo passaggio richiedono un nuovo build e una nuova esecuzione.

Le prove con socket RTSP reali, incluso avvio e arresto di una sessione con callback del sistema operativo isolati, sono documentate in [pyrowave-rtsp-validation.md](pyrowave-rtsp-validation.md).

## Staging e licenze

```powershell
cmake --build build/pyrowave-server-off --target playnite-launcher sunshine_wgc_capture build_uninstall_ui -j 4
cmake --build build/pyrowave-server-on --target playnite-launcher sunshine_wgc_capture build_uninstall_ui -j 4
cmake --install build/pyrowave-server-off --prefix "$PWD/build/pyrowave-package-check/off" --component application
cmake --install build/pyrowave-server-on --prefix "$PWD/build/pyrowave-package-check/on" --component application
```

Nella build ON le regole installano `libpyrowave-shared-0.dll` dalla directory dell'eseguibile, conservando gli eventuali trattamenti del rilascio, e `licenses/pyrowave/` con:

- `PyroWave/LICENSE`, la patch locale riproducibile e `Granite/LICENSE`;
- `volk/LICENSE.md`;
- `Vulkan-Headers/LICENSE.md` e i testi MIT/Apache-2.0 in `Vulkan-Headers/LICENSES/`;
- `DEPENDENCIES.txt` con revisioni, versione API e opzioni pertinenti.

Entrambi i comandi di staging sono terminati con codice 0. Il SHA-256 della DLL installata coincide con quello della DLL accanto al server e nello staging della dipendenza: `d6105cbc2861df660b7116fc04270408aba28adc94db513be7127e9798645f73`. L'export `pyrowave_vibepollo_runtime_contract` è presente nella tabella PE della DLL effettivamente installata. Nel percorso OFF non sono presenti né la DLL né `licenses/pyrowave`. Gli otto file di licenza/manifest/patch della build ON sono tutti non vuoti.

Lo staging finale è stato aggiornato dopo la correzione del desktop Windows e del teardown GPU/probe. Gli eseguibili copiati coincidono con quelli dell'ultimo link: SHA-256 ON `09892b75b928b1d0396de73ee75c88526079d13d52b27be17b636f769c72cb0b`, OFF `03098de33340cba758e10fcff54c6ba90f3d6ba27a3c089a30b539ca842f351c`. Questi hash identificano gli artefatti locali, non una promessa di identità binaria fra build con timestamp/versioni generati diversi.

Il manifest locale `build/pyrowave-package-check/manifest.json` registra dimensioni e SHA-256 dei 6 file OFF e dei 15 file ON; `imports.json` registra le dipendenze PE dei due server e della DLL opzionale, oltre alla presenza dell'export di identità.

Sono state controllate anche le tabelle import con `C:/msys64/ucrt64/bin/objdump.exe -p <eseguibile-o-DLL>`. Non è stato eseguito il server di produzione per simulare l'assenza di Vulkan. Il rifiuto di ABI errata e le altre prove runtime sono registrati separatamente nella convalida GPU.

### Directory sperimentale con assets

I seguenti comandi sono stati eseguiti dopo la build finale. Gli script CMake generati sono stati prima controllati: copiano file e prevedono soltanto un eventuale strip, non eseguono setup del servizio o driver.

```powershell
cmake --build build/pyrowave-server-on --target web_ui -j 4
cmake --install build/pyrowave-server-on --prefix "$PWD/build/pyrowave-portable-on" --component application
cmake --install build/pyrowave-server-on --prefix "$PWD/build/pyrowave-portable-on" --component assets
cmake --install build/pyrowave-server-on --prefix "$PWD/build/pyrowave-portable-on" --component Unspecified
```

Esito: tutti i comandi terminati con codice 0; Web UI finale ricompilata con 3055 moduli legacy e 148 moduli v2. La directory contiene 207 file, inclusi `sunshine.exe`, DLL opzionale, licenze/patch, helper WGC, entrambe le interfacce web e 46 file HLSL/HLSLI. Il manifest con dimensioni e SHA-256 è `build/pyrowave-package-check/portable-manifest.json`; i log di copia sono `portable-install-application.log`, `portable-install-assets.log` e `portable-install-Unspecified.log` nella stessa directory. Non sono stati avviati eseguibili o script dalla directory prodotta. Questa prova dimostra la produzione degli artefatti sperimentali; installazione, firma, avvio del pacchetto e integrazione MSI restano da verificare.

## Limiti e correzioni emerse durante la build

- Un primo tentativo OFF ha letto `config.h` mentre veniva sostituito da un altro lavoro attivo, producendo ridefinizioni. Il successivo build della stessa unità è passato senza modifiche correttive al file.
- Le build ON hanno individuato variabili locali chiamate `error` che nascondevano il logger usato da `BOOST_LOG(error)`. I nomi locali sono stati corretti nei percorsi GPU e capture; il successivo link del server ON è passato.
- La verifica della tabella PE ha trovato inizialmente l'export di identità assente sotto MinGW: la dichiarazione pubblica non aggiornava la tabella `.def` esplicita. Il patchset finale include anche questa tabella; il controllo dell'export viene ripetuto sulla DLL effettivamente distribuita.
- La configurazione verificata usa WebRTC disattivato e il valore predefinito `BUILD_WERROR=OFF`. Sono comparsi warning in Boost, funzioni WebRTC inutilizzate e `display_settings_helper_v2.cpp`; non vengono presentati come un build con tutti i warning trattati come errori.
- Linux, macOS, ARM64, MSVC, MSI completo, firma del pacchetto e avvio con driver Vulkan assente non sono dimostrati da queste compilazioni.
- Dopo modifiche successive alle unità coinvolte, ripetere i target interessati e aggiornare le evidenze in Vikunja prima di dichiarare completato il criterio corrispondente.
