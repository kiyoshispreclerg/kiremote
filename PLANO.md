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
                   envio de arquivos, controle de mídia, configurações;
                   a fase 3 também o oferece como módulos do KiDesktop)
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

## Interface (kiremote, GTK2; e a versão KiDesktop)

O conteúdo abaixo vale para as duas interfaces: ambas montam as mesmas
telas a partir da `libkrui` (ver fase 3).

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

Navegar arquivos (sftp) feito: `kiremote-cli --browse-files` monta com
`sshfs` (testado só com `sshfs` falso no `tests/net/plugins.sh`; falta
conferir no app de verdade) ou abre `sftp://`. Falta da fase 2: responder a
notificações (`kdeconnect.notification.reply`, que precisa de campo de
resposta na notificação ou da interface). A bandeja de sistema não é do daemon: fica na
interface GTK (e, no KiDesktop, num widget do xispanel). Medido ocioso, sem
barramento: 8,9 MB de RSS e zero acordadas em 5 s; a meta de 5 MB ainda não
foi atingida (a maior parte é OpenSSL e libs de X e D-Bus carregadas).

### 3. Interfaces: `kiremote-gtk` e `kiremote-kidesktop`

Duas interfaces equivalentes (mesmas funções, mesmas configurações), em
dois pacotes. Uma é para qualquer desktop; a outra é a mesma coisa
integrada ao KiDesktop. O ponto é que **só o hospedeiro muda**: nada de
lógica, de lista de configurações nem de texto é escrito duas vezes.

```
kiremoted ── socket ──┬─ libkrui.a (GTK2, -fPIC)  ← toda a interface vive aqui
                      │    krclient  cliente assíncrono do socket (GLib)
                      │    krmodel   estado → structs/textos, sem GTK (testado)
                      │    widgets   kr_ui_devices_new(), kr_ui_settings_new(),
                      │              kr_ui_pair_dialog(), kr_ui_device_menu()...
                      ├─ kiremote            hospedeiro 1: janela + GtkStatusIcon
                      └─ kiremote-kidesktop  hospedeiro 2: três módulos finos
                           kiconf   aba "Kiremote"      (settings)
                           xisserve página --kiremote   (devices + ações)
                           xispanel widget kiremote     (ícone/badge, sem GTK)
```

Os pontos de extensão do KiDesktop já existem e preveem o Kiremote pelo
nome (`xispanel_widget.h`, `xisserve_page.h`, `kiconf/plugins.h`):

- **kiconf**: `kiremote.desktop` em `<datadir>/kiconf/tabs` com
  `X-KiConf-Module=kiremote-tab.so`, que exporta
  `GtkWidget *kiconf_tab_build(void)` → `kr_ui_settings_new()`.
- **xisserve**: `kiremote.so` em `<libdir>/xisserve/pages`
  (`xisserve --kiremote`), exporta `xisserve_page_entry()` →
  `kr_ui_devices_new()` em versão compacta (lista, bateria, sinal, menu
  de ações: ping, achar, enviar arquivo, navegar arquivos, mais; botões de
  desconectar e de parear/conectar novo).
- **xispanel**: `kiremote.so` em `<libdir>/xispanel/widgets` (+
  `kiremote.widget`), exporta `xispanel_widget_entry()`. Roda dentro do
  xispanel, sem GTK e sem bloquear: só desenha o ícone com o badge
  (aparelhos conectados / pedido de pareamento) e o tooltip; o clique chama
  `xpw_open_xisserve(w, NULL, "--kiremote")`. Usa o `krmodel`, não o GTK.

**Decisões de desenho**

1. *Uma verdade só: o daemon.* Nenhuma interface lê o `.ini` nem fala o
   protocolo; tudo passa pelo socket de controle. Assim as duas veem as
   mesmas coisas e uma mudança de configuração vale para ambas e para a
   CLI.
2. *Configurações geradas de um esquema.* O daemon declara cada chave
   (seção, nome, tipo, padrão, texto traduzido, dependência de plugin) e
   responde por `config-schema`/`config-get`/`config-set`. `kr_ui_settings_new()`
   monta a tela a partir disso. Plugin novo ou opção nova aparece nas duas
   interfaces sem tocar em código de interface, e o `kiremote-cli --set`
   ganha de graça.
3. *Biblioteca estática privada (`libkrui.a`), não `.so` compartilhada.*
   Ela é ligada nos módulos e no `kiremote`. As duas interfaces
   dependem de `kiremote (= ${binary:Version})`, então não há ABI para
   versionar nem pacote `libkrui0` para manter. Custo: o código vai
   duplicado em até 3 objetos (alguns KB), aceitável.
4. *Estado para o widget sem socket.* O `xispanel` não deixa o módulo
   registrar fds, só `xpw_async()` e ticks. O daemon mantém
   `$XDG_RUNTIME_DIR/kiremote-status.json` (escrito por rename, só quando
   algo muda); o widget confere o mtime no tick e relê. Sem processo filho
   por atualização e sem GTK no xispanel. (Plano B: `xpw_async("kiremote-cli
   --status")`.)
5. *Bandeja sem duplicar.* `[ui] tray = auto`: o `kiremote` só põe o
   `GtkStatusIcon` se não estiver no KiDesktop (onde o widget do xispanel
   faz isso). Os dois pacotes podem ficar instalados juntos.
6. *Gettext único.* O mesmo domínio `kiremote`; `ui/` entra no
   `po/POTFILES.in`. Os `.mo` ficam no pacote `kiremote`.

**Árvore e build**

```
src/        daemon e CLI (como hoje; sem GTK)
ui/         libkrui: krclient.c, krmodel.c, widgets, settings.c  (GTK2)
ui/gtk/     main do kiremote (janela, status icon, --pair <id>)
kidesktop/  tab.c, page.c, widget.c + .desktop/.widget
debian/     um fonte, quatro binários (abaixo)
```

O Makefile liga as partes por `pkg-config`: `gtk+-2.0` → `libkrui` e
`kiremote`; cabeçalhos `xisserve_page.h`/`xispanel_widget.h`/`plugins.h`
presentes → módulos do KiDesktop. Sem eles, o daemon e a CLI continuam
compilando sozinhos.

**Pacotes** (fonte único `kiremote`)

| Pacote | Conteúdo | Depende de |
|---|---|---|
| `kiremote` | `kiremoted`, `kiremote-cli`, `.mo`, autostart | openssl, dbus, x11… |
| `kiremote-gtk` | `kiremote`, `.desktop`, ícones | `kiremote (= v)`, GTK2 |
| `kiremote-kidesktop` | `kiremote-tab.so` + `.desktop`, página, widget | `kiremote (= v)`, `kidesktop-core (>= ABI)` |
| `kiremote-mirror` (fase 5) | `kiremote-mirror` | libavcodec, Xv… |

O ABI do widget/página (`XISPANEL_WIDGET_ABI`, `XISSERVE_PAGE_ABI` = 1) é
verificado em tempo de execução pelo KiDesktop; o pacote registra o ABI
contra o qual foi compilado no `Depends`.

**Etapas**

- **3a. API do daemon para interfaces (feita).** Sem GTK. No socket:
  `status` (nome, versão, pid e os aparelhos com bateria, sinal, `plugins`
  que fazem algo com aquele aparelho, `locked`), `subscribe` (eventos
  `device`, `removed`, `pair-request`, `config`, uma linha cada; cliente que
  não lê é derrubado depois de 256 KB pendentes), `config-schema`,
  `config-get`, `config-set` (valor vazio ou nulo volta ao padrão; a
  resposta diz se precisa reiniciar) e o arquivo
  `$XDG_RUNTIME_DIR/kiremote-status.json` ao lado do socket (escrito por
  rename quando algo muda, apagado ao sair; com outro `-s`, fica ao lado
  desse socket, então os testes não tocam o do usuário). O esquema
  (`src/schema.c`) é a única lista de opções: tipo, validação, padrão,
  texto traduzido, se precisa reiniciar, de qual plugin depende; um teste
  garante que todo plugin tem seu interruptor. `kr_config_set/unset/save`
  editam o `.ini` no lugar, mantendo comentários. CLI: `--status`,
  `--schema`, `--get [KEY]`, `--set KEY=VALUE`, `--unset KEY`, `--watch`,
  `--json`. Não entrou: eventos de chamada (a telefonia segue só por
  notificação) e o diálogo de resposta a notificações.
- **3b. `krclient` e `krmodel` (feitas).** Em `ui/`, C puro (sem GTK nem
  GLib; hoje dentro do `libkr.a`, a `libkrui` com widgets vem na 3c).
  `krmodel`: estado → structs, ordem da lista (pedidos de pareamento,
  conectados, ausentes, os outros; por nome), textos (“80% (carregando)”,
  “LTE 3/4”, “Conectado”), dica do painel, **ações** de cada aparelho
  (`kr_dev_actions`: o mesmo menu nas duas interfaces) e os pedidos que elas
  mandam, e leitura do arquivo de estado para o widget (com checagem do pid).
  `krclient`: assina os eventos, mantém o modelo, manda pedidos com
  resposta por callback na ordem, reconecta sozinho (1 s, até 5 s); quem
  hospeda só liga o fd no seu laço. Testes: `test_schema`, `test_control`,
  `test_model`, `test_client` e os eventos/configuração no `pair.sh`.
- **3c. `libkrui` e `kiremote` (feitas).** `ui/gtk/` (GTK2, `-fPIC`,
  `build/libkrui.a`; o Makefile só a monta se `gtk+-2.0` existir): `krui`
  (a conexão no laço do GLib, ouvintes, e os "hooks" de diálogo que o teste
  responde pelo usuário), `krdevices` (lista: ícone, nome, estado, bateria,
  sinal, menu de ações, botões de parear, arrastar arquivos para um aparelho,
  “procurar aparelhos”), `kractions` (pedir arquivos/texto, confirmar o que
  desfaz, enviar, mostrar o erro do daemon; o menu), `krpair` (a pergunta com a
  chave de verificação; fecha sozinha se respondida em outro lugar),
  `krsettings` (todas as configurações montadas do `config-schema`; salva a
  cada mudança; esmaece o que é de um plugin desligado; avisa o que precisa
  reiniciar; edita a lista de comandos). `ui/gtk/main.c` é o `kiremote`: janela
  com Aparelhos e Configurações, `GtkStatusIcon` conforme `[ui] tray`
  (`auto`: não no KiDesktop), `--settings`, `--pair ID`, `--hidden`.
  O esquema ganhou `ui.tray` e os rótulos traduzidos das escolhas. Testes:
  `tests/ui/test_ui.c` (24) sob um Xvfb privado (`tests/ui/run.sh`), com um
  daemon falso no mesmo processo que usa o socket, o esquema e o `kr_config`
  reais; sob ASan só falha o vazamento que passa pelo nosso código (o GTK não
  libera seus singletons). `KR_UI_SHOT=arq.png tests/ui/run.sh build` salva uma
  captura da janela. Falta: instância única (abrir o `kiremote` de novo hoje
  abre outra janela) e o daemon abrir a interface num pedido de pareamento.
- **3d. Módulos do KiDesktop**: aba do kiconf, página do xisserve, widget
  do xispanel. Teste: `XISSERVE_PAGE_PATH`/`XISPANEL_WIDGET_PATH`/
  `KICONF` apontando para `build/`, em Xvfb.
- **3e. Empacotamento e CI**: `debian/`, `.deb` dos quatro pacotes, CI
  rodando `make test`/`asan` com gcc e clang e conferindo que o pacote
  `-kidesktop` instala sem o `-gtk` e vice-versa.
- Depois (já na fase 3 pelo escopo original): controle de mídia do celular
  e teclado remoto na interface; resposta a notificações (fecha o que
  falta da fase 2).

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
