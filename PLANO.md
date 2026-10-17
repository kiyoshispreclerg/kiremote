# Kiremote — plano

Integração celular ↔ desktop para o KiDesktop, compatível com o protocolo do
KDE Connect (usa o app oficial do Android, o de iOS, o GSConnect etc. do
outro lado), só que sem Qt/KF5: C11, Xlib e OpenSSL no daemon, GTK2 só na
interface. E com o que o KDE Connect não tem: espelhamento e controle da
tela do celular, num cliente próprio do protocolo do scrcpy.

## Objetivos

- Parear e conversar com o app oficial do KDE Connect (Android primeiro),
  protocolo v8. Sem v7: o kdeconnect-kde atual também ignora quem fala
  menos que 8 (o KDE Connect 21.12 desta máquina é v7 e não serve de
  parceiro de teste).
- Cobrir quase todos os plugins do KDE Connect que fazem sentido sem KDE.
- Espelhar e controlar a tela do Android (USB e Wi-Fi), com áudio, usando o
  `scrcpy-server` (o jar que roda no celular) e um cliente nosso: sem SDL,
  sem o cliente do scrcpy.
- Leve: daemon ocioso com poucos MB de RSS e zero CPU; nada de GUI rodando
  quando ninguém está olhando.

Fora do escopo por enquanto: Wayland, Windows/macOS, backend Bluetooth,
app próprio para o celular.

Repositório próprio; no futuro pode entrar no `kidesktop-extras`. Por
isso nada aqui depende do código do KiDesktop, só das interfaces padrão
que ele implementa (SNI, Notifications, MPRIS).

Todos os programas são multi-idioma com gettext (domínio `kiremote`,
msgids em inglês, `po/` com en e pt_BR): mensagens de log, notificações,
menu da bandeja e interface.

## Arquitetura

```
kiremoted         (C11, sem GTK; laço único com poll(), sem threads)
 ├─ descoberta     UDP 1716 (broadcast) + TCP 1716–1764; mDNS opcional depois
 ├─ links          TLS sem bloquear (BIOs de memória, como o tls.c do kikarinhas)
 ├─ pareamento     certificados autoassinados, confiança fixada por device id
 ├─ payloads       arquivos em conexões TLS à parte, sem bloquear o laço
 ├─ plugins        um arquivo por plugin, tabela de tipos de pacote → handler
 ├─ X11            clipboard (XFixes), entrada remota (XTest)
 ├─ D-Bus          Notify, MPRIS, item da bandeja (SNI + DBusMenu)
 └─ socket unix    $XDG_RUNTIME_DIR/kiremote.sock, JSON por linha
kiremote          (GTK2, abre sob demanda: dispositivos, pareamento, SMS,
                   envio de arquivos, controle de mídia, configurações)
kiremote-cli      (sem GTK: listar, parear, ping, enviar arquivo, scripts)
kiremote-mirror   (Xlib + Xv/XShm + libavcodec + ALSA; um processo por
                   celular espelhado, iniciado pelo daemon ou pela interface)
```

### Princípios

- O daemon não carrega GTK, libavcodec nem nada de vídeo. A interface e o
  espelhamento são processos à parte, que só existem enquanto abertos.
- Uma thread só no daemon; tudo no mesmo `poll()`: sockets UDP/TCP/TLS, fd
  do X, conexão D-Bus, socket unix, timerfd dos keepalives.
- O ícone da bandeja é do daemon (SNI por libdbus), não da interface: o
  xispanel só hospeda StatusNotifierItem, e assim a bandeja não custa um
  processo GTK aberto. O menu vai por DBusMenu, que o xispanel já lê.
- Cada plugin é isolado atrás da mesma interface (`kr_plugin`: tipos que
  recebe, tipos que envia, `on_packet`, `on_connect`, `on_disconnect`). As
  capacidades anunciadas na identidade saem da lista de plugins ligados.
- Nada de inventar campo de pacote: tudo é conferido em `upstream/`
  (kdeconnect-kde e kdeconnect-android) e anotado em `docs/PROTOCOL.md`,
  como no kisnitch.

### Dependências

| Biblioteca | Onde | Uso |
|---|---|---|
| OpenSSL | daemon | TLS, geração de certificado/chave, SHA-256 da chave de verificação |
| libX11, libXfixes, libXtst | daemon | clipboard (aviso de mudança de seleção) e entrada remota |
| libdbus-1 | daemon | Notify, MPRIS, SNI/DBusMenu (como no kikarinhas; ou dlopen como no xispanel) |
| cJSON (embutido) | todos | pacotes |
| GTK2 | `kiremote` | interface |
| libavcodec, libavutil | `kiremote-mirror` | decodificar H.264/H.265 (VA-API opcional pelo próprio libavcodec) |
| libXv, libXext (MIT-SHM) | `kiremote-mirror` | mostrar YUV direto, escala pela GPU; XShm + libswscale de reserva |
| ALSA | daemon, `kiremote-mirror` | tocar o "achar este computador"; áudio do espelhamento |
| externos, opcionais | | `adb` (só o servidor dele), `sshfs` (navegar arquivos), `pactl` (volume), `xdg-open` |

Build com Makefile + `config.mk` + pkg-config, no esquema do kikarinhas:
partes opcionais só entram se a dependência existir (GTK2 → `kiremote`;
libavcodec + Xv → `kiremote-mirror`).

Código a reaproveitar do kikarinhas: `tls.c`, `json.c`, `ini.c`, `log.c`,
`util.c`, `control.c` (socket JSON), `mpris.c`, `audio.c`. Do xispanel:
as ideias do `sni.c`/`notifd.c` (libdbus sem watches, drenar no laço).

## Protocolo do KDE Connect (resumo)

A fonte é o `docs/PROTOCOL.md`, conferido no código do upstream; isto é
só o mapa.

- **Pacote**: JSON numa linha, `{"id", "type", "body", "payloadSize"?,
  "payloadTransferInfo"?}`, terminado em `\n`.
- **Identidade** (`kdeconnect.identity`): `deviceId`, `deviceName`,
  `deviceType` (`desktop`, `laptop`, `smartphone`, `tablet`, `tv`),
  `protocolVersion`, `tcpPort`, `incomingCapabilities`,
  `outgoingCapabilities`.
- **Descoberta**: identidade por broadcast UDP na 1716. Quem recebe abre
  TCP para o `tcpPort` do outro (1716–1764), manda um pacote de conexão em
  texto puro (com `targetDeviceId`/`targetProtocolVersion`) e aí começa o
  TLS **com papéis invertidos** (quem abriu o TCP é o servidor TLS).
  Certificados autoassinados; o CN é o `deviceId`
  (`^[a-zA-Z0-9_-]{32,38}$`).
- **Dentro do TLS**: os dois trocam a identidade completa (tipo,
  capacidades); `deviceId` e versão têm que bater com os de fora.
- **Pareamento**: o pedido leva `timestamp` (até 30 min de diferença de
  relógio), prazo de 30 s, e os dois lados mostram uma chave de
  verificação: 8 hexa do SHA-256 das duas chaves públicas + timestamp.
- **Pareamento** (`kdeconnect.pair`, `{"pair": true|false}`): confiança
  fixada no certificado do outro lado (guardado em
  `~/.config/kiremote/trusted/<id>.pem`). Certificado diferente com o mesmo
  id = recusa.
- **Payload**: o remetente abre uma porta (1739–1764) e manda o número em
  `payloadTransferInfo.port`; o outro conecta e recebe `payloadSize` bytes
  por TLS.

Atenção nos testes: o `kdeconnectd` desta máquina ocupa a 1716. Para testar,
ou para o KDE Connect, ou roda num network namespace (como os testes de
integração do kisnitch).

## Plugins

Coluna "Fase": quando entra. Nomes de pacote a conferir no upstream.

| Recurso | Pacotes | Lado desktop | Fase |
|---|---|---|---|
| Ping | `kdeconnect.ping` | notificação; enviar pela bandeja | 2 |
| Bateria | `kdeconnect.battery` (+`.request`) | nível no menu/tooltip, aviso de bateria baixa | 2 |
| Achar o celular | `kdeconnect.findmyphone.request` | item da bandeja | 2 |
| Achar este computador | idem, recebido | toca som em loop (ALSA) até clicar | 2 |
| Clipboard | `kdeconnect.clipboard`, `.clipboard.connect` | XFixes avisa a mudança; só texto | 2 |
| Compartilhar | `kdeconnect.share.request` | recebe arquivos (pasta configurável), URLs (`xdg-open`), texto (clipboard); envia arquivos/URLs | 2 |
| Notificações do celular | `kdeconnect.notification` (+`.request`, `.reply`, `.action`) | `Notify` no D-Bus, ícone do payload, dispensar sincronizado | 2 |
| Telefonia | `kdeconnect.telephony` (+`.request_mute`) | notificação de chamada, silenciar toque | 2 |
| Pausar na chamada | (usa telefonia) | pausa os players MPRIS, retoma depois | 2 |
| Controle de mídia do desktop | `kdeconnect.mpris` (+`.request`) | o celular controla os players daqui (MPRIS), com capa do álbum | 2 |
| Touchpad e teclado remotos | `kdeconnect.mousepad.request` (+`.echo`, `.keyboardstate`) | XTest | 2 |
| Apresentação | `kdeconnect.presenter` | ponteiro laser numa janela ARGB sobreposta; setas | 2 |
| Executar comandos | `kdeconnect.runcommand` (+`.request`) | lista de comandos no .ini | 2 |
| Volume do desktop | `kdeconnect.systemvolume` (+`.request`) | `pactl`, como o xispanel | 2 |
| Bloquear | `kdeconnect.lockdevice` (+`.request`) | comando configurável (o bloqueador do KiDesktop) | 2 |
| Sinal da operadora | `kdeconnect.connectivity_report` | no menu | 2 |
| Navegar arquivos | `kdeconnect.sftp` (+`.request`) | monta com `sshfs` e abre no gerenciador; sem sshfs, abre `sftp://` | 2 |
| Inibir descanso de tela | (pareado e conectado) | `xdg-screensaver`/configurável | 2 |
| Controle de mídia do celular | `kdeconnect.mpris` no outro sentido | janela de controle na interface; opcional: expor como player MPRIS no D-Bus (aí o widget de mídia do xispanel controla o celular) | 3 |
| Teclado para o celular | `kdeconnect.mousepad.request` enviado | campo de texto na interface | 3 |
| SMS | `kdeconnect.sms.*` | conversas, enviar, anexos | 4 |
| Contatos | `kdeconnect.contacts.*` | cache de vCards para mostrar nomes no SMS e nas chamadas | 4 |
| Foto | `kdeconnect.photo.request` | tirar foto com o celular e salvar aqui | 5 |
| Notificações do desktop → celular | `kdeconnect.notification` enviado | escuta `Notify` no D-Bus (`BecomeMonitor`) | 5 |

Fora: `bigscreen`, `digitizer`, `virtualmonitor` (este pode voltar como
extra, ver fase 7).

## Espelhamento (kiremote-mirror)

O KDE Connect não espelha o celular, e no Android não há como capturar a
tela de outro jeito sem app próprio ou root. O `scrcpy-server` resolve:
é um jar que o `adb` copia para o celular e roda com `app_process`; ele
codifica a tela no próprio celular (H.264/H.265 por hardware) e manda
por sockets que o `adb` encaminha. O cliente oficial usa SDL2 + FFmpeg;
o nosso usa Xlib + libavcodec, sem toolkit.

Requisito do lado do usuário, sem como contornar: depuração USB ligada (e,
para Wi-Fi, depuração sem fio, Android 11+).

Peças:

1. **adb**: falar direto com o servidor do adb na porta 5037 (protocolo de
   texto simples: `host:transport:<serial>`, `shell:`, `sync:` para enviar
   o jar, `reverse:`/`forward`). Nada de rodar `adb` a cada comando; só
   `adb start-server` se ele não estiver no ar.
2. **Servidor**: enviar o `scrcpy-server` para `/data/local/tmp`, iniciar
   com a **mesma versão** que o cliente espera (o servidor recusa versão
   diferente), `scid` aleatório. Sockets na ordem vídeo, áudio, controle.
3. **Vídeo**: metadados do codec (id, largura, altura) e depois pacotes com
   cabeçalho de 12 bytes (pts + flags de config/keyframe, tamanho) →
   libavcodec. VA-API opcional pelo hwaccel do libavcodec (libva já
   instalado).
4. **Tela**: janela Xlib; YUV 4:2:0 vai direto por XvShmPutImage (a GPU
   converte e escala, zero cópia de conversão na CPU). Sem Xv: libswscale
   + XShmPutImage. Redesenha só quando chega quadro novo.
5. **Controle**: toque/mouse, roda, teclas, texto, voltar/início/recentes,
   ligar tela, clipboard nos dois sentidos, girar. Mensagens binárias do
   protocolo de controle do scrcpy.
6. **Áudio** (Android 11+): `audio_codec=raw` (PCM) no USB, sem decodificar;
   Opus no Wi-Fi (libavcodec tem decodificador). Saída ALSA sem bloquear,
   como o `audio.c` do kikarinhas.
7. **Wi-Fi sem procurar IP**: o link do KDE Connect já sabe o IP do
   celular. O botão "Espelhar" casa esse IP com os aparelhos do
   `adb mdns services` / `adb devices` e conecta sozinho.

O `scrcpy-server` é Apache-2.0: dá para distribuir junto com aviso. Versão
fixada em `config.mk`, baixado da release com SHA-256 conferido (ou o do
pacote `scrcpy-server` da distro, se a versão bater). Compilar o jar exige
SDK do Android: não faz parte do nosso build.

Metas: 1080p60 decodificado por software com uma fração de um núcleo;
RSS do `kiremote-mirror` dominado pelo libavcodec, e ele só existe com a
janela aberta.

## Interface (kiremote, GTK2)

- Lista de dispositivos (pareados e visíveis), estado, bateria, sinal.
- Diálogo de pareamento com a chave de verificação (o daemon abre a
  interface com `--pair <id>` quando chega um pedido: o `notifd` do
  xispanel não suporta ações).
- Por dispositivo: enviar arquivos (diálogo e arrastar-soltar), enviar
  texto/URL, achar celular, navegar arquivos, espelhar, controle de mídia,
  teclado remoto, plugins ligados/desligados, comandos.
- SMS (fase 4): lista de conversas + conversa, no estilo do app de SMS do
  KDE Connect, sem nada além de GtkTreeView/GtkTextView.
- Textos com gettext (msgids em inglês, `po/` com pt_BR e en), como o
  `kikarinhas-config`.

## Integração com o KiDesktop

- `kisession` inicia o `kiremoted` (`.desktop` de autostart).
- Bandeja: SNI + DBusMenu, que o xispanel já hospeda.
- Notificações: `org.freedesktop.Notifications` (o `notifd` do xispanel ou
  outro). Com ações, se o servidor anunciar `actions` em
  `GetCapabilities`; sem, cai na interface. Tarefa à parte no KiDesktop:
  `notifd` com `ActionInvoked`/`NotificationClosed` (e `inline-reply`, para
  responder mensagens direto da notificação).
- Volume e bloqueio do mesmo jeito que o KiDesktop já faz (`pactl`,
  comando do bloqueador configurável).

## Fases

### 0. Base (feita)

Esqueleto (Makefile, `config.mk`, `-Werror -Wpedantic`, `make test`,
`make asan`, `make lint`), `upstream/` somente leitura com kdeconnect-kde,
kdeconnect-android e scrcpy (versões anotadas), `docs/PROTOCOL.md`,
módulos copiados do kikarinhas, CLAUDE.md.

### 1. Núcleo do protocolo (feita)

Identidade, descoberta UDP, TCP + TLS com papéis invertidos, só v8,
geração do certificado, pareamento com chave de verificação, lista de
confiáveis, reconexão, keepalive, payloads. `kiremote-cli` (`-l`, `--pair`,
`--ping`, `--share`).

Aceite: parear com o app Android e trocar ping nos dois sentidos; dois
`kiremoted` pareando entre si num netns (teste de integração); chave de
verificação igual à do upstream para os mesmos certificados (vetor de
teste calculado à parte); parsers com alvo de fuzz.

Feito:

- Módulos: `packet` (pacotes e identidades), `cert` (chave EC P-256 e
  certificado como o upstream, chave de verificação), `conn` (TCP + TLS sem
  bloquear), `link` (aperto de mão completo), `pair` (máquina de estados),
  `trust` (`trusted/<id>.pem` + `.ini`), `net` (UDP, TCP, broadcast,
  netlink), `transfer` (payloads), `control` (socket JSON), `daemon`,
  plugins `ping` e `share` (só arquivos; texto e link vão para o log).
- Pedido de pareamento vindo do celular: o daemon mostra a chave no log e
  aceita com `kiremote-cli -d <id> --pair` (notificação com botões é da
  fase 2).
- `tests/net/pair.sh`: dois daemons em network namespaces ligados por veth
  descobrem um ao outro, pareiam (mesma chave dos dois lados), trocam ping
  e arquivos nos dois sentidos, reconectam depois de reiniciar sem parear
  de novo e despareiam. Roda no `make test` (e no `make asan`).
- Vetor da chave de verificação em `tests/fixtures/pair-*`, gerado por
  `tools/verification-vector.sh` só com a CLI do `openssl`.
- `tests/fuzz/fuzz_packet.c` (`make fuzz` com libFuzzer; o corpus roda no
  `make test`).

Testado com o app Android de verdade: pareamento, ping nos dois sentidos e
arquivo do celular para o PC funcionam. Do PC para o celular dava erro no
fim: o Android lê o payload até o EOF e o daemon esperava o celular fechar.
Corrigido (`kr_conn_finish`: close_notify + FIN depois de enviar tudo), com
teste que lê até o EOF (`tests/unit/test_transfer.c`); falta reconferir no
celular.

### 2. Plugins do daemon (quase pronta)

Os da tabela marcados como fase 2, mais o ícone SNI com menu. Sem GTK
ainda: tudo pela bandeja, notificações e `kiremote-cli`.

Aceite: cada plugin testado contra o app Android; medir RSS e wakeups do
daemon ocioso (meta: < 5 MB, nenhum wakeup além do keepalive).

Infraestrutura feita: fontes no laço (`kr_source`: o D-Bus, os processos
filhos e os vigias dos plugins entram no mesmo `poll()`), `proc` (filhos sem
bloquear, `pidfd`), `bus` (libdbus de sessão, sem autolaunch), `notify`
(`Notify`, botões só se o servidor tiver `actions`), `mpris` (modelo dos
players), gancho de plugins (`init`, `on_connect`, `on_disconnect`,
`on_packet`, `request`, `describe`, estado por dispositivo) e `[plugins]` no
.ini para ligar e desligar cada um.

Feitos (testados com um celular falso em `tests/net/plugins.sh`; falta
conferir no app de verdade): ping com notificação, bateria (nos dois
sentidos, aviso de bateria baixa), sinal da operadora, achar o celular e
achar este computador, telefonia (notificação da chamada, silenciar, chamada
perdida), pausar na chamada, controle de mídia do desktop (MPRIS) e volume
do sistema com a lista de saídas (`pactl`). Compartilhar já recebia e
enviava arquivos.

Também feitos (testados com o celular falso em `tests/net/desktop.sh`, com
X, D-Bus de sessão e de "sistema" privados; falta conferir no app de verdade):
clipboard (texto; nos dois sentidos, com o aviso de gerenciador de senhas),
compartilhar texto e link, notificações do celular (ícone, atualização,
botões, dispensar), executar comandos (`[runcommand]` no .ini), mouse e
teclado remotos (XTest), ponteiro da apresentação (XShape), bloquear (o
Android desta revisão não tem plugin de bloqueio) e inibir o descanso de tela
(desligado por padrão).

Falta da fase 2: sftp (navegar arquivos) e responder a notificações
(`kdeconnect.notification.reply`, que precisa de campo de resposta na
notificação ou da interface). A bandeja de sistema não é do daemon: fica na
interface GTK (e, no KiDesktop, num widget do xispanel). Medido ocioso, sem
barramento: 8,9 MB de RSS e zero acordadas em 5 s; a meta de 5 MB ainda não
foi atingida (a maior parte é OpenSSL e libs de X e D-Bus carregadas).

### 3. Interface GTK2

`kiremote`: dispositivos, pareamento, envio de arquivos, controle de
mídia do celular, teclado remoto, configurações.

### 4. SMS e contatos

### 5. Espelhamento

`kiremote-mirror`: adb, servidor, vídeo, Xv, controle, clipboard; depois
áudio; depois Wi-Fi casado pelo IP do KDE Connect. Botão na interface e no
menu da bandeja.

### 6. Fechamento dos plugins

Foto, notificações do desktop → celular, migrar pareamentos do KDE
Connect (ler `~/.config/kdeconnect/trusted_devices` e os certificados
dos aparelhos, para não precisar parear de novo, se o celular aceitar o
nosso certificado; senão só importar nomes), mDNS.

### 7. Extras além do KDE Connect

Escolher conforme o custo; nenhum entra no daemon.

- Câmera do celular como webcam (`video_source=camera` do scrcpy →
  v4l2loopback).
- Abrir um app do celular numa janela própria (tela virtual do scrcpy 3,
  `new_display`).
- Gravar o espelhamento (H.264 cru → .mkv com libavformat, só se pedido).
- Celular como segunda tela (o `virtualmonitor` do KDE Connect usa VNC;
  avaliar x11vnc + saída virtual no XiS).

## Riscos

- **Protocolo**: o upstream muda (v8 trocou pareamento e identidade).
  Seguir o `docs/PROTOCOL.md`, atualizar os submódulos com cuidado e testar
  contra o app da loja.
- **Porta 1716 ocupada** enquanto o KDE Connect estiver instalado e rodando:
  o Kiremote divide a UDP 1716 com ele (`SO_REUSEADDR`, como o próprio KDE
  Connect faz), usa a primeira TCP livre (1717) e, se nem a UDP der, avisa
  e segue só anunciando. O celular vê os dois como computadores diferentes.
- **Android limita o clipboard em segundo plano** (10+): o envio do
  celular depende de abrir o app/tile, como no KDE Connect. Não é nosso.
- **scrcpy**: o protocolo entre cliente e servidor muda entre versões.
  Versão fixada e testes com capturas gravadas do vídeo e do controle.
- **Ações nas notificações** dependem do servidor de notificações; sem
  elas, a experiência cai para a interface.
- **Licenças**: GPL-3.0 como os outros projetos. kdeconnect-kde é
  GPL-2.0-or-later (compatível; código traduzido mantém o aviso de
  copyright); scrcpy-server Apache-2.0 (redistribuível com NOTICE).
