# Kiremote — notas para o Claude

Integração celular ↔ desktop para o KiDesktop, compatível com o protocolo v8
do KDE Connect (o app oficial do Android fica do outro lado), em C11 sem
Qt/KF5. Leia o `PLANO.md` antes de começar uma fase (arquitetura, estado de
cada fase) e o `docs/PROTOCOL.md` antes de mexer em qualquer pacote.

Fase atual: 1 feita (testada com o app Android). Fase 2 (plugins do
daemon) quase pronta: falta a resposta a notificações (sftp feito, só testado
com `sshfs` falso); veja o `PLANO.md`.

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
- Testes de rede **nunca** usam o D-Bus, o X ou o áudio do usuário: os
  sockets são arquivos e atravessam o network namespace. Os scripts fazem
  `unset DISPLAY DBUS_SESSION_BUS_ADDRESS ...` e sobem os seus (um
  `dbus-daemon` privado, um Xvfb em display alto a partir do `:40`), com
  `pactl`/`paplay`/`xdg-open`/`xdg-screensaver` falsos no PATH
  (`tests/tools/`). Ao testar à mão, não rode `pactl set-*` no som do usuário.
- Nunca use `pkill -f` por nome genérico (Xephyr, Xvfb, dbus-daemon...): o
  usuário roda sessões aninhadas e isso derrubou uma. Mate só o PID que você
  mesmo iniciou. O display do seu shell (`$DISPLAY`) pode ser uma sessão
  aninhada do usuário: não desenhe nele nem o herde. `KR_TEST_XEPHYR=1`
  mostra o X do teste numa janela, só se o usuário pedir.
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
| `src/control.c` | socket de controle, JSON por linha (veio do kikarinhas) |
| `src/plugins.c`, `src/plugin_*.c` | lista de plugins e capacidades; um arquivo por plugin (ping, share, battery, connectivity, find, telephony, pausemusic, mpris, volume, clipboard, notifications, runcommand, mousepad, presenter, screensaver, lock, sftp) |
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
| `tests/net/pair.sh` | integração: dois daemons em network namespaces |
| `tests/net/plugins.sh`, `tests/net/desktop.sh`, `tests/tools/` | integração dos plugins: celular falso (`fakephone.c`, serve payloads), D-Bus de sessão e de sistema privados com serviços falsos (Python), Xvfb, `xtool.c`, `pactl`/`paplay`/`xdg-*` falsos |
| `tests/fuzz/` | alvos de fuzz, `driver.c` (sem libFuzzer) e corpus |
| `docs/PROTOCOL.md` | o protocolo, conferido no upstream |
| `docs/UPSTREAM.md` | versões dos submódulos e divergências |
| `upstream/` | submódulos: kdeconnect-kde, kdeconnect-android, scrcpy |

## Convenções

- Prefixo `kr_`/`KR_` em tudo que é público; um módulo por arquivo,
  cabeçalho com comentário explicando o porquê e a posse da memória.
- Caminhos com `kr_pathf`.
- `README.md` (pt-BR, padrão) e `README.en.md` andam juntos: mudou um, mude
  o outro.
- Commits em português, terminando com a linha Co-Authored-By.
