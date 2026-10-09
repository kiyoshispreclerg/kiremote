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
