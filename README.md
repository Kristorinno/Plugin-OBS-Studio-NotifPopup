# Notificación Emergente + Biblia para OBS

Plugin para OBS Studio que añade notificaciones flotantes estilo iPhone con desenfoque progresivo del fondo, y un módulo completo de presentación de Biblia con control remoto desde el celular.

## ✨ Funciones

- **Notificación emergente:** burbuja animada que entra desde cualquier dirección (arriba, izquierda o derecha), con desenfoque dinámico del fondo.
- **Biblia integrada:** ventana con 3 columnas (o 4 en pantalla completa) con versículos, libros del Antiguo y Nuevo Testamento, capítulos y versículos.
- **Buscador de libros:** filtrá por nombre y encontrá "1 Corintios" o "2 Corintios" escribiendo simplemente "Corintios".
- **Control remoto:** escaneá el código QR y manejá la Biblia desde el celular.
- **Lista de 9 frases:** guardá tus frases más usadas y disparalas con un clic.
- **Indicador de proyección:** los versículos que están al aire se marcan en morado.
- **Atajo de teclado:** mostrá/ocultá la notificación sin usar el mouse.
- **Personalización total:** colores de fondo y letra, fuentes personalizadas, tamaño de letra por columna, tiempos de entrada/salida, sombras (cantidad, ángulo y difuminado) y nivel de desenfoque.

## 📦 Instalación (Para usuarios finales)

1. Descargá el instalador `.exe` desde la sección **Releases**.
2. Cerrá OBS por completo.
3. Ejecutá el instalador y seguí los pasos (te va a pedir permisos de administrador).
4. Abrí OBS y el dock **"Notificación emergente"** aparecerá en el menú **Docks**.

## 📖 Cómo usar

### Notificación
1. Agregá el filtro **"Notificación emergente"** a tu fuente de imagen o video.
2. Abrí el dock desde el menú **Docks**.
3. Seleccioná la fuente con el filtro en el desplegable.
4. Configurá los ajustes con el engranaje o editá las frases con el botón de lista.
5. Usá los botones del 1 al 9 o el atajo de teclado para mostrarla.

### Biblia
1. Presioná **F5** (o usá el botón "Abrir Biblia" en el dock).
2. Agregá tus biblias en formato `.xml` (HolyRics) o `.txt` con el botón **+**.
3. Navegá por los libros, capítulos y versículos.
4. Seleccioná uno o más versículos y presioná **Enter** para proyectarlos.
5. Usá el **control remoto** desde el celular para manejar todo a distancia.

## 💻 Compilación desde el código fuente (Para desarrolladores)

### Requisitos del sistema
- **OBS Studio:** versión 30 o superior (Qt6). Probado en OBS 31.1.1 y 32.2.2.
- **Sistema Operativo:** Windows de 64 bits (el plugin está pensado para Windows; la compilación en macOS/Linux no está testeada).
- **Compilador:** Visual Studio 2022 o Visual Studio 2026 (con la carga de trabajo "Desarrollo para el escritorio con C++" y el Windows 11 SDK 10.0.22621.0).
- **CMake:** versión 3.28 o superior (para VS 2026 se recomienda 4.2.0 o superior).
- **Git:** opcional, pero recomendado para clonar la plantilla oficial.

### Pasos para compilar
1. Descargá o cloná la plantilla oficial de OBS: `github.com/obsproject/obs-plugintemplate`.
2. Copiá tus archivos de la carpeta `src/` y `data/` sobre la plantilla.
3. Editá los 3 archivos de configuración (`CMakeLists.txt`, `CMakePresets.json`, `buildspec.json`) como se explica en la guía.
4. Abrí PowerShell en la carpeta del proyecto y ejecutá:
   `$env:_CL_ = "/WX-"`
   `cmake --preset windows-x64`
   `cmake --build --preset windows-x64`
   `cmake --install build_x64 --config RelWithDebInfo --prefix release`
5. Copiá la carpeta `obs-notif-popup` de `release` a `C:\ProgramData\obs-studio\plugins\`.

Para una guía paso a paso más detallada (con solución de errores comunes), ver el archivo **`guia-compilar-plugin-obs.txt`** incluido en este repositorio.

## 📄 Licencia

GNU General Public License v2.0 (GPL v2). Ver el archivo `LICENSE.md` para más detalles.
