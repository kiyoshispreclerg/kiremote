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

Em começo de desenvolvimento: só a base (fase 0). Veja o
[PLANO.md](PLANO.md) para o que está previsto e em que ordem.

## Compilar

Precisa de um compilador C11, `make`, `pkg-config`, OpenSSL e gettext
(`msgfmt`).

```sh
make
make test
build/kiremoted --help
```

## Licença

GPL-3.0-or-later. Veja [LICENSE](LICENSE).

Não é afiliado ao KDE. "KDE Connect" é usado só para dizer com que
protocolo o Kiremote é compatível.
