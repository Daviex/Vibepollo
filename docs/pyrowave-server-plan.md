# Piano di implementazione PyroWave — server VibePollo

Data: 23 settembre 2026. Baseline VibePollo analizzata: `8bf0ef7d3dbc0402e553deb93bb52e50447c4225`.

## Obiettivo e stato

Integrare PyroWave nel server VibePollo come codec sperimentale opzionale, preparandone il contratto per il successivo sviluppo del client.

### Estensione completa richiesta il 23 settembre 2026 — in corso

La richiesta successiva amplia l'obiettivo a tutte le capacità attualmente offerte da PyroWave. Il confronto con upstream conferma che `master` coincide ancora con il pin `d2997ac172bdc00e29c58e3f2938acb7e94580bf`. La [matrice delle capacità](pyrowave-capability-matrix.md) registra funzionalità, prove disponibili e lavoro ancora aperto. Le evidenze della prima fase riportate sotto riguardano **SDR 4:2:0** e non qualificano automaticamente i nuovi percorsi.

Il piano operativo aggiunto in **Vikunja → VibePollo → PyroWave — Server** è:

| Attività | ID Vikunja | Risultato richiesto |
| --- | --- | --- |
| PW-X00 | 167 | Inventario completo e criteri di completamento |
| PW-X01 | 168 | Profili colore espliciti e negoziazione v2 compatibile con v1 |
| PW-X02 | 169 | Metadati VUI reali e piani GPU 8/16 bit, 420/444 |
| PW-X03 | 170 | Conversione GPU SDR, wide gamut, PQ/HDR e chroma siting |
| PW-X04 | 171 | Cattura, probe per profilo, capability e metadati HDR |
| PW-X05 | 172 | Aggiornamento bitrate per fotogramma senza ricreare l'encoder |
| PW-X06 | 173 | Frammenti nativi, recupero parziale e protezione dei dati critici |
| PW-X07 | 174 | Precisione wavelet configurabile e diagnostica GPU |
| PW-X08 | 175 | Contesti per adattatore e più encoder |
| PW-X09 | 176 | Backend host Linux/ARM Vulkan |
| PW-X10 | 177 | Backend host macOS Metal con capacità specifiche upstream |
| PW-X11 | 178 | Build, documentazione, packaging e audit finale della copertura |

La prima sequenza di lavoro è X00 → X01 → X02/X03 → X04; X05 e X06 estendono il trasporto, X07/X08 il runtime. I backend X09/X10 devono pubblicare le proprie capacità effettive. X11 dipende dall'intero inventario: attività incomplete e hardware non qualificato restano visibili. Il client applicativo continua a essere la fase successiva; contratto e dati di riferimento sono parte del lavoro server.

### Prima fase sperimentale — baseline storica

La pianificazione comprende una attività di analisi (PW-S00) e undici attività implementative (PW-S01–PW-S11), collegate da 23 dipendenze in Vikunja. La fase server sperimentale è implementata sul branch `codex/pyrowave-server`, con build Windows x64 OFF/ON, pacchetto locale e contratto per la successiva fase client. Le evidenze e i limiti sono registrati nelle rispettive attività e nei report.

La verifica finale comprende 71 test dei componenti, 21 casi RTSP ON e 6 OFF, cattura e decodifica di 45 fotogrammi per ciascuno dei percorsi DXGI e WGC, e 13 probe consecutivi con 437 handle invariati. Sono stati corretti i problemi di colore e di gestione delle risorse emersi durante le prove. Il [report GPU](pyrowave-gpu-validation.md) distingue i benchmark storici dalle regressioni sull'ultima build; il [report RTSP](pyrowave-rtsp-validation.md) documenta cattura→codifica→UDP→decoder di riferimento e arresto della sessione.

La matrice verificata è Windows x64, RTX 3080 Ti con driver `32.0.16.1714`, SDR 4:2:0, una sessione PyroWave. Il pacchetto sperimentale è stato prodotto senza installarlo o aggiornare il servizio in esecuzione. Restano qualificazioni successive: carico di gioco e uso GPU, prestazioni sostenute, altri driver/GPU, modalità fisiche del display, recupero da guasti del driver, MSI/firma e streaming end-to-end con l'app client. Questa consegna conclude l'integrazione server sperimentale su tale matrice; il progetto completo prosegue con il client.

Contratto implementativo: [PyroWave protocol v1](pyrowave-protocol-v1.md). Il profilo iniziale esplicita BT.709 full range e chroma siting left; questi metadati negoziati prevalgono sui valori predefiniti del packetizer upstream.

Il riferimento operativo è **Vikunja → VibePollo → PyroWave — Server**. Questo documento conserva ambito, decisioni da prendere e criteri di completamento; lo stato corrente delle attività e le evidenze di esecuzione sono mantenuti in Vikunja.

## Ambito iniziale

- Host Windows x64, cattura WGC/DXGI, conversione e codifica sulla GPU tramite interoperabilità D3D11/Vulkan.
- Primo profilo SDR YCbCr 4:2:0; risoluzioni, frequenze e bitrate supportati saranno dichiarati dopo le verifiche.
- Compilazione opzionale e attivazione esplicita, disabilitate per impostazione predefinita.
- Identità codec dedicata e negoziazione versionata con un futuro client compatibile.
- Riutilizzo da valutare del trasporto RTSP/GameStream, con autenticazione, controllo, input, audio, pacing, cifratura e FEC esistenti.
- Convalida server mediante strumenti interni e decoder upstream di riferimento.

L'app client sarà lo step successivo. HDR, 4:4:4, host Linux/macOS e integrazione browser WebRTC sono estensioni successive. SDR e 4:2:0 sono scelte di ambito del primo rilascio, non limiti dimostrati del formato.

PyroWave è open source MIT e usa un formato non standard. È un codec con perdita e solo frame intra. Le API che accettano buffer CPU usano comunque la GPU: non costituiscono un encoder software alternativo. Le prestazioni dichiarate dall'autore non dimostrano la latenza della pipeline completa di VibePollo.

## Vincoli emersi dall'analisi

| Area | Evidenza nella baseline | Conseguenza implementativa |
| --- | --- | --- |
| Identità codec | `src/video.h`: slot H.264/HEVC/AV1 e `codec_from_config()` con fallback a H.264 | Estendere il modello e gestire esplicitamente i valori sconosciuti |
| Selezione encoder | `src/video.cpp`: factory e dispatch specifici per backend; `validate_encoder()` verifica prima H.264 | Introdurre un backend e un probing adatti a PyroWave, senza simulare H.264 |
| Cattura | `src/platform/windows/display_wgc.cpp` e `display_vram.*`: risorse D3D11 e sincronizzazione con keyed mutex | Verificare condivisione delle immagini, fence, ownership e corrispondenza GPU/LUID |
| Negoziazione | `src/rtsp.cpp`: `bitStreamFormat`; capability nei percorsi HTTP e video | Definire identificatore e versione solo dopo aver fissato il contratto |
| Trasporto | `src/stream.cpp`: massimo 4 blocchi FEC, 255 shard totali per blocco con FEC; soglia di 1024 pacchetti per blocco oggi segnalata nel log | Calcolare un limite frame sicuro prima della codifica e impedire l'invio oltre i limiti |
| Frame grandi | `src/stream.cpp` disabilita FEC quando servirebbero più di 4 blocchi | Includere FEC e overhead nel budget ordinario del nuovo codec |
| Client esistenti | `third-party/moonlight-common-c` e percorsi WebRTC riconoscono i codec standard | Annunciare PyroWave solo a client che lo richiedono e ne supportano il contratto |
| Dipendenza upstream | C API 0.5.0 e bitstream in evoluzione; Granite e Vulkan | Fissare revisioni, verificare ABI, feature GPU e toolchain; conservare le licenze |

Il limite frame dovrà includere arrotondamenti per blocco, header, cifratura, MTU e parità FEC. Una formula basata sul solo bitrate medio non basta. Il trasferimento in CPU del bitstream compresso può essere necessario per la rete; il percorso dei pixel grezzi deve essere valutato separatamente per evitare copie GPU→CPU→GPU.

### Decisioni emerse dall'implementazione

- Il contratto v1 usa un frame completo PWVF e riutilizza il trasporto esistente con un piano esatto di shard, parità e cifratura.
- L'import diretto NV12 ha prodotto errori di chroma sulla GPU verificata. La conversione scrive quindi due texture condivise R8 e R8G8, senza readback dei pixel nel percorso di produzione.
- I cicli di creazione/distruzione di un dispositivo Vulkan minimo hanno mostrato crescita degli handle anche senza PyroWave. Il runtime sperimentale conserva un solo contesto e DLL per processo, associati al LUID; encoder, immagini e fence restano per sessione. Cambio GPU o perdita del dispositivo richiedono riavvio esplicito del server. La verifica ripetuta di questa correzione è registrata in PW-S05.
- Le patch locali di ownership/cleanup della dipendenza sono versionate e incluse nel manifest di build; non modificano il bitstream negoziato.
- La verifica ripetuta ha individuato anche una perdita preesistente degli handle desktop Windows. `syncThreadDesktop()` conserva ora un solo handle per thread, riusa l'identità invariata e chiude gli handle dopo averli scollegati dal thread. Le prove isolate della helper e quelle complete DXGI/WGC sono documentate nel report GPU.

## Sequenza e verifiche decisive

1. **PW-S01 — Contratto:** definire formato della negoziazione e del payload, limiti e comportamento degli errori. L'incapsulamento a frame completo è la proposta iniziale da verificare.
2. **PW-S02 e PW-S03 — Fondamenta:** fissare le dipendenze e rendere il modello codec estendibile. Le due attività possono procedere in parallelo dopo il contratto.
3. **PW-S04 e PW-S05 — Fattibilità GPU reale:** verificare feature Vulkan, GPU di cattura e condivisione delle immagini. Registrare almeno una combinazione hardware/driver funzionante prima di dichiarare praticabile la pipeline.
4. **PW-S06–PW-S09 — Integrazione:** encoder, budget, trasporto, negoziazione, configurazione e gestione degli errori.
5. **PW-S10 — Convalida server:** ricostruire e decodificare l'output effettivamente prodotto dal server; coprire i percorsi modificati e le regressioni pertinenti.
6. **PW-S11 — Consegna:** documentare misure, pacchetto sperimentale e contratto verificato per avviare la fase client.

Le verifiche pertinenti vengono eseguite durante ciascuna implementazione e consolidate in PW-S10. Se l'interoperabilità GPU fallisce, il relativo impedimento rimane aperto e il piano viene aggiornato con le evidenze. Le stime temporali vanno riesaminate dopo PW-S04/PW-S05; non vengono assegnate scadenze prive di una base misurabile.

## Attività in Vikunja

Progetto padre rilevato in questa sessione: `VibePollo` (ID 6). Sottoprogetto: `PyroWave — Server` (ID 7). Questi ID e quelli delle attività sono riferimenti della sessione del 23 settembre 2026: all'inizio di una nuova sessione risolverli di nuovo per nome tramite elenco o ricerca.

| Codice | ID attività | Attività | Dipende da |
| --- | ---: | --- | --- |
| PW-S00 | 142 | Pianificazione e analisi di fattibilità del server | — |
| PW-S01 | 143 | Definire architettura e contratto versionato per il futuro client | PW-S00 |
| PW-S02 | 144 | Integrare PyroWave e Granite nella build opzionale | PW-S01 |
| PW-S03 | 145 | Separare identità codec, backend e capacità di sessione | PW-S01 |
| PW-S04 | 146 | Implementare probing Vulkan/PyroWave per adattatore | PW-S02, PW-S03 |
| PW-S05 | 147 | Collegare cattura D3D11, conversione YUV e immagini Vulkan | PW-S02, PW-S04 |
| PW-S06 | 148 | Implementare sessione encoder e budget per fotogramma | PW-S03, PW-S05 |
| PW-S07 | 149 | Integrare packetizzazione, cifratura e limiti del trasporto | PW-S01, PW-S06 |
| PW-S08 | 150 | Implementare capability e negoziazione PyroWave opt-in | PW-S01, PW-S04, PW-S07 |
| PW-S09 | 151 | Configurazione, diagnostica, gestione errori e rollback | PW-S04, PW-S06, PW-S08 |
| PW-S10 | 152 | Convalidare il server con harness e regressioni mirate | PW-S05, PW-S06, PW-S07, PW-S08, PW-S09 |
| PW-S11 | 153 | Misure, packaging e consegna alla fase client | PW-S10 |

Le dipendenze sono registrate anche come relazioni effettive tra le attività. La tabella descrive il piano; gli stati, i criteri verificati e gli esiti delle esecuzioni si trovano nelle attività Vikunja e nei report di convalida.

## Regole di aggiornamento durante lo sviluppo

- All'inizio di ogni sessione leggere il progetto e l'attività pertinenti in Vikunja, risolvendone gli ID per nome. Verificare prima le dipendenze.
- All'avvio del lavoro spostare l'attività nella colonna **Doing**; mantenerla aperta finché restano criteri da soddisfare.
- Dopo ogni avanzamento significativo annotare decisioni, file modificati, eventuali commit o PR, verifiche realmente eseguite, risultati e lavoro residuo.
- Registrare impedimenti con la causa osservata, l'impatto e l'azione necessaria; non sostituire una verifica GPU mancante con un esito positivo.
- Usare percentuali soltanto se sostenute da criteri completati; non inventare avanzamenti o scadenze.
- Chiudere tramite `update_task(done: true)` solo quando tutti i criteri risultano soddisfatti e le evidenze sono registrate.
- Aggiornare questo documento quando cambiano ambito, architettura, dipendenze o criteri. Vikunja resta il riferimento per lo stato corrente.
- Gli aggiornamenti avvengono nel corso delle sessioni di implementazione; questo piano non crea una sincronizzazione automatica o un'attività in background.

Formato consigliato per un commento di avanzamento:

> Avanzamento: …  
> Decisioni e modifiche: …  
> Verifiche eseguite e risultato: …  
> Impedimenti o verifiche pendenti: …  
> Prossimo passo: …  
> Riferimenti a codice, commit o PR: …

## Schede di lavoro

### PW-S00 — Pianificazione e analisi di fattibilità del server

ID Vikunja: 142. Dipendenze: nessuna.

Registrare il piano implementativo del solo server, derivato dalla verifica statica di VibePollo e del codice upstream PyroWave.

**Lavoro previsto**

- Consolidare ambito Windows x64, SDR 4:2:0 e funzionalità opzionale; rinviare l'app client alla fase successiva.
- Registrare in Vikunja attività, dipendenze e criteri di completamento, più il riferimento persistente docs/pyrowave-server-plan.md.
- Documentare i vincoli: probing H.264 obbligatorio, interoperabilità WGC D3D11/Vulkan, limiti MTU/FEC, formato e API upstream in evoluzione.

**Criteri di completamento**

- Piano pubblicato in Vikunja e disponibile nel repository con corrispondenza delle attività.
- Le attività implementative restano aperte e prive di avanzamento inventato.
- È esplicito che l'analisi è statica: nessuna build, benchmark o compatibilità hardware è stata convalidata.

**Aree coinvolte:** docs/pyrowave-server-plan.md; src/video.h; src/video.cpp; src/stream.cpp; src/rtsp.cpp; src/platform/windows.

### PW-S01 — Definire architettura e contratto versionato per il futuro client

ID Vikunja: 143. Dipendenze: PW-S00.

Fissare il contratto lato server prima di introdurre codice dipendente dal nuovo formato.

**Lavoro previsto**

- Partire da un'estensione opt-in di RTSP/GameStream con riuso di autenticazione, controllo, input, audio e invio UDP esistenti; verificare la sostenibilità dell'incapsulamento a frame completo.
- Definire identificatore codec dedicato, versione del protocollo e del bitstream supportata, metadati SDR/range/chroma, ordine dei byte, timestamp e lunghezze.
- Definire limiti di risoluzione, frequenza, bitrate, MTU, frame e sessioni concorrenti a partire dai vincoli reali; nessun valore pubblico definitivo prima della verifica.
- Documentare semantica frame intra-only, richieste IDR, errori, riavvio e assenza di cambio codec implicito nella sessione.
- Decidere come ricostruire confini e lunghezze PyroWave dopo il trasporto; il recupero parziale nativo del codec è un'estensione successiva se il primo profilo usa frame completi.

**Criteri di completamento**

- Documento tecnico versionato con richieste/risposte di esempio e layout del payload, riutilizzabile dalla fase client.
- Client privi di capability PyroWave continuano a negoziare solo formati standard; input sconosciuto non ricade su H.264.
- Decisione esplicita sull'incapsulamento e tabella dei limiti/overhead che guiderà encoder e packetizer.
- HDR, 4:4:4, browser WebRTC e app client sono fuori dal primo profilo, per scelta di ambito.

**Aree coinvolte:** src/video.h; src/rtsp.cpp; src/nvhttp.cpp; src/stream.h; src/stream.cpp; src/stream_protocol.*; documentazione protocollo.

### PW-S02 — Integrare PyroWave e Granite nella build opzionale

ID Vikunja: 144. Dipendenze: PW-S01.

Ottenere una dipendenza riproducibile e isolata dal normale avvio del server.

**Lavoro previsto**

- Fissare commit esatti di PyroWave e delle componenti Granite necessarie; registrare licenze e artefatti shader inclusi.
- Integrare la C API attraverso un adattatore interno e un'opzione CMake proposta SUNSHINE_ENABLE_PYROWAVE, disabilitata per default.
- Verificare la toolchain Windows reale del repository e la scelta DLL/libreria; upstream richiede CMake >=3.27 per la build condivisa Windows.
- Controllare la versione API/ABI a runtime e caricare Vulkan/dipendenze solo quando necessario; nessun download al primo streaming.
- Preparare inclusione delle dipendenze negli artefatti abilitati e compilazione esclusa sulle altre piattaforme iniziali.

**Criteri di completamento**

- Build Windows x64 con feature OFF e ON riproducibili, con revisioni e comandi registrati.
- Feature OFF non richiede PyroWave/Granite/Vulkan né dipendenze nuove al caricamento; host senza runtime Vulkan avvia normalmente i codec standard.
- Versione incompatibile produce un errore locale comprensibile e rende PyroWave indisponibile.
- Notices MIT/altre licenze realmente incluse nel packaging pertinente.

**Aree coinvolte:** cmake/prep/options.cmake; cmake/dependencies/*; cmake/compile_definitions/windows.cmake; cmake/packaging/windows.cmake; third-party; NOTICE.

### PW-S03 — Separare identità codec, backend e capacità di sessione

ID Vikunja: 145. Dipendenze: PW-S01.

Rappresentare PyroWave senza attribuirgli capacità H.264 inesistenti e senza alterare la scelta dei codec standard.

**Lavoro previsto**

- Aggiungere un'identità interna esplicita per PyroWave, distinta dal backend Vulkan; preservare i valori wire esistenti.
- Aggiornare selezione della sessione, factory, capacità, cache e nomi in telemetria dove oggi esistono solo tre slot/array.
- Rimuovere il fallback ambiguo su H.264 per richieste del nuovo formato o formati sconosciuti nei confini interessati.
- Isolare il nuovo backend dal requisito H.264 di validate_encoder e dalla scelta automatica generica; limitare il refactoring ai punti necessari.

**Criteri di completamento**

- Una sessione PyroWave viene instradata soltanto al backend corretto; le tre modalità standard conservano valori e selezione.
- Un formato non riconosciuto fallisce prima dell'avvio della sessione.
- Capacità, cache e metadati identificano separatamente codec e backend, comprese sessioni contemporanee ove supportate.
- Verifiche mirate di selezione, identità e assenza di falsa pubblicità H.264 superate.

**Aree coinvolte:** src/video.h; src/video.cpp; src/video_policy.*; src/video_encoder_probe_policy.h; src/platform/common.h; src/stream.h; src/session_history*.

### PW-S04 — Implementare probing Vulkan/PyroWave per adattatore

ID Vikunja: 146. Dipendenze: PW-S02, PW-S03.

Determinare il supporto effettivo sulla GPU di cattura prima di proporre PyroWave.

**Lavoro previsto**

- Verificare Vulkan, feature subgroup/16-bit/8-bit richieste dalla revisione fissata e supporto delle immagini/sincronizzazioni esterne.
- Selezionare la GPU Vulkan tramite LUID della GPU D3D11 di cattura, senza scegliere implicitamente il primo dispositivo.
- Creare un probe PyroWave autonomo con cache dipendente da adattatore e configurazione, invalidata sui cambi reali.
- Restituire motivi di indisponibilità distinti: feature disattivata, dipendenza assente, ABI errata, driver/GPU non compatibile o interoperabilità non supportata.

**Criteri di completamento**

- Esito riproducibile sulla GPU disponibile con modello, driver e runtime annotati.
- Il fallimento PyroWave non invalida le capacità degli encoder standard.
- Cambio adattatore o stato delle dipendenze invalida correttamente la cache; mismatch LUID è rifiutato.
- I casi senza Vulkan o con capacità mancanti sono verificati attraverso controlli mirati/failure injection ove appropriato.

**Aree coinvolte:** src/video.cpp; src/video_encoder_probe_policy.h; src/platform/windows/display*; nuovo adattatore PyroWave.

### PW-S05 — Collegare cattura D3D11, conversione YUV e immagini Vulkan

ID Vikunja: 147. Dipendenze: PW-S02, PW-S04.

Alimentare PyroWave con fotogrammi reali di VibePollo mantenendo i pixel sulla GPU.

**Lavoro previsto**

- Riutilizzare la cattura WGC/DXGI e la conversione colore ove compatibile; produrre piani SDR YCbCr 4:2:0 condivisibili.
- Adattare l'esempio upstream D3D11/Vulkan alla gestione WGC keyed mutex e introdurre fence/barriere/ownership corrette.
- Allocare e riutilizzare un insieme limitato di risorse, gestire handle duplicati, scelta LUID, resize, stride, range colore e rilascio.
- Evitare readback dei pixel in produzione; il trasferimento del bitstream compresso alla CPU per l'invio resta previsto.

**Criteri di completamento**

- Frame reali WGC e DXGI passano in Vulkan e nel decoder di riferimento senza errori di colore/ordine piani.
- Nessun trasferimento GPU→CPU→GPU dei pixel nel percorso di produzione.
- Stop, cambio risoluzione e ricreazione risorse non causano deadlock o riuso di risorse ancora in uso.
- Costi di conversione e sincronizzazione misurati separatamente e registrati; matrice GPU non provate esplicita.

**Aree coinvolte:** src/platform/windows/display_wgc.cpp; src/platform/windows/display_vram.*; src/platform/windows/ipc/*; src_assets/windows/assets/shaders; adattatore PyroWave.

### PW-S06 — Implementare sessione encoder e budget per fotogramma

ID Vikunja: 148. Dipendenze: PW-S03, PW-S05.

Produrre bitstream PyroWave attraverso la pipeline di cattura e le code video del server.

**Lavoro previsto**

- Implementare device/sessione PyroWave e collegarli alle factory e al dispatch encode, usando un contenitore di payload opaco appropriato.
- Tradurre bitrate e frequenza effettiva in maximum_bitstream_size, allineamenti e limiti del contratto; includere il budget di trasporto/FEC nel limite complessivo senza confondere i due bitrate.
- Preservare frame index e timestamp di cattura/elaborazione; definire ogni frame come autonomo e rendere innocue le richieste di reference invalidation.
- Gestire cambio bitrate, backpressure, cancellazione, teardown e reinit, con sincronizzazione delle API non thread-safe.

**Criteri di completamento**

- Il server produce una sequenza valida entro il budget concordato con timestamp e frame index corretti.
- Decoder upstream ricostruisce output di prova; input statici e in movimento sono coperti.
- Cambio bitrate, pausa cattura, errore e stop hanno esiti definiti senza crescita illimitata delle code.
- Nessun trattamento SPS/PPS/Annex-B applicato ai dati PyroWave e nessuna commutazione codec implicita.

**Aree coinvolte:** src/video.cpp; src/video.h; src/platform/common.h; nuovo backend PyroWave; policy bitrate e sessione.

### PW-S07 — Integrare packetizzazione, cifratura e limiti del trasporto

ID Vikunja: 149. Dipendenze: PW-S01, PW-S06.

Inviare il nuovo payload attraverso un formato ricostruibile e compatibile con i limiti reali del trasporto scelto.

**Lavoro previsto**

- Implementare l'incapsulamento deciso in PW-S01 usando le API di packetizzazione upstream e mantenendo informazioni sufficienti per ricostruire confini e lunghezze.
- Calcolare overhead degli header, cifratura e FEC, payload MTU e limite massimo per frame prima di allocare o inviare.
- Gestire esplicitamente i limiti attuali di 4 blocchi FEC e 255 shard totali per blocco con FEC; imporre il rifiuto prima dell'invio dei blocchi da almeno 1024 pacchetti, soglia oggi soltanto segnalata nel log. Mantenere la FEC prevista nel budget normale.
- Conservare pacing, timestamp e contatori; aggiungere limiti per buffer, riordino e payload non validi secondo il contratto.

**Criteri di completamento**

- Fixture e ricevitore di prova ricostruiscono i byte originali da pacchetti server, anche con cifratura/FEC previste.
- MTU e limiti frame sono rispettati ai valori soglia; input eccessivi vengono limitati/rifiutati con messaggio definito.
- Perdita e riordino producono l'esito documentato (recupero FEC o scarto frame nel profilo iniziale).
- La frammentazione dei codec standard continua a superare le verifiche pertinenti.

**Aree coinvolte:** src/stream.cpp; src/stream_protocol.*; src/stream.h; src/rtsp.cpp; tests/unit/test_stream.cpp.

### PW-S08 — Implementare capability e negoziazione PyroWave opt-in

ID Vikunja: 150. Dipendenze: PW-S01, PW-S04, PW-S07.

Avviare PyroWave soltanto quando server e futuro ricevitore dichiarano un contratto compatibile.

**Lavoro previsto**

- Aggiungere capability e versione dedicate nell'estensione definita, senza riutilizzare i bit o i nomi dei codec standard.
- Validare codec, versione, profilo, dimensioni, bitrate e limiti prima di allocare risorse/avviare la cattura.
- Selezionare PyroWave solo quando feature e probe sono positivi e la richiesta client è esplicita.
- Mantenere il percorso browser WebRTC limitato ai codec supportati dal suo stack; descrivere l'eventuale fallback come nuova negoziazione standard.

**Criteri di completamento**

- Richieste sintetiche compatibili avviano una sessione server PyroWave; richieste incompatibili falliscono con errore utile.
- Client senza la nuova capability ricevono esclusivamente le possibilità standard e non selezionano PyroWave.
- HDR e 4:4:4 fuori dal profilo iniziale non vengono accettati o degradati silenziosamente.
- Autenticazione/autorizzazione esistenti si applicano alla nuova modalità.

**Aree coinvolte:** src/nvhttp.cpp; src/rtsp.cpp; src/video.h; src/confighttp.cpp; src/webrtc_stream.cpp; documentazione API/protocollo.

### PW-S09 — Configurazione, diagnostica, gestione errori e rollback

ID Vikunja: 151. Dipendenze: PW-S04, PW-S06, PW-S08.

Rendere la modalità sperimentale controllabile e diagnosticabile dal server.

**Lavoro previsto**

- Aggiungere interruttore runtime disattivato per default, limiti validati e spiegazione dell'esigenza di un client compatibile.
- Mostrare disponibilità e motivazione senza far scegliere PyroWave a una sessione WebRTC/browser che non lo supporta.
- Registrare tempi conversione/sincronizzazione/encode/packetizzazione, bitrate payload/rete, frame scartati, code e stato FEC.
- Gestire reconnect, stop servizio, riavvio sessione, cambio display/risoluzione/GPU, device lost e concorrenza supportata; definire rifiuti quando una combinazione è fuori ambito.
- Disabilitare la funzione o riavviare una sessione con codec standard attraverso negoziazione esplicita.

**Criteri di completamento**

- Configurazioni non valide sono respinte e la UI spiega capacità/limiti della modalità.
- Feature OFF ripristina il comportamento ordinario per sessioni future; una sessione già negoziata non cambia formato di nascosto.
- Casi di errore e ricreazione hanno timeout/uscite controllate senza risorse abbandonate nel percorso normale.
- Log e statistiche consentono di distinguere costo codec, interop e rete.

**Aree coinvolte:** src/config.*; src/confighttp.cpp; src_assets/common/assets/web; src/video.cpp; src/platform/windows; src/session_history*.

### PW-S10 — Convalidare il server con harness e regressioni mirate

ID Vikunja: 152. Dipendenze: PW-S05, PW-S06, PW-S07, PW-S08, PW-S09.

Dimostrare la correttezza del server prima di iniziare l'app client.

**Lavoro previsto**

- Consolidare le verifiche eseguite durante ogni attività in un harness CLI/test: richieste sintetiche, cattura output e/o loopback, ricostruzione trasporto e decoder PyroWave di riferimento.
- Non sviluppare UI, pairing o applicazione client; gli strumenti di prova restano interni alla convalida server.
- Coprire frame noti, colori/range/chroma, risoluzioni e fps rappresentativi, timestamp, dimensioni, sequenze, MTU, cifratura, FEC, perdita/riordino, backpressure e teardown.
- Eseguire regressioni mirate dei codec standard, feature OFF/ON, runtime assente e build pertinenti; separare verifiche automatiche da quelle GPU reali.

**Criteri di completamento**

- Comandi, hardware/driver, risultati e limiti riproducibili registrati; nessun test non eseguito riportato come superato.
- Output server ricostruibile e decodificabile entro tolleranze definite per il codec con perdita.
- Nessuna regressione nota nei percorsi standard toccati.
- I casi che richiedono GPU non disponibili rimangono esplicitamente pendenti o limitano la matrice supportata; streaming end-to-end con app client resta fase successiva.

**Aree coinvolte:** tests/unit/test_video.cpp; tests/unit/test_stream.cpp; tests/unit/test_encoder_probe_policy.cpp; tests/unit/test_rtsp_startup_snapshot.cpp; tools; report convalida.

### PW-S11 — Misure, packaging e consegna alla fase client

ID Vikunja: 153. Dipendenze: PW-S10.

Consegnare un server sperimentale riproducibile e un contratto utilizzabile dal futuro client.

**Lavoro previsto**

- Misurare latenza p50/p95 di cattura/conversione/encode/packetizzazione/invio, throughput, bitrate effettivo, uso GPU e costo sotto carico del gioco.
- Confrontare i percorsi disponibili su hardware dichiarato e registrare i limiti del confronto; i numeri upstream non sono misure VibePollo.
- Verificare il packaging con dipendenze e licenze della build abilitata e il percorso standard disabilitato.
- Fornire guida configurazione/disabilitazione, compatibilità e problemi noti; congelare per la prima fase client versione protocollo, fixture, payload e transcript di negoziazione.
- Elencare separatamente gli sviluppi successivi: app client, validazione end-to-end reale, HDR/4:4:4 e piattaforme ulteriori.

**Criteri di completamento**

- Pacchetto server sperimentale producibile e documentato, senza pubblicazione automatica.
- Report prestazioni e matrice supportata basati su dati reali; nessuna promessa di latenza end-to-end prima del client.
- Materiale client completo: capability/handshake, schema e limiti wire, metadati, errori, fixture decodificabili e versioni delle dipendenze.
- Tutte le attività server necessarie risultano completate oppure la consegna indica chiaramente gli impedimenti residui senza dichiarare il progetto finito.

**Aree coinvolte:** packaging/windows; cmake/packaging/windows.cmake; .github/workflows/ci-windows.yml; docs; fixture protocollo.


## Condizioni per iniziare la fase client

- Contratto versionato con esempi di negoziazione, layout del payload, gestione degli errori e limiti.
- Revisioni upstream fissate, materiale di prova e output server ricostruibili tramite gli strumenti di convalida.
- Pipeline server verificata sulle combinazioni hardware/driver dichiarate, con regressioni e limiti documentati.
- Misure che distinguano cattura, conversione, sincronizzazione, codifica, recupero del bitstream e invio.
- Pacchetto sperimentale e istruzioni per abilitare o disabilitare PyroWave.

La latenza percepita e il funzionamento end-to-end con l'applicazione client saranno verificati nella fase successiva. Il decoder di riferimento serve a dimostrare la conformità dell'output del server.

## Fonti e punti di partenza

- [Repository VibePollo](https://github.com/Nonary/Vibepollo), baseline indicata all'inizio del documento.
- [PyroWave: README e repository](https://github.com/Themaister/pyrowave).
- [PyroWave: API C](https://github.com/Themaister/pyrowave/blob/master/pyrowave.h).
- [PyroWave: specifica del bitstream](https://github.com/Themaister/pyrowave/blob/master/bitstream/bitstream.md).
- [PyroWave: esempio di cattura desktop e interoperabilità Windows](https://github.com/Themaister/pyrowave/blob/master/encode_desktop.cpp).
- [PyroWave: configurazione della build](https://github.com/Themaister/pyrowave/blob/master/CMakeLists.txt).
- [PyroWave: licenza MIT](https://github.com/Themaister/pyrowave/blob/master/LICENSE).

I riferimenti upstream iniziali possono cambiare. L'implementazione usa i commit e gli hash fissati in `cmake/dependencies/pyrowave-pins.cmake`, con patch locale verificata e licenze descritte nel [report di build](pyrowave-build-validation.md).
