<div align="center">

> *"Fiat lux, fiat X11, fiat sessio."*  
> ***Ave, Sesare! Ave, imperator!***  
❤️‍🔥

</div>

# Sesar

> *"Un escritorio, en C puro, sin dependencias absurdas."*

Entorno de escritorio para Linux embebido, escrito en C puro sobre Xlib + FreeType. Sin Python, sin GTK, sin Qt, sin Electron, sin node.

## ADVERTENCIA

**Proyecto experimental.** Desarrollado para Gladiator (escritorio Linux sobre Android via Termux + proot-distro). Solo probado en ese entorno. Si queres usarlo en otro contexto, sos libre de hacerlo, pero no garantizamos que funcione. Probalo bajo tu propio riesgo.

## Que es

Un unico binario que reemplaza a jwm + rofi + polybar + nitrogen + un monton de piezas mas, cada una con sus propias dependencias y configuraciones. Sesar hace todo eso solo:

- **`sesar-shell setup`** — genera `~/.jwmrc` con barra inferior, tema neon, atajos de teclado y menu contextual.
- **`sesar-shell session`** — aplica recursos de xterm y ejecuta jwm.
- **`sesar-shell menu`** — menu de inicio con busqueda, categorias y lista de apps `.desktop`. Se abre desde la barra o con Super+Espacio.
- **`sesar-shell power`** — dialogo de energia / sesion (reiniciar WM, cerrar sesion, cancelar). Super+Esc.
- **`sesar-shell hud`** — HUD de sistema con CPU, RAM y bateria. Se actualiza cada segundo, se queda arriba.
- **`sesar-shell wallpaper`** — pinta el fondo synthwave en la raiz de X (hexagono de marca, horizonte, grilla de perspectiva, estrellas).
- **`sesar-shell xres`** — aplica tema neon a xterm via RESOURCE_MANAGER.
- **`sesar-shell appmenu`** — imprime el menu de apps en XML para JWM.
- **`sesar-shell files`** — abre el gestor de archivos disponible.
- **`sesar-shell uninstall`** — restaura la configuracion anterior de jwm.

## Como funciona

Todo el dibujo se hace sobre un canvas de pixeles propio (memoria malloc-eada), con antialiasing por distance fields. FreeType rasteriza la tipografia, la cachea por glifo y se dibuja sobre el canvas. Despues todo se hace `XPutImage` a la ventana.

No hay toolkit: cada boton, cada panel, cada borde, cada glow, es codigo dibujando pixeles directamente. Eso permite el look neon sin depender de un tema de GTK/Qt ni de un compositor con soporte de shaders.

Las ventanas (menu, power, hud) se dibujan con `override_redirect` para que jwm no las toque, se les aplica forma con la extension XShape (esquinas biseladas), y se quedan arriba.

## Que logra

- **UI completa sin toolkit**: menus, HUD, dialogos, botones, listas.
- **Tema neon consistente**: cyan, magenta, violeta, glow en cada borde.
- **Soporte UTF-8** con FreeType (acentos, simbolos, emoji basico).
- **Integracion con jwm**: barra con paginador, task list, reloj, y menu contextual en el root.
- **Lectura de `.desktop`**: parsea `/usr/share/applications` y `~/.local/share/applications`, con soporte para `Name[xx]` localizado.
- **Wallpaper generativo** con synthwave: hexagono, horizonte, grilla, estrellas, vineta.
- **HUD de sistema** con `/proc/stat`, `/proc/meminfo` y `/sys/class/power_supply`.

## Compilar

Requiere Xlib, Xext y FreeType2.

    cc -O2 -o sesar-shell sesar-shell.c \
        $(pkg-config --cflags --libs x11 xext freetype2) -lm

En Ubuntu/Debian:

    apt install build-essential pkg-config libx11-dev libxext-dev libfreetype-dev

## Uso

    sesar-shell setup       # genera ~/.jwmrc
    sesar-shell session     # aplica recursos + ejecuta jwm
    sesar-shell menu        # menu de inicio
    sesar-shell power       # dialogo de energia
    sesar-shell hud         # HUD de sistema
    sesar-shell wallpaper   # pinta el fondo
    sesar-shell xres        # tema de xterm
    sesar-shell appmenu     # menu XML para JWM
    sesar-shell files       # gestor de archivos
    sesar-shell uninstall   # restaura jwm original

## Variables

- SESAR_SCALE — escala de UI. Default: calculado por tamano de pantalla.
- SESAR_FONT — path a un TTF regular. Default: busca en `$PREFIX/share/sesar` y `/usr/share/fonts`.
- SESAR_FONT_BOLD — path a un TTF bold.
- SESAR_TERM — terminal por defecto. Default `xterm`.
- SESAR_TRAY — alto de la barra en pixeles.

## Correcciones respecto a la version inicial

- Fix en `run_wallpaper`: el pixmap del fondo ahora se guarda en una variable global y no se libera durante la sesion. La version previa hacia `XFreePixmap` sobre el pixmap que el root window tenia asignado como background, lo que dejaba el root sin fondo en el siguiente expose (pantalla negra ciclica cuando algo fuerza un repintado).

## Licencia

MIT. Ver LICENSE.
