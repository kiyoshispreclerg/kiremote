# Kiremote

[English](README.en.md)

Conecta o computador ao celular, compatível com o **KDE Connect**: use o
app oficial do KDE Connect no Android (ou no iOS) e o Kiremote do lado do
desktop. Feito para o KiDesktop, mas roda em qualquer desktop X11.

A ideia é a mesma dos outros programas do KiDesktop: leve e rápido. O
daemon é C puro com OpenSSL e Xlib, sem Qt nem KDE Frameworks; a interface
é GTK2 e só fica aberta quando você usa.

Também vai ter o que o KDE Connect não tem: **espelhar e controlar a tela
do Android** (pelo `scrcpy-server`, com um cliente próprio, sem SDL).

## Situação

Em desenvolvimento: o núcleo do protocolo (fase 1) está pronto e a fase 2
(plugins) está no meio. O `kiremoted` descobre os aparelhos na rede, pareia,
e já tem: ping, arquivos, bateria, sinal da operadora, achar o celular e
achar o computador, notificação de chamada (e silenciar), pausar a música
na chamada, controle de mídia do desktop pelo celular e volume do sistema.
O `kiremote-cli` controla tudo pelo terminal, e o `kiremote` (GTK2) é a
janela: aparelhos (bateria, sinal, ping, achar, enviar arquivos e texto —
também arrastando arquivos —, navegar, parear e desparear), a pergunta de
pareamento com a chave de verificação e todas as configurações, montadas a
partir do que o daemon informa. Fora do KiDesktop ele põe um ícone na
bandeja (`[ui] tray` no .ini). Os módulos do KiDesktop (aba do kiconf,
página do xisserve, widget do xispanel) vêm a seguir. Veja o [PLANO.md](PLANO.md) para o que está
previsto e em que ordem.

## Uso

```sh
build/kiremote             # a janela (precisa do GTK2 para compilar); --settings, --hidden
```

```sh
build/kiremoted -v &            # o daemon (as mensagens vão para o terminal)
build/kiremote-cli -l           # aparelhos pareados e os que estão por perto
build/kiremote-cli -d Celular --pair      # pede o pareamento e espera
build/kiremote-cli -d Celular --ping
build/kiremote-cli -d Celular --share foto.jpg
build/kiremote-cli -d Celular --find-phone   # o celular toca
build/kiremote-cli -d Celular --mute-call    # silencia a chamada que toca
build/kiremote-cli --stop-ring               # para o som quando o celular procura o computador
build/kiremote-cli -d Celular --share-text "oi"   # vai para o clipboard do celular
build/kiremote-cli -d Celular --share-url https://exemplo.org
build/kiremote-cli --status                  # o daemon e os aparelhos (com --json, para scripts)
build/kiremote-cli --schema                  # todas as configurações, com o texto de cada uma
build/kiremote-cli --get clipboard.auto_share
build/kiremote-cli --set clipboard.auto_share=off   # grava no .ini, mantendo os comentários
build/kiremote-cli --watch                   # eventos (aparelho mudou, pedido de pareamento...) em JSON
```

Notificações, controle de mídia e pausar na chamada precisam de uma sessão
D-Bus (`$DBUS_SESSION_BUS_ADDRESS`); sem ela o daemon funciona, mas só
escreve no log. O volume usa o `pactl`, e o som de "achar o computador",
`paplay`, `pw-play` ou `aplay`. Cada plugin pode ser desligado na seção
`[plugins]` do .ini. Clipboard, mouse/teclado e ponteiro de apresentação
precisam de X11 (`$DISPLAY`); os comandos que o celular pode rodar são só os
da seção `[runcommand]`. Para navegar pelos arquivos do celular
(`kiremote-cli -d CELULAR --browse-files`) o ideal é ter o `sshfs`
instalado; sem ele o daemon entrega o endereço `sftp://` ao `xdg-open`.

No celular, abra o app do KDE Connect: o computador aparece na lista. Dá
para parear dos dois lados; os dois mostram a mesma chave de verificação.
Quando o pedido vem do celular, o daemon mostra a chave no log e você aceita
com `kiremote-cli -d <aparelho> --pair` (ou recusa com `--unpair`). Os
arquivos recebidos vão para a pasta de downloads; a configuração está em
[data/kiremote.ini](data/kiremote.ini).

Se o computador não aparecer no celular, confira se o firewall deixa passar
UDP e TCP de 1716 a 1764, ou adicione o IP do computador no app ("Adicionar
dispositivos pelo IP"). O Kiremote convive com um KDE Connect rodando na
mesma máquina: os dois aparecem no celular como computadores diferentes.

## Compilar

Precisa de um compilador C11, `make`, `pkg-config`, OpenSSL e gettext
(`msgfmt`).

```sh
make
make test        # testes de unidade, fuzz no corpus e dois daemons em network namespaces
build/kiremoted --help
```

## Licença

GPL-3.0-or-later. Veja [LICENSE](LICENSE).

Não é afiliado ao KDE. "KDE Connect" é usado só para dizer com que
protocolo o Kiremote é compatível.
