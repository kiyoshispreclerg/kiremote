# Protocolo do KDE Connect (v8)

O que o Kiremote implementa, conferido no código do upstream (versões em
`docs/UPSTREAM.md`). Cada item cita o arquivo de onde saiu; ao mudar o
upstream, confira de novo. Plugins entram aqui conforme forem feitos.

Abreviações: `kde/` = `upstream/kdeconnect-kde/`, `and/` =
`upstream/kdeconnect-android/src/main/java/org/kde/kdeconnect/`.

## Pacote

`kde/core/networkpacket.cpp`

JSON compacto numa linha, terminado em `\n`:

```json
{"id": 1728000000000, "type": "kdeconnect.ping", "body": {...},
 "payloadSize": 1234, "payloadTransferInfo": {"port": 1739}}
```

- `id`: milissegundos desde a época (`currentMSecsSinceEpoch`).
- `type` obrigatório; sem ele o pacote é descartado.
- `body` ausente = objeto vazio no kdeconnect-kde; o Android exige o campo
  (`and/NetworkPacket.kt`, `unserialize` usa `getJSONObject("body")`), então
  sempre mandamos `body`, mesmo vazio.
- `payloadSize`/`payloadTransferInfo` só quando há payload.
- Limites: identidade até 8192 bytes (`MAX_IDENTITY_PACKET_SIZE`,
  `kde/core/backends/lan/lanlinkprovider.cpp`); demais pacotes até 32 MiB
  (`MAX_PACKET_SIZE`, `landevicelink.cpp`).

## Identidade do dispositivo

`kde/core/deviceinfo.h`, `kde/core/kdeconnectconfig.cpp`

- **deviceId**: `^[a-zA-Z0-9_-]{32,38}$`. O kdeconnect-kde gera um UUID
  em hexa minúsculo sem hífens (32 caracteres, `QUuid::Id128`). Ids antigos
  (16 hexa, UUID com `_`) existem em pareamentos velhos, mas não passam
  nessa validação.
- **deviceName**: o upstream remove `"',;:.!?()[]<>` e corta em 32
  caracteres (`filterName`). Nome vazio depois do filtro = identidade
  inválida.
- **deviceType**: `desktop`, `laptop`, `phone`, `tablet`, `tv`. Na leitura
  aceita também `smartphone` (grafia antiga). O kdeconnect-kde sempre manda
  `desktop` (só muda por `PLASMA_PLATFORM`); o Kiremote deixa configurar.
- **protocolVersion**: 8. Abaixo disso o kdeconnect-kde ignora o aparelho
  (`deviceDiscovered`, `tcpPacketReceived`). Com aparelho já confiável,
  versão menor que a última vista = recusa (`isProtocolDowngrade`).

Três formas do pacote `kdeconnect.identity` (`kde/core/deviceinfo.h`):

| Forma | Campos do body | Quando |
|---|---|---|
| descoberta (UDP) | `deviceId`, `deviceName`, `protocolVersion`, `tcpPort` | broadcast |
| conexão (TCP, texto puro) | `deviceId`, `deviceName`, `protocolVersion`, `targetDeviceId`, `targetProtocolVersion` | primeira linha no TCP |
| completa (dentro do TLS) | `deviceId`, `deviceName`, `deviceType`, `protocolVersion`, `incomingCapabilities`, `outgoingCapabilities` | logo após o handshake, nos dois sentidos |

`deviceName` vai nas duas primeiras só para passar na validação de
implementações antigas — e do Android, que valida tudo com
`isValidIdentityPacket` (nome não vazio depois do filtro), inclusive o UDP.

O Android manda a identidade **completa** nas três formas
(`and/backends/lan/LanLinkProvider.kt`): no UDP com `tcpPort` a mais, no TCP
com `targetDeviceId` e `targetProtocolVersion` a mais. E manda
`targetProtocolVersion` como **string** (`"8"`, vem de
`getString("protocolVersion")`); o kdeconnect-kde lê com `get<int>`, que
converte. O Kiremote aceita número ou string com número.

Validação (`kde/core/deviceinfo.h`): descoberta precisa de `deviceId`
válido; conexão, de `deviceId` e `targetDeviceId` válidos; completa, de
`deviceId` válido e `deviceName` não vazio depois do filtro.

## Descoberta e conexão

`kde/core/backends/lan/lanlinkprovider.{h,cpp}`

- UDP na porta **1716** (bind com reuso de endereço). TCP do servidor na
  primeira livre de **1716 a 1764**.
- Ao iniciar e quando a rede muda, manda a identidade de descoberta por
  broadcast (`255.255.255.255`) a partir de cada endereço IPv4 local que
  não seja loopback, e também para os IPs configurados à mão
  (`customDevices`).
- Ao receber descoberta (ignorando loopback e o próprio id): valida,
  descarta `tcpPort` fora de 1716–1764 e `protocolVersion < 8`, e no máximo
  uma conexão por aparelho a cada `MILLIS_DELAY_BETWEEN_CONNECTIONS_TO_SAME_DEVICE`
  = 500 ms (até 1024 ids lembrados, `MAX_TRACKED_CONNECTION_TIMES`). Abre
  TCP para `ip:tcpPort`. O Android limita por IP e por id, 1000 ms, e
  ignora endereços que não sejam de rede local (`isPrivateAddress`).
  - Se o TCP falha: responde com a própria descoberta por UDP só para
    aquele IP, para o outro lado tentar conectar (`recoverConnectionError`).
- **Quem abriu o TCP** manda a identidade de conexão em texto puro e vira
  **servidor TLS** (`startServerEncryption`).
- **Quem aceitou o TCP** lê uma linha (prazo de 1 s, até 8192 bytes), valida:
  `targetDeviceId` (se presente) tem que ser o próprio id,
  `targetProtocolVersion` (se presente) tem que ser 8, `protocolVersion >= 8`;
  e vira **cliente TLS** (`startClientEncryption`).
- Depois do handshake, os dois mandam a identidade completa e esperam a do
  outro (prazo de 1 s). `deviceId` e `protocolVersion` têm que ser os
  mesmos da fase em texto puro, e o `deviceId` tem que ser o CN do
  certificado do outro lado (`addLink`).
- Já existindo link com aquele id, o socket novo substitui o antigo se o
  certificado for o mesmo; certificado diferente = recusa.
- Até 42 links de aparelhos não pareados (`MAX_UNPAIRED_CONNECTIONS`).
- Keepalive TCP: ocioso 30 s, intervalo 10 s, 3 tentativas
  (`configureSocket`).

### TLS

- Certificado do outro lado: aparelho confiável → só aceita o certificado
  guardado no pareamento (`VerifyPeer` com ele como única CA); não
  confiável → aceita qualquer um (`QueryPeer`), erro de autoassinado
  ignorado.
- Nome do par verificado = `deviceId`.
- Os dois lados apresentam certificado: o Android, como servidor TLS, pede
  o do cliente (`needClientAuth` se confiável, `wantClientAuth` se não;
  `and/helpers/security/SslHelper.kt`).
- O Android usa no máximo **TLS 1.2** (`SSLContext.getInstance("TLSv1.2")`,
  "1.3 dá problema em aparelhos antigos").
- O Android dá 10 s a cada leitura do aperto de mão (`setSoTimeout`); o
  kdeconnect-kde, 1 s para cada identidade.

O Kiremote pede o certificado do outro lado nos dois papéis, não deixa o
OpenSSL julgá-lo (autoassinado) e confere depois do aperto de mão, antes de
mandar a identidade: CN igual ao `deviceId` da fase em texto puro e, se
confiável, certificado idêntico ao guardado.

## Certificado próprio

`kde/core/sslhelper.cpp`, `kde/core/kdeconnectconfig.cpp`

- Chave nova: EC P-256 (`prime256v1`), e o kdeconnect-kde grava
  `keyAlgorithm=EC` na config (`generatePrivateKey`). RSA 2048 só existe em
  chaves antigas, lidas com o algoritmo padrão `RSA`. O Android também gera
  EC P-256 (`and/helpers/security/RsaHelper.kt`, `secp256r1`) e trata a falta
  de `keyAlgorithm` como RSA; instalações antigas têm RSA, então o outro
  lado pode vir com qualquer um dos dois.
- X.509 v3, serial aleatório de 160 bits, sujeito = emissor =
  `CN=<deviceId>, O=KDE, OU=KDE Connect`, validade de 1 ano atrás até 10
  anos à frente, assinatura SHA-512.
- O `deviceId` próprio é lido do CN do certificado. Certificado ausente,
  inválido, expirado ou ainda não válido → gera outro com id novo e apaga os
  pareamentos (`loadOrGeneratePrivateKeyAndCertificate`).
- Arquivos `privateKey.pem` e `certificate.pem` com permissão 0600.

## Pareamento

`kde/core/backends/pairinghandler.{h,cpp}`

Pacote `kdeconnect.pair`:

- pedido: `{"pair": true, "timestamp": <segundos desde a época>}`
- aceite: `{"pair": true}` (sem timestamp)
- recusa, cancelamento, desparear, tempo esgotado: `{"pair": false}`

Estados: `NotPaired`, `Requested` (pedimos), `RequestedByPeer` (pediram),
`Paired`.

- Recebe `pair: true`:
  - `Requested` → pareado.
  - `RequestedByPeer` → ignora (segundo pedido).
  - `Paired` sem timestamp → ignora; com timestamp → despareia e trata
    como pedido novo.
  - `NotPaired` → exige `timestamp` (sem ele, ignora); diferença para o
    relógio local maior que **1800 s** = falha ("relógios fora de
    sincronia"); senão `RequestedByPeer`.
- Recebe `pair: false`: `Requested`/`RequestedByPeer` → falhou;
  `Paired` → despareado.
- Pedir quando o outro já pediu = aceitar.
- Prazo de **30 s** em `Requested`/`RequestedByPeer`; esgotado, manda
  `pair: false`. (O Android dá 25 s em `RequestedByPeer`, para cair antes
  do outro lado.)
- Pacote que não seja `kdeconnect.pair` vindo de aparelho não pareado:
  descartado, e responde `{"pair": false}` (`Device::privateReceivedPacket`
  chama `unpair()`): o outro lado acha que está pareado e não está.
- Só se manda a um aparelho pacote de tipo que esteja nas
  `incomingCapabilities` dele (`Device::sendPacket`), exceto identidade e
  pareamento. As capacidades só são atualizadas quando as duas listas
  vêm não vazias (`updateDeviceInfo`).

### Chave de verificação

Mostrada nos dois lados enquanto o pareamento está em andamento:

```
a, b = DER da chave pública (SubjectPublicKeyInfo) de cada certificado
se a < b (comparação de bytes): troca a e b      # maior primeiro
sha256(a || b || str(timestamp)) → hexa → 8 primeiros caracteres em maiúsculas
```

`timestamp` é o do pedido, em decimal.

## Payloads

`kde/core/backends/lan/compositeuploadjob.{h,cpp}`, `landevicelink.cpp`

- Quem envia escuta na primeira porta livre de **1739 a 1764**, manda o
  pacote com `payloadSize` e `payloadTransferInfo: {"port": N}` e, ao
  receber a conexão, é **servidor TLS**.
- Quem recebe conecta em `ip:port` (o IP do link) como **cliente TLS**, com
  o certificado do pareamento, e lê `payloadSize` bytes.
- Vários envios em sequência reaproveitam a mesma porta (job composto).
- Sem conexão a tempo, a porta fecha: 30 s no kdeconnect-kde
  (`CompositeUploadJob`, `m_timeout`), 10 s no Android (`and/backends/lan/LanLink.java`,
  `server.setSoTimeout`).
- **Quem envia fecha a conexão ao terminar**: o Android lê o payload até o
  EOF (`and/plugins/share/CompositeReceiveFileJob.java`, `receiveFile`), e
  o kdeconnect-kde fecha o socket depois de enviar (`LanLink.java`,
  `sendPayload`, no `finally`). Quem espera o receptor fechar primeiro trava
  até o timeout e o Android mostra erro, mesmo com o arquivo completo.
- O Android só considera que há payload com `payloadSize` diferente de 0;
  arquivo vazio vai sem payload.

## Plugins

### Ping

`kde/plugins/ping/pingplugin.cpp`, `and/plugins/ping/PingPlugin.kt`

`kdeconnect.ping`, nos dois sentidos, `{"message": "..."}` opcional. Sem
mensagem, os dois mostram "Ping!".

### Compartilhar

`kde/plugins/share/shareplugin.cpp`, `and/plugins/share/`

`kdeconnect.share.request`, nos dois sentidos; o Android também aceita
`kdeconnect.share.request.update` (`numberOfFiles`, `totalPayloadSize`,
para a barra de progresso).

| Corpo | Significado |
|---|---|
| `filename` + payload, `lastModified` e `creationTime` (ms) opcionais, `open` | arquivo; `numberOfFiles` e `totalPayloadSize` do lote inteiro |
| `text` | texto (o kdeconnect-kde põe no clipboard) |
| `url` | link (abre no navegador) |

O nome recebido é cortado no último `/`; com nome que já existe o
kdeconnect-kde renomeia (`setAutoRenameIfDestinationExists`). O Android
distingue arquivo por `filename`; o kdeconnect-kde, por payload ou
`filename`.

### Bateria

`kde/plugins/battery/batteryplugin.cpp`, `kde/plugins/battery/README`,
`and/plugins/battery/BatteryPlugin.kt`

`kdeconnect.battery`, nos dois sentidos:

| Campo | Significado |
|---|---|
| `isCharging` (bool) | carregando |
| `currentCharge` (int) | porcentagem; no kdeconnect-kde é a média das baterias do computador |
| `thresholdEvent` (int) | 0 = nada, 1 = entrou no estado "baixa" (≤ 15% e descarregando no kdeconnect-kde; o Android usa `ACTION_BATTERY_LOW`) |
| `batteryQuantity` (int) | só o kdeconnect-kde manda |
| `request` (bool) | pede ao outro lado que mande o estado dele (README do plugin; o código do kdeconnect-kde não trata, o Android responde) |

Sem bateria no computador, não se manda nada (`slotChargeChanged` só avisa).
Um periférico (mouse sem fio) não é a bateria do computador: no Solid só entra
`isPowerSupply()`; no sysfs, `scope` = `Device`.

### Sinal da operadora

`kde/plugins/connectivity-report/connectivity_reportplugin.cpp`,
`and/plugins/connectivityreport/ConnectivityReportPlugin.kt`

`kdeconnect.connectivity_report`, só do celular para o computador:
`{"signalStrengths": {"<id da assinatura>": {"networkType": "LTE",
"signalStrength": 0-4}, ...}}`. Um celular com dois chips manda uma entrada
por chip; o kdeconnect-kde mostra a primeira (`subscriptions.first()`).
`networkType`: `5G`, `LTE`, `HSPA`, `UMTS`, `EDGE`, `GPRS`, `Unknown`... (o
ícone do kdeconnect-kde só distingue esses).

### Achar o celular / achar este computador

`kde/plugins/findmyphone`, `kde/plugins/findthisdevice`,
`and/plugins/findmyphone`, `and/plugins/findremotedevice`

Um pacote só, `kdeconnect.findmyphone.request`, de corpo vazio, nos dois
sentidos: quem recebe toca um som. No kdeconnect-kde são dois plugins
(`findmyphone` só envia, `findthisdevice` só recebe); no Android também
(`FindRemoteDevicePlugin` envia, `FindMyPhonePlugin` recebe). Não é o ping:
o celular toca no volume máximo até alguém dispensar, mesmo no silencioso.
O `findthisdevice` toca o som uma vez e desmuta as saídas que estavam no
mudo.

### Telefonia

`kde/plugins/telephony/telephonyplugin.cpp`, `kde/plugins/telephony/README`,
`and/plugins/telephony/TelephonyPlugin.kt`

`kdeconnect.telephony`, do celular: `event` (`ringing`, `talking`,
`missedCall`; `sms` é o formato antigo e o kdeconnect-kde ignora),
`phoneNumber`, `contactName` (nome ou, sem contato, o número),
`phoneThumbnail` (JPEG em base64) e `isCancel`. **O Android manda `isCancel`
como a string `"true"`** (`lastPacket["isCancel"] = "true"`); o
kdeconnect-kde lê com `get<bool>`, que converte. O Kiremote aceita as duas
formas. Ao fim da chamada o Android reenvia o último pacote com `isCancel`.

`kdeconnect.telephony.request_mute` (do computador, corpo vazio) silencia o
toque (`TelephonyPlugin.kt`, `muteRinger`).

### Pausar na chamada

`kde/plugins/pausemusic/pausemusicplugin.cpp`, `.../README`

Não tem pacote próprio: usa o `kdeconnect.telephony`. `ringing` ou `talking`
(só `talking` se a opção `conditionTalking` estiver ligada) pausa os
players MPRIS que estão tocando (`Pause`, ou `Stop` se `CanPause` for falso);
o pacote com `isCancel` volta a tocar os que foram pausados. O
kdeconnect-kde ainda tem uma opção para mudar o volume do sistema para mudo.

### Volume do sistema

`kde/plugins/systemvolume/systemvolumeplugin-pulse.cpp`,
`and/plugins/systemvolume/SystemVolumePlugin.kt`

O computador recebe `kdeconnect.systemvolume.request` e manda
`kdeconnect.systemvolume`:

- `{"requestSinks": true}` → `{"sinkList": [{"name", "description", "volume",
  "maxVolume", "muted", "enabled"}, ...]}`. `volume` está na escala do
  PulseAudio (`maxVolume` = 65536 = 100%); `enabled` = é a saída padrão. A
  lista também vai ao conectar e quando uma saída entra ou sai.
- `{"name": "<saída>", "volume": n}` define o volume **e desmuta**;
  `{"name", "muted": bool}`; `{"name", "enabled": true}` torna padrão.
  O nome tem que ser de uma saída da última lista (`sinksMap`).
- Mudanças feitas por outro programa saem como `{"name", "volume"}`,
  `{"name", "muted"}` ou `{"name", "enabled"}`.

### Controle de mídia do desktop

`kde/plugins/mpriscontrol/mpriscontrolplugin.cpp`, `.../README`,
`and/plugins/mprisreceiver/`

O computador manda `kdeconnect.mpris` e recebe `kdeconnect.mpris.request`.
Players são os `org.mpris.MediaPlayer2.*` no D-Bus de sessão, menos
`kdeconnect.*` e `playerctld`. O nome mostrado é o `Identity` do player, com
` [2]`, ` [3]` se repetir.

Computador → celular (campos de um mesmo pacote, só os que mudaram):

| Campo | Origem MPRIS |
|---|---|
| `playerList` (nomes), `supportAlbumArtPayload` (true) | lista de players |
| `title`, `artist` (juntos com ", "), `album`, `albumArtUrl`, `url`, `length` (ms) | `Metadata` (`xesam:*`, `mpris:artUrl`, `mpris:length`/1000) |
| `isPlaying` | `PlaybackStatus` = `Playing` |
| `canPause`, `canPlay`, `canGoNext`, `canGoPrevious`, `canSeek` | `Can*` |
| `loopStatus`, `shuffle` | `LoopStatus`, `Shuffle` (opcionais) |
| `volume` (0–100) | `Volume` × 100 |
| `pos` (ms) | `Position`/1000, junto com qualquer mudança; sozinho após `Seeked` |
| `player` | nome do player, em todo pacote que não é a lista |

Sem título nem artista e com `xesam:url` de arquivo local, o título é o nome
do arquivo e o álbum a pasta (`mprisPlayerMetadataToNetworkPacket`). Com a
integração do Plasma no navegador, o player simples do Firefox/Chromium fica
de fora da lista (`getFilteredPlayerKeys`).

Celular → computador (`kdeconnect.mpris.request`), com `player`:
`requestPlayerList`; `action` (nome do método MPRIS); `setLoopStatus`;
`setShuffle`; `setVolume` (0–100); `Seek` (µs, relativo); `SetPosition` (ms,
absoluto: o kdeconnect-kde converte em `Seek` pela diferença para a posição
atual); `requestNowPlaying`; `requestVolume`; `albumArtUrl` (pede a capa). Um
pedido com `playerList` vem de um cliente MPRIS e é ignorado.

Capa: só `file://`, e só a URL que o player diz estar mostrando agora; sai
como payload de `{"transferringAlbumArt": true, "player", "albumArtUrl"}`.

### Clipboard

`kde/plugins/clipboard/clipboardplugin.cpp`, `.../README`,
`and/plugins/clipboard/ClipboardPlugin.kt`

`kdeconnect.clipboard`, nos dois sentidos: `{"content": "texto"}` quando o
clipboard muda; quem recebe põe o texto no seu. E `kdeconnect.clipboard.connect`,
mandado ao conectar: `{"content", "timestamp"}` (ms desde a época, de quando o
clipboard mudou por último). Quem recebe só aplica se o `timestamp` não for 0
nem anterior ao seu (o clipboard mais novo vence). O kdeconnect-kde ainda tem
`kdeconnect.clipboard.file` (arquivo ou imagem como payload); esta versão do
Android não manda nem aceita, então o Kiremote não o implementa. O
kdeconnect-kde reconhece o aviso de gerenciador de senhas
(`x-kde-passwordManagerHint`, `secret`) e deixa de enviar se
`sendPassword` estiver desligado (padrão: ligado).

### Compartilhar: texto e links

`kde/plugins/share/shareplugin.cpp`, `and/plugins/share/SharePlugin.kt`

Além do arquivo (veja Compartilhar acima): `kdeconnect.share.request` com
`{"text"}` (vai para o clipboard, com notificação que oferece abrir o link se
a última linha for um) ou `{"url"}` (abre no navegador, `QDesktopServices::openUrl`).

### Notificações do celular

`kde/plugins/notifications/`, `.../README`,
`and/plugins/notifications/NotificationsPlugin.kt`

Do celular, `kdeconnect.notification`: `id`, `appName`, `title`, `text`,
`ticker` (para Androids antigos), `isClearable`, `isCancel` (foi dispensada no
celular), `silent` (o Android marca assim as que já existiam ao conectar),
`actions` (lista de rótulos), `requestReplyId`, `conversation` (lista de
`{sender, content}`; `title`/`text` já vêm preenchidos para compatibilidade),
`payloadHash` e, com ele, o ícone em PNG como payload. O mesmo `id` de novo
atualiza. O kdeconnect-kde tira o prefixo `org.kde.kdeconnect_tp::` do `id` do
cancelamento.

Do computador: `kdeconnect.notification.request` com `{"request": true}` ao
conectar (manda as existentes) e `{"cancel": "<id>"}` quando o usuário
dispensa uma aqui; `kdeconnect.notification.action` com
`{"key": "<id>", "action": "<rótulo>"}`; e `kdeconnect.notification.reply`
(`requestReplyId`, `message`), que o Kiremote ainda não manda. Ao acionar um
botão, o kdeconnect-kde copia para o clipboard um número de 4 a 10 dígitos
isolado no rótulo (códigos de verificação; U+2063 entre dígitos é ignorado).

### Mouse e teclado remotos

`kde/plugins/mousepad/x11remoteinput.cpp`, `.../README`,
`and/plugins/mousepad/MousePadPlugin.kt`

`kdeconnect.mousepad.request` do celular, uma destas formas por pacote:
`dx`/`dy` (movimento relativo), `x`/`y` (absoluto), `singleclick`,
`doubleclick`, `middleclick`, `rightclick`, `singlehold`/`singlerelease`
(arrastar), `scroll` + `dy` (uma volta da roda, para cima se `dy > 0`),
`key` (texto) ou `specialKey` (1 a 32, tabela `SpecialKeysMap`: BackSpace,
Tab, Linefeed, setas, Page Up/Down, Home, End, Return, Delete, Escape,
Sys_Req, Scroll_Lock, Control, Alt, Shift, Super, F1 a F12); as de tecla podem
trazer `ctrl`, `alt`, `shift`, `super`. Com o ponteiro invertido (canhoto) os
botões esquerdo e direito trocam. O computador manda
`kdeconnect.mousepad.keyboardstate` com `{"state": true}` ao conectar.

### Executar comandos

`kde/plugins/runcommand/runcommandplugin.cpp`,
`and/plugins/runcommand/RunCommandPlugin.java`

Do celular, `kdeconnect.runcommand.request`: `{"requestCommandList": true}`,
`{"key": "<comando>"}`, `{"stop": ...}`, `{"setup": ...}`. Do computador,
`kdeconnect.runcommand`: `{"commandList": "<JSON em texto>", "canAddCommand"}`
(a lista é um objeto JSON **dentro de uma string**: `{"<chave>": {"name",
"command"}}`), e `kdeconnect.runcommand.output`: `{"commandStarted": true,
"command", "id"}`, `{"commandOutput": true, "stdout": [..], "stderr": [..],
"id"}` (de cinco em cinco linhas) e `{"commandFinished": true, "success",
"exitCode", "id"}`. O celular só escolhe a chave de um comando já
configurado no computador.

### Apresentação

`kde/plugins/presenter/presenterplugin.cpp`, `and/plugins/presenter/`

`kdeconnect.presenter` do celular: `{"dx", "dy"}` (frações da tela; `dy`
multiplicado pela proporção largura/altura) move um ponteiro vermelho,
começando no meio da tela; `{"stop": true}` o esconde; ele some sozinho 0,5 s
depois do último movimento. As teclas de slide (PageUp/PageDown/F5/Esc) vêm
como `kdeconnect.mousepad.request`.

### Bloquear

`kde/plugins/lockdevice/lockdeviceplugin.cpp`

`kdeconnect.lock`: `{"isLocked": bool}` (estado de quem manda) e
`{"lockResult": bool}` (resposta a `setLocked`). `kdeconnect.lock.request`:
`{"requestLocked": null}` (manda-me o estado) e `{"setLocked": bool}`. O
estado é o `LockedHint` da sessão no logind (barramento de sistema). O
Android desta revisão do upstream **não tem** plugin de bloqueio (não há
`kdeconnect.lock` no código dele); serve a outros clientes.

### Inibir o descanso de tela

`kde/plugins/screensaver-inhibit/`

Sem pacotes: com um aparelho pareado conectado, pede ao serviço de descanso
de tela da sessão (`org.freedesktop.ScreenSaver.Inhibit`) para não apagar nem
bloquear a tela, e devolve o pedido (`UnInhibit`, mais `SimulateUserActivity`)
quando o último vai embora. Desligado por padrão (`EnabledByDefault: false`).

### Navegar pelos arquivos (sftp)

`kde/plugins/sftp/{sftpplugin,mounter}.cpp`, `and/plugins/sftp/SftpPlugin.kt`,
`SimpleSftpServer.kt`

Quem navega manda `kdeconnect.sftp.request` `{"startBrowsing": true}`. O
celular sobe um servidor SSH e responde `kdeconnect.sftp`:
`{"ip", "port", "user", "password", "path"}` e, com mais de um
armazenamento, `"multiPaths": [...]` e `"pathNames": [...]` (nomes para
mostrar). `user` é fixo (`kdeconnect`), a senha é nova a cada pedido, a porta
fica em 1739–1764. O servidor aceita a chave pública do certificado do
aparelho pareado (a nossa `privateKey.pem`) ou a senha. Outros pacotes do
celular: `{"serverRunning": false}` (ao ligar o plugin e quando o servidor
parou: quem tem a montagem pede de novo) e `{"errorMessage": "..."}` (sem
permissão de arquivos, sem armazenamento configurado). O desktop monta com
`sshfs user@ip:path` (com `multiPaths`, a raiz `/`) e mostra os `multiPaths`
como pastas; só aceita o pacote se tiver pedido (`startBrowsing`) antes.

## Espelhamento (scrcpy-server)

Não é KDE Connect: é o protocolo entre o cliente do scrcpy e o
`scrcpy-server`, versão fixada em `config.mk` (`SCRCPY_VERSION`, hoje 5.0.1).
O protocolo muda entre versões e o servidor recusa outra versão do cliente.
Lido em `upstream/scrcpy` (v5.0.1): `app/src/server.c`, `app/src/demuxer.c`,
`app/src/adb/adb.c`, `app/src/adb/adb_tunnel.c`,
`server/.../device/DesktopConnection.java`, `.../device/Streamer.java`,
`.../Options.java`. O jar vem da release
(`install_release.sh`: URL e SHA-256).

**Subir o servidor.** Tudo pelo executável `adb`, como o cliente oficial:

1. `adb -s SERIAL push scrcpy-server /data/local/tmp/<nome>.jar`.
2. Túnel reverso: o cliente escuta numa porta TCP local e roda
   `adb -s SERIAL reverse localabstract:scrcpy_<scid> tcp:<porta>`; o
   servidor conecta nele. `scid`: 31 bits aleatórios, em hexadecimal com 8
   dígitos (`%08x`; `scrcpy_generate_scid`). Se o `reverse` falhar, o oficial
   cai para `forward` (o cliente conecta e o servidor manda um byte 0 para
   provar que está escutando); não usamos ainda.
3. `adb -s SERIAL shell CLASSPATH=/data/local/tmp/<nome>.jar app_process /
   com.genymobile.scrcpy.Server <versão> chave=valor...`. Chaves que usamos:
   `scid`, `log_level` (`verbose`, `debug`, `info`, `warn`, `error`),
   `video` / `audio` / `control` (`false` desliga), `video_codec` (`h264`,
   `h265`, `av1`), `max_size` (lado maior, em pixels), `video_bit_rate`,
   `max_fps`, `cleanup`. A saída do shell é o log do servidor.
4. Depois de conectados os sockets, `adb reverse --remove
   localabstract:scrcpy_<scid>`.

**Sockets.** Um por fluxo ligado, sempre na ordem vídeo, áudio, controle.
O primeiro começa com 64 bytes: o nome do aparelho em UTF-8, completado
com zeros (o cliente força o último byte a 0).

**Fluxo de vídeo e de áudio.** 4 bytes big-endian com o codec: `h264`
`0x68323634`, `h265` `0x68323635`, `av1` `0x00617631`, `opus`
`0x6f707573`, `aac` `0x00616163`, `flac` `0x666c6163`, `raw` `0x00726177`
(PCM s16le); `0` = o aparelho desligou o fluxo (sem captura de áudio:
segue só com vídeo); `1` = erro de configuração (parar). Depois, cabeçalhos
de 12 bytes:

- bit 63 ligado: **sessão** (só vídeo; a primeira vem logo depois do
  codec, e uma nova a cada rotação ou mudança de tamanho). Bytes 0–3:
  `0x80000000`, com o bit 0 = redimensionado pelo cliente; 4–7 largura,
  8–11 altura (big-endian).
- bit 63 desligado: **pacote**. Bytes 0–7: PTS em µs com flags (bit 62 =
  config, sem PTS; bit 61 = keyframe); 8–11: tamanho (nunca 0), seguido do
  pacote cru do MediaCodec. Em H.264/H.265 o pacote de config (SPS/PPS)
  vai grudado na frente do próximo pacote antes de decodificar
  (`sc_packet_merger`). Áudio é sempre estéreo, 48 kHz.

Gravado do aparelho de teste (Android 12, `max_size=800`): nome
`2201117TL`, `h264`, sessão 360×800, config de 31 bytes.

## Pendente de leitura

- mDNS (`mdnsdiscovery`): serviço, campos TXT (fase 6).
- `kdeconnect.notification.reply`, SMS e contatos.
