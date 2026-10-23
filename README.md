# BlendNet
Le rendu d'animations ou de scènes complexes sous Blender est une opération très coûteuse en ressources GPU. Un seul poste de travail peut mettre plusieurs heures, voire plusieurs jours, à rendre une animation longue. BlendNet permet d'exploiter la puissance GPU de plusieurs ordinateurs pour accélérer ces rendus.

## État du développement

Le socle **P1** comprend les cibles CMake en couches, la configuration JSON, le journal,
une fenêtre ImGui vide et une CLI sans interface.

La phase **P2** ajoute `ProtocolCodec` et les 22 types de messages, avec validation des
schémas, framing big-endian de 12 octets et blocs binaires bornés. Voir la
[spécification du protocole v1](docs/protocole-v1.md).

La phase **P3** ajoute le réseau TLS, l'appairage par token, le pin du certificat,
PING/PONG et la reconnexion. Les outils `blendnet-server-cli` et `blendnet-cli --connect`
permettent une [démo de connexion headless](docs/reseau-p3.md).

La phase **P4** ajoute les transferts de fichiers dans les deux sens, le SHA-256 en
flux, les checkpoints de reprise et les quotas de réception. Les deux CLI permettent
une [démo de transfert avec reprise](docs/transfert-p4.md), y compris après redémarrage
du récepteur.

La phase **P5** ajoute les jobs, le découpage en chunks, l'éligibilité des clients,
l'attribution et la redistribution après échec, ainsi que la validation et l'assemblage
des résultats. La [démo des jobs P5](docs/jobs-p5.md) utilise trois clients simulés et
supporte la perte d'un client pendant son chunk.

La phase **P6** ajoute l'installation de Blender vérifiée par SHA-256, la sonde réelle
des backends, le rendu local annulable et le benchmark persisté. La
[démo Blender P6](docs/blender-p6.md) rend un vrai chunk de trois frames en CPU.
Le client complet qui exécute les jobs réseau vient en P7 ; les vues graphiques en P8.

Références : [cahier des charges](docs/cahier-des-charges-blendnet.md) et
[phases P1 à P9](docs/blendnet-spécification-agent-ia-guide-developpement.md).

## Compilation

Prérequis : **CMake ≥ 3.21**, Git et compilateur **C++20** (GCC, Clang ou MSVC).
Le réseau requiert les en-têtes **Asio standalone** et **OpenSSL ≥ 1.1.1**. La GUI requiert
OpenGL 3.3 et GLFW. Les ZIP requièrent Zlib ; CMake utilise libzip, nlohmann/json, GLFW et GoogleTest installés
quand ils sont disponibles, et télécharge les dépendances manquantes depuis GitHub.
Dear ImGui est téléchargé à une version fixe.
P6 requiert aussi **libcurl ≥ 7.85** ; reproc installé est utilisé, ou téléchargé
à une version fixe. L'installation de Blender sous Linux utilise `tar` et `xz`.

### Linux (Debian / Ubuntu, backend X11)

```sh
sudo apt install build-essential cmake git libgl1-mesa-dev \
    libglfw3-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libasio-dev libssl-dev zlib1g-dev libcurl4-openssl-dev tar xz-utils
./build.sh
./build/blendnet
```

`build.sh` utilise CMake avec le générateur **Unix Makefiles** (GNU Make sous Linux).
Il configure, compile puis lance les tests dans `build/`. Si ce dossier contient un cache
d'un autre générateur (par exemple Ninja), le script sauvegarde son contenu dans
`build-backup-XXXXXX/build/`, puis recrée `build/` avec Make. Les dépendances déjà
téléchargées sont réutilisées depuis cette sauvegarde, qui doit rester présente tant
que le nouveau cache CMake référence leurs sources.
Si la migration a été interrompue avant la configuration, relancer le script suffit :
il réutilise les sources disponibles dans la sauvegarde la plus récente. Les compilations
suivantes conservent le cache CMake et les dépendances déjà téléchargées.
Le script s'arrête dès qu'une étape échoue et transmet ses arguments à la configuration :

```sh
./build.sh -DBLENDNET_BUILD_GUI=OFF
```

La compilation utilise deux tâches par défaut ; `CMAKE_BUILD_PARALLEL_LEVEL` permet
d'ajuster ce nombre. Les commandes équivalentes sont :

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Pour une première migration depuis un autre générateur, utiliser `./build.sh` ; les
commandes manuelles ci-dessus supposent un dossier neuf ou déjà configuré pour Make.

### Windows

Installer Visual Studio 2022 avec les outils de développement C++, CMake et Git.
Installer aussi Asio standalone et OpenSSL pour l'architecture ciblée ; renseigner
`ASIO_INCLUDE_DIR` et `OPENSSL_ROOT_DIR` si CMake ne les trouve pas automatiquement.
Depuis un terminal développeur :

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\blendnet.exe
```

### macOS

Installer les outils de compilation Xcode, CMake, Git, Asio standalone et OpenSSL,
puis (renseigner `OPENSSL_ROOT_DIR` si nécessaire) :

```sh
./build.sh
./build/blendnet
```

### Mode sans interface

Cette configuration ne recherche ni OpenGL ni GLFW et ne compile pas ImGui :

```sh
cmake -S . -B build-headless-make -G "Unix Makefiles" \
    -DBLENDNET_BUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-headless-make --parallel 2
ctest --test-dir build-headless-make --output-on-failure
./build-headless-make/blendnet-cli --help
./build-headless-make/blendnet-cli --config /tmp/blendnet/settings.json --init-config
```

Options : `BUILD_TESTING=OFF` pour exclure les tests ; `BLENDNET_TEST_GUI=ON` pour
enregistrer un test de démarrage graphique qui rend trois frames puis se ferme. Ce test
nécessite une session graphique accessible et OpenGL 3.3 (ou `xvfb-run -a ctest ...`
avec un pilote logiciel compatible). Sans cette option, les tests fonctionnent sans écran.

Pour compiler hors ligne, installer les dépendances ou fournir leurs sources locales à
FetchContent : `-DFETCHCONTENT_SOURCE_DIR_JSON=/chemin/json`,
`-DFETCHCONTENT_SOURCE_DIR_GLFW=/chemin/glfw`,
`-DFETCHCONTENT_SOURCE_DIR_IMGUI=/chemin/imgui` et
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/chemin/googletest`. Le dépôt ne contient pas ces sources.
Pour libzip : `-DFETCHCONTENT_SOURCE_DIR_LIBZIP=/chemin/libzip` (ou installer `libzip-dev`).
Pour reproc : `-DFETCHCONTENT_SOURCE_DIR_REPROC=/chemin/reproc`.

## Configuration et journal

La GUI crée `settings.json` au premier démarrage. Un fichier invalide provoque une erreur
sans écraser son contenu. La CLI valide la configuration ; elle ne crée un fichier absent
qu'avec `--init-config`. Les deux acceptent `--config CHEMIN`, `--help` et `--version`.

Emplacements par défaut :

| Plateforme | Configuration |
| --- | --- |
| Linux | `$XDG_CONFIG_HOME/blendnet/settings.json` ou `~/.config/blendnet/settings.json` |
| Windows | `%APPDATA%/BlendNet/settings.json` |
| macOS | `~/Library/Application Support/BlendNet/settings.json` |

Sans répertoire utilisateur disponible, le repli est `.blendnet/settings.json`.
Le [fichier d'exemple](docs/settings.example.json) montre les champs du socle. Les chemins
de travail sert désormais à la réception des fichiers côté client ; le cache sera utilisé
dans les phases suivantes. Le rendu local est disponible avec `--render-local` ;
le rendu des jobs reçus sera raccordé en P7.
Le backend par défaut est `CPU` ; `--probe-blender` détecte les backends disponibles
et `--benchmark-blender` enregistre un score volontairement.

Le journal de la GUI est écrit dans `logs/blendnet.log`, à côté de la configuration,
ainsi que sur stderr. La CLI journalise sur stderr. Les lignes sont horodatées en UTC ;
niveaux acceptés : `debug`, `info`, `warning`, `error`, `off`. Les tokens et le contenu
de la configuration ne sont jamais journalisés par les points d'entrée.

## Architecture

```text
blendnet → blendnet-ui → blendnet-server / blendnet-client → blendnet-core
blendnet-cli → blendnet-client → blendnet-core
blendnet-server-cli → blendnet-server → blendnet-core
blendnet-tests → blendnet-core
blendnet-network-tests → blendnet-server / blendnet-client → blendnet-core
blendnet-transfer-tests → blendnet-server / blendnet-client → blendnet-core
blendnet-job-tests → blendnet-server / blendnet-client → blendnet-core
```

`src/core/` ne dépend d'aucune bibliothèque graphique. Les modules serveur et client
exposent les services réseau aux outils et à la future UI ; les sous-dossiers des phases
suivantes sont réservés.
Le style C++ est défini par `.clang-format`.
