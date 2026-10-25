# Kiremote — notas para o Claude

Integração celular ↔ desktop para o KiDesktop, compatível com o protocolo v8
do KDE Connect (o app oficial do Android fica do outro lado), em C11 sem
Qt/KF5. Leia o `PLANO.md` antes de começar uma fase (arquitetura, estado de
cada fase) e o `docs/PROTOCOL.md` antes de mexer em qualquer pacote.

Fase atual: 1 feita (testada com o app Android). Fase 2 (plugins do
daemon) quase pronta: falta a resposta a notificações (sftp feito, só testado
com `sshfs` falso). Fase 3 (interfaces `kiremote-gtk` e
`kiremote-kidesktop`, que compartilham a `libkrui`) em andamento: 3a (API do
daemon), 3b (`krclient`/`krmodel`), 3c (`libkrui` e o `kiremote`, uma janela por
sessão) 3d (módulos do KiDesktop) e 3e (`debian/`, scripts e CI) feitas. Versão 0.1.0.
Fase 4 (SMS e contatos) adiada: o KiDesktop não tem onde guardar contatos.
Fase 5 (espelhamento) feita: `kiremote-mirror` com vídeo, som, mouse, teclado,
clipboard, atalhos, Wi-Fi, a aba Mirroring nas configurações e o 4º `.deb`; o que
fica para depois (Xv etc.) está no `PLANO.md`.

O usuário escreve em português (pt-BR) e quer as respostas em português.
Comentários e identificadores no código, em inglês. Todo texto que o usuário
vê (logs, notificações, menus, interface) passa pelo gettext: msgids em
inglês com `_()`/`N_()`/`ngettext()`, traduções em `po/` (en e pt_BR).
Arquivo novo com textos? Ponha em `po/POTFILES.in` e rode `make pot`.

## Regras

- Nunca invente campo de pacote, tipo, porta ou comportamento: confira em
  `upstream/` (somente leitura) e registre em `docs/PROTOCOL.md`, citando o
  arquivo. Divergências de propósito vão para `docs/UPSTREAM.md`.
- Só protocolo v8, como o kdeconnect-kde atual.
- O daemon (`kiremoted`) não usa GTK, nem threads, e nada pode bloquear:
  tudo no mesmo `poll()`. GTK2 só na interface; libavcodec só no
  `kiremote-mirror`.
- Escreva o teste junto com o código. `make test` e `make asan` passam, com
  gcc e clang (`make CC=clang BUILD=build/clang test`).
- Parser de entrada da rede ganha alvo de fuzz (a partir da fase 1).

## Compilar e testar

```sh
make            # build/kiremoted, sem avisos (-Werror -Wpedantic etc.)
make test       # testes de unidade (tests/unit/test_*.c, tests/harness.h)
make asan       # os testes com ASan/UBSan
make test-net   # só tests/net/pair.sh: dois daemons em network namespaces
make test-ui    # só tests/ui/run.sh (precisa de GTK2 e Xvfb)
tests/mirror/run.sh build   # a janela e o clipboard do mirror num Xvfb privado (precisa de xsel)
tests/mirror/camera.sh build # --camera e um processo por celular, com o adb falso e um arquivo no lugar do dispositivo
./scripts/build-deb.sh   # os 4 .deb em dist/ (KIREMOTE_SKIP_DEPS_CHECK=1 e KD_INCDIR=/usr/local/include se o kidesktop-core veio do fonte; SCRCPY_SERVER=arquivo evita baixar o jar)
make fuzz       # libFuzzer (clang): build/fuzz/fuzz_packet tests/fuzz/corpus/packet
make lint       # cppcheck + funções proibidas (strcpy, strcat, sprintf, vsprintf, gets)
make pot        # recria po/kiremote.pot e mescla nos .po
build/kiremoted -v -c /tmp/x.ini -s /tmp/x.sock
build/kiremote-cli -s /tmp/x.sock -l
LANGUAGE=en build/kiremoted --help   # o catálogo vem de build/locale
```

- O `kdeconnectd` desta máquina (KDE Connect 21.12, v7) ocupa a UDP/TCP
  1716 (a UDP com `SO_REUSEADDR`: o `kiremoted` consegue dividir). Testes
  de rede rodam num network namespace (`unshare -rn`, como o
  `tests/net/pair.sh`), nunca na rede do host; não pare o KDE Connect do
  usuário sem pedir.
- Teste novo de comportamento entre daemons vai no `tests/net/pair.sh`;
  lógica sem rede (estados, parsers), em teste de unidade.
- Testes que abrem o kiconf, o xisserve ou o xispanel (`tests/ui/kidesktop.sh`) só os rodam no Xvfb próprio, com HOME, `XDG_*` e rede
  privados. Nunca os rode no `$DISPLAY` do usuário: nem `kiconf --help` (abre
  janela); o xispanel dele é um daemon único.
- Testes de rede **nunca** usam o D-Bus, o X ou o áudio do usuário: os
  sockets são arquivos e atravessam o network namespace. Os scripts fazem
  `unset DISPLAY DBUS_SESSION_BUS_ADDRESS ...` e sobem os seus (um
  `dbus-daemon` privado, um Xvfb em display alto a partir do `:40`), com
  `pactl`/`paplay`/`xdg-open`/`xdg-screensaver` falsos no PATH
  (`tests/tools/`). Ao testar à mão, não rode `pactl set-*` no som do usuário.
- Nunca mate processo por nome (`pkill`, `pgrep | kill`, nem em script de teste): o
  usuário roda o `kiremoted` e o resto de verdade na máquina. Mate só o PID que
  o próprio script iniciou.
- Nunca use `pkill -f` por nome genérico (Xephyr, Xvfb, dbus-daemon...): o
  usuário roda sessões aninhadas e isso derrubou uma. Mate só o PID que você
  mesmo iniciou. O display do seu shell (`$DISPLAY`) pode ser uma sessão
  aninhada do usuário: não desenhe nele nem o herde. `KR_TEST_XEPHYR=1`
  mostra o X do teste numa janela, só se o usuário pedir.
- O `kiremote-mirror` na mão (celular de verdade, USB, `:0` permitido): o
  autosync do clipboard manda para o celular o que o usuário copia, e o
  clipboard dele pode ter senha ou token; não imprima nem guarde
  (`xsel -o`) e não ponha nada seu no clipboard dele. `--tcpip` muda o adbd do
  celular: depois `adb disconnect IP:5555` e `adb -s SERIAL usb`.
- Testes que gravam arquivos: `-c` e `XDG_CONFIG_HOME` apontando para um
  diretório temporário, nunca o `~/.config/kiremote` do usuário. Não leia o
  `~/.config/kdeconnect/privateKey.pem`.
- O daemon e a CLI fixam `LC_NUMERIC=C` (`kr_i18n_init`): sem isso, em pt_BR
  o cJSON não lê nem escreve números fracionários. Código novo que use
  `strtod`/`printf("%f")` herda isso; não chame `setlocale(LC_ALL, ...)` depois.
- Mudar `VERSION` ou outro `-D` de `config.mk` não força recompilação: dê
  `make clean`.

## Estrutura

| Arquivo | Papel |
|---|---|
| `src/kiremoted.c` | daemon: opções, laço `poll()` com signalfd |
| `src/kiremote-cli.c` | CLI: fala com o daemon pelo socket de controle |
| `src/daemon.c` | núcleo: descoberta, links, dispositivos, pareamento, transferências, pedidos do socket |
| `src/packet.c` | pacotes JSON, validação de identidade, filtro de nome (alvo de fuzz) |
| `src/cert.c` | chave e certificado próprios, chave de verificação |
| `src/conn.c` | TCP sem bloquear, texto puro e depois TLS em BIOs de memória |
| `src/link.c` | aperto de mão (identidade, TLS com papéis invertidos) e pacotes de um link |
| `src/pair.c` | máquina de estados do pareamento, sem I/O |
| `src/trust.c` | aparelhos confiáveis (`trusted/<id>.pem` e `.ini`) |
| `src/net.c` | UDP 1716, TCP 1716–1764, endereços de broadcast, netlink |
| `src/transfer.c` | payloads (portas 1739–1764) |
| `ui/gtk/*.c` | `libkrui` (GTK2): lista de aparelhos, ações, pareamento, configurações; `main.c` é o `kiremote`. Só compila com `gtk+-2.0` |
| `kidesktop/*.c` | os módulos do KiDesktop: aba do kiconf (`tab.c`), página do xisserve (`page.c`), widget do xispanel (`widget.c`, sem GTK) |
| `src/control.c` | socket de controle, JSON por linha (veio do kikarinhas); assinantes de eventos com fila de saída limitada |
| `src/schema.c` | a lista das opções do `.ini` (tipo, validação, padrão, texto traduzido): as interfaces e a CLI se montam dela |
| `ui/krmodel.c` | estado dos aparelhos, ordem, textos e ações, sem GTK (as duas interfaces e o widget do xispanel usam) |
| `ui/krclient.c` | cliente assíncrono do socket com reconexão, sem GLib; quem hospeda liga o fd no laço |
| `src/plugins.c`, `src/plugin_*.c` | lista de plugins e capacidades; um arquivo por plugin (ping, share, battery, connectivity, find, telephony, pausemusic, mpris, volume, clipboard, notifications, runcommand, mousepad, presenter, screensaver, lock, sftp) |
| `src/pointer_img.c` | o visual do ponteiro da apresentação em pixels (formas, PNG/GIF com quadros via libpng/giflib), sem X; o `plugin_presenter.c` o põe numa janela com XShape |
| `src/proc.c` | processos filhos sem bloquear (`pactl`, player de som), `pidfd` |
| `src/bus.c` | D-Bus de sessão no `poll()`: chamadas assíncronas, sinais |
| `src/notify.c` | notificações (`Notify`), cai para o log sem servidor |
| `src/mpris.c` | players MPRIS: modelo e comandos |
| `src/x11.c` | conexão X compartilhada (clipboard, XTest, ponteiro) no `poll()` |
| `src/config.c` | `kiremote.ini`: dispositivo, `[network]`, `[share]` |
| `src/ini.c` | .ini lido e editado no lugar (mantém comentários; veio do kikarinhas) |
| `src/json.c`, `vendor/cjson/` | cJSON e caminhos `a.b[0]` |
| `src/log.c` | log em stderr com nome do programa; `-v` liga o debug |
| `src/i18n.c` | `setlocale` + `bindtextdomain` (procura `locale/` ao lado do executável) |
| `src/util.c` | arquivos, caminhos (`kr_pathf` falha em vez de truncar), XDG, relógio |
| `tests/ui/test_widget.c`, `tests/ui/kidesktop.sh` | o widget carregado por um hospedeiro de teste; os três módulos nos programas reais do KiDesktop, num Xvfb privado |
| `tests/ui/test_ui.c`, `tests/ui/run.sh` | os widgets sob um Xvfb privado, contra um daemon falso no mesmo processo |
| `debian/`, `scripts/`, `.github/workflows/` | os três pacotes, os scripts de build/dependências/smoke test e a CI |
| `tests/net/pair.sh` | integração: dois daemons em network namespaces (também eventos, arquivo de estado e `--set`) |
| `tests/net/plugins.sh`, `tests/net/desktop.sh`, `tests/tools/` | integração dos plugins: celular falso (`fakephone.c`, serve payloads), D-Bus de sessão e de sistema privados com serviços falsos (Python), Xvfb, `xtool.c`, `pactl`/`paplay`/`xdg-*` falsos |
| `tests/fuzz/` | alvos de fuzz, `driver.c` (sem libFuzzer) e corpus |
| `mirror/stream.c`, `mirror/adb.c`, `mirror/server.c`, `mirror/ctlmsg.c`, `mirror/keys.c` | espelhamento sem libav (na `libkr`): fluxo do scrcpy-server e respostas do controle (alvos de fuzz), `adb devices -l`, subir o servidor pelo adb no `poll()`, mensagens de controle, teclas X → Android |
| `mirror/session.c` | um `kiremote-mirror` por celular (socket por aparelho; raise/quit/status); libkr, as interfaces perguntam por ele |
| `mirror/v4linfo.c`, `v4l2.c`, `camera.c` | a câmera como webcam: o que há em `/sys` sem root (módulo, dispositivos), a escrita no v4l2loopback (só o driver `v4l2 loopback`), libswscale + inversão |
| `mirror/conf.c` | `[mirror]` do .ini → opções do `kiremote-mirror` (a linha de comando vale mais; as chaves estão no `src/schema.c`) |
| `mirror/main.c`, `decode.c`, `screen.c`, `audio.c` | o `kiremote-mirror`: libavcodec, janela Xlib (libswscale + XShm, clipboard com XFixes), som pelo libpulse dentro do `poll()` (`kr_audio_poll`); só compila com libavcodec/libswscale/xext/xfixes/libpulse |
| `docs/PROTOCOL.md` | o protocolo, conferido no upstream |
| `docs/UPSTREAM.md` | versões dos submódulos e divergências |
| `upstream/` | submódulos: kdeconnect-kde, kdeconnect-android, scrcpy |

## Convenções

- Prefixo `kr_`/`KR_` em tudo que é público; um módulo por arquivo,
  cabeçalho com comentário explicando o porquê e a posse da memória.
- Caminhos com `kr_pathf`.
- `README.md` (inglês, padrão) e `README.pt-BR.md` andam juntos: mudou um,
  mude o outro.
- Commits em inglês (desde 2026-10-23; os antigos estão em português),
  terminando com a linha Co-Authored-By.
