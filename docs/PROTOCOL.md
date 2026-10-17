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

## Pendente de leitura

- mDNS (`mdnsdiscovery`): serviço, campos TXT (fase 6).
- Os pacotes de cada plugin da fase 2, conforme forem feitos.
