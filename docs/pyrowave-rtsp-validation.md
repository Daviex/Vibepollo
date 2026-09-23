# Convalida RTSP PyroWave in loopback

> **PW-X extension status (23 September 2026):** This document records the original SDR 4:2:0 phase at commit `205c510c`. Its limits, hashes and test results are historical. For the extended implementation, use the [capability matrix](pyrowave-capability-matrix.md), [protocol v2](pyrowave-protocol-v2.md) and [extension build report](pyrowave-extension-build.md). The earlier results do not qualify HDR, 4:4:4 or the new transport/runtime paths.


Data: 23 settembre 2026. Questa prova usa gli oggetti del server Windows x64 MinGW compilato con il nuovo codec. Non avvia l'entry point dell'applicazione, non legge configurazioni o pairing di produzione e non rappresenta una convalida del futuro client.

## Risultati osservati

- 20 casi negativi nella build ON: risposte RTSP 400/406 attese, zero sessioni create e DLL PyroWave mai caricata.
- 6 casi negativi nella build OFF: incluse richiesta PyroWave con risposta 406, codec sconosciuto/malformato, opt-in assente, duplicati e ultima riga senza terminatore.
- Caso positivo ON: `ANNOUNCE` risponde `200 OK`; il vero probe GPU riesce, `stream::session::alloc/start` pubblicano una sessione codec 3 e il contatore delle sessioni attive vale 1. Dopo shutdown e join il contatore torna a 0, non rimangono teardown in corso e la concessione esclusiva PyroWave può essere nuovamente acquisita.
- Caso positivo con video, ripetuto sulla DLL finale con identità verificata: cattura DXGI reale, conversione esistente, encoder PyroWave, sender UDP, ricostruzione PWVF e decoder di riferimento producono tre fotogrammi non vuoti in RAM. I timestamp crescono strettamente e gli indici corrispondono al frame trasportato.
- Durante la stessa sessione, una richiesta IDR e la riapplicazione dello stesso bitrate mantengono lo stato `running`. Un bitrate diverso attiva l'arresto previsto per la rinegoziazione; shutdown e join liberano poi la concessione esclusiva.
- Dopo il rilascio della sessione e della concessione, un warmup e 12 chiamate pubbliche `probe_pyrowave(true)` verificano nuovamente disponibilità, conversione e codifica di prova a 64×64. Il numero di handle del processo resta 437 per ogni chiamata, senza crescita.

Il primo caso positivo non invia ping audio o video: verifica avvio, selezione del codec e rilascio delle risorse. I messaggi `Initial Ping Timeout` nel relativo log sono attesi per questa scelta; non costituiscono una diagnosi della rete dell'utente. La chiusura dell'acceptor può inoltre registrare l'abort dell'operazione pendente durante il normale shutdown.

La prova finale con video invia soltanto ping UDP video, senza ping audio o input. Il timeout audio di 8 secondi durante la chiusura è quindi atteso. Usa output 1280×720 SDR, 60 fps richiesti, pacchetti da 1392 byte e FEC al 20%, senza cifratura. La cattura osservata è un desktop 2560×1440 nel formato FP16 scRGB SDR, sulla RTX 3080 Ti. La ricostruzione verifica i dati e conta la parità; non induce perdita di pacchetti e non dimostra recupero FEC in questa prova.

### Metriche dell'ultima esecuzione

| Controllo | Valore osservato |
| --- | --- |
| Fotogrammi decodificati in RAM | 3 |
| Datagrammi ricevuti / frammenti di parità | 371 / 44 |
| Fotogrammi scartati dal receiver | 0 |
| Escursione massima luma decodificata | 255 |
| Primo / ultimo timestamp PWVF | 579208 / 644226 µs, strettamente crescenti |
| Byte PWVF totali / massimo per fotogramma | 446208 / 148740 |
| Bitrate encoder riapplicato senza arresto | 79308 kbps |
| Nuovo bitrate che richiede rinegoziazione | 80308 kbps |
| Stato finale dopo shutdown | 0 sessioni e 0 teardown; concessione nuovamente acquisibile |
| Handle prima / dopo warmup / dopo 12 probe forzati | 437 / 437 / 437 |
| Minimo / massimo handle negli ultimi 6 probe | 437 / 437 |

`receiver_decode_drain_fps=44.7703` nel log misura il ritmo con cui questa breve prova svuota e decodifica i frame ricevuti. Non misura gli fps della cattura o dello streaming e non è un benchmark. Nessun contenuto del desktop viene salvato: i file conservati contengono soltanto metriche e diagnostica. La DLL usata ha SHA-256 `d6105cbc2861df660b7116fc04270408aba28adc94db513be7127e9798645f73`.

Il controllo dei probe fallisce se la crescita finale dopo warmup supera 4 handle oppure se l'escursione degli ultimi sei campioni supera 4. Nell'esecuzione riportata entrambi i valori sono zero. La prova è stata ripetuta dopo la correzione dell'ownership del desktop Windows, del teardown del probe e dei binding del converter; usa il vero factory/probe della produzione e non una sua simulazione.

## Isolamento riproducibile

`tools/pyrowave/build_rtsp_smoke.py` riusa gli oggetti e le librerie del server, conservando `sunshine.rsp`. Rinomina `main` soltanto in una copia del suo oggetto e compila nuovamente `rtsp.cpp` e `stream.cpp` con `SUNSHINE_PYROWAVE_RTSP_HARNESS=1`. Le modifiche condizionali sono esplicite nel sorgente:

- nomi privati per gli eventi Windows HDR, così la prova non reimposta gli eventi del server già in esecuzione;
- isolamento dei callback di piattaforma, profili driver, frame limiter, gestione/ripristino display, pausa/ripresa applicazione e tray/aggiornamenti;
- supervisione dell'applicazione sostituita dalla vita del processo di prova, che ha il proprio watchdog: nessuna app utente viene caricata;
- stessi parser, negoziazione, probe, allocazione, avvio dei thread, contatori di proprietà e rilascio della sessione.

Il valore predefinito della macro è assente nelle build normali. Il listener è associato solo a `127.0.0.1`, su una porta scelta dal sistema. Ogni caso è un processo distinto, con watchdog interno di 30 secondi e limite di 35 secondi imposto dal processo padre. Cronologia sessioni disattivata, client UUID vuoto, nessuna applicazione caricata o comando di avvio/arresto. Input di piattaforma e task pool non vengono inizializzati.

I negativi impostano anche `input_only=true` per impedire l'avvio GPU qualora una validazione regredisse. Lo script controlla status e motivazione della risposta, e per versione/profilo/revisione, host disabilitato e frequenza discordante anche il motivo specifico nel log: questa protezione aggiuntiva non viene conteggiata come prova della validazione attesa.

## Casi negativi ON

| Caso | Risposta |
| --- | --- |
| Codec sconosciuto o malformato | 400 Unsupported video codec |
| Opt-in della richiesta launch assente | 406 Codec does not match launch request |
| Estensione duplicata con valori diversi, anche nell'ultima riga senza newline | 400 Conflicting codec parameters |
| Host disabilitato | 406 Unsupported PyroWave session + motivo host disabilitato |
| Profilo, revisione o versione errati; versione assente | 406 Unsupported PyroWave session + motivo incompatibilità |
| HDR richiesto | 406 Unsupported PyroWave profile |
| Numero malformato o oltre `int` | 400 Invalid PyroWave numeric parameter |
| Cifratura vuota, negativa, con suffisso o overflow | 400 Invalid PyroWave numeric parameter |
| Bit cifratura non supportato (`8`) | 406 Unsupported PyroWave profile |
| Campo numerico duplicato con valori diversi | 400 Conflicting PyroWave parameters |
| Frequenza intera e frazionaria discordanti | 406 Unsupported PyroWave session + motivo frequenze discordanti |

## Comandi

Dalla radice del repository, con MSYS2 UCRT64 nel `PATH` e build complete descritte in [pyrowave-build-validation.md](pyrowave-build-validation.md):

```powershell
cmake --build build/pyrowave-server-on --target sunshine -j 4 -- -d keeprsp
cmake --build build/pyrowave-server-off --target sunshine -j 4 -- -d keeprsp
python tools/pyrowave/build_rtsp_smoke.py --server-build build/pyrowave-server-off --output-dir build/pyrowave-rtsp-smoke/final-off --run --feature-off
python tools/pyrowave/build_rtsp_smoke.py --server-build build/pyrowave-server-on --output-dir build/pyrowave-rtsp-smoke/final --run --positive --media
```

Gli esiti sono salvati in `results.json`, con un log distinto per caso, nelle directory indicate. Gli oggetti originali e l'eseguibile `sunshine.exe` restano quelli della build normale; il relativo processo di produzione non viene avviato, arrestato o aggiornato.

L'ultima esecuzione completa supera 6/6 casi OFF e 21/21 casi ON, compreso il positivo video/controlli e il successivo lifecycle di 13 probe. Omettendo `--media` si può ripetere il precedente caso positivo di solo avvio/arresto.

Questa prova non comprende autenticazione/pairing NVHTTP, avvio di applicazioni, modifiche del display, installazione del servizio, client esterno o reti remote. L'assenza di questi passaggi deve rimanere esplicita quando si usano i risultati come criterio di avanzamento.
