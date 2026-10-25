# Upstream

Submódulos em `upstream/`, somente leitura. Servem de referência para o
protocolo; o Kiremote não compila nada deles.

| Projeto | Repositório | Commit | Data |
|---|---|---|---|
| kdeconnect-kde | <https://invent.kde.org/network/kdeconnect-kde> | `2845196a3f3db9a65bda4be3ef5b3deb58fb136e` | 2026-10-09 |
| kdeconnect-android | <https://invent.kde.org/network/kdeconnect-android> | `f17585913b875826a29c688842a7fb410467ea0f` | 2026-10-09 |
| scrcpy | <https://github.com/Genymobile/scrcpy> | `a60891aea193d92e7e5c3942700eca63f9d19a5f` (v5.0.1) | 2026-10-08 |

Para atualizar: `git -C upstream/<projeto> fetch && git -C upstream/<projeto>
checkout <commit>`, conferir o que mudou nos arquivos citados em
`docs/PROTOCOL.md` e atualizar esta tabela no mesmo commit.

O kdeconnect-kde e o kdeconnect-android são GPL (ver as licenças de cada
um): código traduzido de lá mantém o aviso de copyright original no
cabeçalho do arquivo. O scrcpy é Apache-2.0.

## Divergências

Lugares onde o Kiremote faz diferente do kdeconnect-kde de propósito.

| Assunto | kdeconnect-kde | Kiremote |
|---|---|---|
| `deviceType` próprio | sempre `desktop` (exceto por `PLASMA_PLATFORM`) | configurável em `[device] type` |
| Broadcast de descoberta | `255.255.255.255` a partir de cada endereço IPv4 local | para o endereço de broadcast de cada interface (`getifaddrs`), num socket só; mesmo efeito, sem um socket por interface |
| Prazo do aperto de mão | 1 s para cada identidade | 10 s para o aperto de mão inteiro (TCP, TLS e as duas identidades), o prazo de leitura do Android |
| Certificado do par | o Qt verifica no handshake (`VerifyPeer` com o certificado guardado como CA) | o OpenSSL aceita qualquer um e o Kiremote compara com o guardado logo depois do handshake, antes de mandar qualquer coisa |
| Dois links ao mesmo tempo para o mesmo aparelho | o mais novo substitui o antigo | igual, exceto quando os dois nascem a menos de 5 s um do outro (os dois lados conectaram ao mesmo tempo): fica, nos dois lados, o que foi aberto pelo id maior; senão cada lado podia fechar o link que o outro guardou |
| Pacote acima de 32 MiB | descarta o que chegou e segue | fecha o link (o aparelho reconecta) |
| Vários arquivos num envio | uma porta para todos, um depois do outro (`CompositeUploadJob`) | uma porta por arquivo, em paralelo (até as 26 portas de 1739–1764) |
| Arquivo recebido | escolhe o nome único no começo e grava direto nele, apagando se falhar (`core/filetransferjob.cpp`) | grava em `<nome>.part` e só dá o nome final (único, `nome (1).ext`) quando completo |
| Ação do controle de mídia | `mprisInterface.call(action)` com o que o celular mandar ("TODO: check valid actions") | só `Play`, `Pause`, `PlayPause`, `Stop`, `Next` e `Previous`; `setLoopStatus` só `None`, `Track`, `Playlist` |
| Áudio (volume, desmutar) | PulseAudioQt | `pactl` (o servidor do PipeWire também responde); só aceita nomes de saída da última lista lida |
| Bateria própria | Solid (UPower) | `/sys/class/power_supply` (tipo `Battery`, fora `scope=Device`) e uevents do kernel; sem polling |
| Achar este computador | toca o som uma vez | repete até 30 s ou até fechar a notificação / `kiremote-cli --stop-ring`; o som é um player externo (paplay, pw-play, aplay) |
| Pausar na chamada | pausa e, opcionalmente, muda o volume para mudo | só pausa (a opção de mudo fica para depois) |
| Chamada tocando | notificação persistente com botão "Mute Call" | igual, mas o botão só existe se o servidor de notificações tiver `actions`; `kiremote-cli --mute-call` cobre o resto |
| Clipboard com segredo de gerenciador de senhas | envia (`sendPassword` padrão ligado) | não envia por padrão (`[clipboard] share_passwords`) |
| Pacote `clipboard.connect` sem texto | envia mesmo vazio | não envia se o clipboard está vazio (o outro lado o apagaria) |
| Link recebido | abre qualquer coisa (`QDesktopServices::openUrl`) | só `http`, `https`, `ftp`, `mailto`, `tel`, `sms` e `geo`, sem espaços; avisa por notificação |
| Desbloquear pelo celular | `Unlock()` do logind, sempre | desligado; precisa de `allow_unlock = true` em `[lock]` |
| Comando de bloquear | `Lock()` do logind | comando configurável (padrão `loginctl lock-session`, que o `xss-lock` do KiDesktop atende) |
| Caminho do ScreenSaver | `/ScreenSaver` | tenta `/org/freedesktop/ScreenSaver` e depois `/ScreenSaver`; sem serviço, `xdg-screensaver suspend` |
| Ponteiro da apresentação | janela Qt/QML transparente | janela circular com XShape, sem compositor, sem capturar cliques |
| Digitar texto | libfakekey | XTest; caracteres fora do layout usam um keycode livre emprestado, com espera entre eles |
| Movimento fracionário do mouse | trunca cada `dx`/`dy` | guarda a fração e soma no próximo |
| Comandos do celular | lista em JSON no config do KDE | seção `[runcommand]` do .ini (o nome da linha é a chave) |
| Navegar pelos arquivos | `sshfs` com a chave do aparelho e `password_stdin`; abre `kdeconnect://` no Dolphin | `sshfs` só com a chave (`BatchMode`, `IdentitiesOnly`; a senha não vai a processo nenhum); abre a pasta montada com `xdg-open`. Sem `sshfs`, `xdg-open sftp://usuário:senha@ip:porta/caminho`. Usa o endereço do link, não o `ip` do pacote; valida `user` (`[A-Za-z0-9_.-]`, sem `-` no começo) e `path` (absoluto, sem `..`); ignora pacotes que não pediu; `[sftp] sshfs = PROGRAMA` troca o programa |
| Número com vírgula decimal (locale pt_BR) | o Qt lê JSON sem olhar o locale | `LC_NUMERIC` fica em `C` no daemon e na CLI (`kr_i18n_init`): o cJSON usa `strtod`/`printf`, e com vírgula decimal rejeitava todo `dx`/`dy` do ponteiro da apresentação |

Do cliente do scrcpy (`upstream/scrcpy/app/src`):

| Assunto | scrcpy | Kiremote |
|---|---|---|
| Caminho do jar no celular | `/data/local/tmp/scrcpy-server.jar` | `/data/local/tmp/kiremote-scrcpy-server.jar`: um scrcpy de outra versão no mesmo celular não troca o nosso jar |
| Túnel | `reverse`, caindo para `forward` se falhar | só `reverse` por enquanto |
| Interface | SDL2 (+ OpenGL) | Xlib: libswscale + XShm (sem Xv: o X moderno não o tem) |
| Áudio | opus por padrão, decodificado com libavcodec e tocado pelo SDL | `audio_codec=raw` (PCM, nada a decodificar; ~1,5 Mbit/s) tocado direto pelo libpulse (o PipeWire também responde), com a fila cortada em 250 ms: prefere falhar a atrasar |
| Clipboard ao colar | Ctrl+V manda o texto do computador com `paste` e não repassa a tecla | só a sincronização automática nos dois sentidos, quando alguém copia (nem ao abrir a janela nem ao ganhar o foco: o que já estava lá pode ser uma senha); Ctrl+V vai como tecla. Sem os atalhos Alt+c/x/v nem o `GET_CLIPBOARD` |
| Atalhos | `MOD` = Alt esquerdo ou Super esquerdo | Alt ou Super de qualquer lado (o X não diz qual), só `h b s m p o n r f` e as setas de volume; não há `MOD+i` (FPS), `MOD+w` (recortar), zoom etc. |
| `--tcpip` | espera `service.adb.tcp.port` | tenta `adb connect` a cada 250 ms por 10 s (mesmo prazo, um passo a menos) |
