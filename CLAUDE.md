# Kiremote — notas para o Claude

Integração celular ↔ desktop para o KiDesktop, compatível com o protocolo v8
do KDE Connect (o app oficial do Android fica do outro lado), em C11 sem
Qt/KF5. Leia o `PLANO.md` antes de começar uma fase (arquitetura, estado de
cada fase) e o `docs/PROTOCOL.md` antes de mexer em qualquer pacote.

Fase atual: 0 feita. Próxima: 1 (núcleo do protocolo).

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
make lint       # cppcheck + funções proibidas (strcpy, strcat, sprintf, vsprintf, gets)
make pot        # recria po/kiremote.pot e mescla nos .po
build/kiremoted -v -c /tmp/x.ini
LANGUAGE=en build/kiremoted --help   # o catálogo vem de build/locale
```

- O `kdeconnectd` desta máquina (KDE Connect 21.12, v7) ocupa a UDP/TCP
  1716. Testes de rede rodam num network namespace, nunca na rede do host;
  não pare o KDE Connect do usuário sem pedir.
- Testes que gravam arquivos: `-c` e `XDG_CONFIG_HOME` apontando para um
  diretório temporário, nunca o `~/.config/kiremote` do usuário. Não leia o
  `~/.config/kdeconnect/privateKey.pem`.
- Mudar `VERSION` ou outro `-D` de `config.mk` não força recompilação: dê
  `make clean`.

## Estrutura

| Arquivo | Papel |
|---|---|
| `src/kiremoted.c` | daemon: opções, laço `poll()` com signalfd |
| `src/config.c` | `kiremote.ini`: nome e tipo do dispositivo |
| `src/ini.c` | .ini lido e editado no lugar (mantém comentários; veio do kikarinhas) |
| `src/json.c`, `vendor/cjson/` | cJSON e caminhos `a.b[0]` |
| `src/log.c` | log em stderr com nome do programa; `-v` liga o debug |
| `src/i18n.c` | `setlocale` + `bindtextdomain` (procura `locale/` ao lado do executável) |
| `src/util.c` | arquivos, caminhos (`kr_pathf` falha em vez de truncar), XDG, relógio |
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
