# P9 — Fiabilité, installation et CI

P9 ajoute les paquets CPack, leurs ressources et tests de démarrage, le fuzzing du
protocole et la CI GitHub Actions. Les exigences concernées sont RNF-01, RNF-07,
RNF-08 et RNF-09, ainsi que les contrôles de reprise/quota RNF-04 et RF-24.

Le périmètre actif est **Ubuntu et Windows**. Les builds, paquets et validations
macOS sont mis en pause ; la CI ne les exécute plus.

## Installer et démarrer

Les paquets ne contiennent ni Blender, ni configuration utilisateur, ni token.
Blender est installé séparément par l'application avec la vérification P6 habituelle.
La configuration et les travaux restent dans leurs dossiers utilisateur lors d'une
mise à jour de l'application.

| Système | Format | Démarrage |
| --- | --- | --- |
| Debian/Ubuntu | `.deb` | Installer avec `sudo apt install ./blendnet_0.1.0_amd64.deb`, puis lancer **BlendNet** dans le menu ou `blendnet`. |
| Linux utilisant RPM | `.rpm` | Installer avec le gestionnaire de paquets de la distribution, puis lancer `blendnet`. |
| Linux portable | `.AppImage` | Donner le droit d'exécution au fichier, puis le lancer. Sans FUSE, utiliser `APPIMAGE_EXTRACT_AND_RUN=1 ./BlendNet-….AppImage`. |
| Linux portable | `.tar.gz` | Extraire l'archive et lancer `usr/bin/blendnet` depuis le dossier extrait. Les bibliothèques système restent nécessaires. |
| Windows | `.exe` NSIS | Lancer l'installateur ; installation par utilisateur et raccourci **BlendNet**. La configuration utilisateur n'est pas dans le dossier d'installation. |
| Windows portable | `.zip` | Extraire l'ensemble du dossier et lancer `bin/blendnet.exe` en conservant les DLL et `share/`. |

Les paquets Linux construits sur Ubuntu 24.04 nécessitent les versions de glibc et
des bibliothèques indiquées par leurs dépendances ; une AppImage ne remplace pas le
noyau, glibc ou le pilote graphique. OpenGL 3.3 reste nécessaire pour l'interface.
Les paquets Windows ne sont pas signés Authenticode. Cette signature de publication
demande le certificat du distributeur et n'est pas exécutée par la CI.

## Générer les paquets avec CMake/CPack

Après une compilation normale :

```sh
cmake --build build --parallel 2
cpack --config build/CPackConfig.cmake
```

Les sorties sont dans `build/packages/`, avec un fichier `.sha256` par paquet.
Le nom des archives portables précise le système et l'architecture. Les générateurs
par défaut sont **TGZ + DEB** sous Linux et **NSIS + ZIP** sous Windows.
Aucune commande ne publie les paquets sur Internet.

Une installation locale, sans modifier le système :

```sh
cmake --install build --component Runtime --prefix "$PWD/.cache/p9/installed"
python3 tests/integration/installed_package.py .cache/p9/installed
# Avec une session graphique et OpenGL 3.3 :
python3 tests/integration/installed_package.py .cache/p9/installed --gui
```

Le test lance les exécutables depuis un autre dossier, sans `LD_LIBRARY_PATH`
hérité, vérifie leurs bibliothèques et ressources, puis ouvre l'interface pendant
trois images si `--gui` est demandé. La configuration et la capture du test sont
temporaires. Pour tester un `.deb` sans installation système :

```sh
mkdir -p .cache/p9/deb-root
dpkg-deb --extract build/packages/blendnet_0.1.0_amd64.deb .cache/p9/deb-root
python3 tests/integration/installed_package.py .cache/p9/deb-root --gui
```

### Linux : RPM et AppImage

RPM nécessite `rpmbuild` (paquet `rpm` sous Debian/Ubuntu) :

```sh
cpack --config build/CPackConfig.cmake -G RPM
```

AppImage utilise le générateur **External** de CPack et linuxdeploy. Téléchargement
de la version utilisée dans la CI, avec vérification du SHA-256 avant exécution :

```sh
mkdir -p .cache/p9
curl --fail --location --retry 3 -o .cache/p9/linuxdeploy-x86_64.AppImage \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
echo 'c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d  .cache/p9/linuxdeploy-x86_64.AppImage' | sha256sum --check
chmod +x .cache/p9/linuxdeploy-x86_64.AppImage
cpack --config build/CPackConfig.cmake -G External \
  -D "CPACK_LINUXDEPLOY_EXECUTABLE=$PWD/.cache/p9/linuxdeploy-x86_64.AppImage"
python3 tests/integration/appimage_package.py \
  build/packages/BlendNet-0.1.0-Linux-x86_64.AppImage --gui
```

Le générateur AppImage nécessite une compilation avec interface. linuxdeploy regroupe
les dépendances ELF et son plugin crée l'image ; le premier lancement du packager
peut télécharger le runtime AppImage officiel et nécessite donc Internet. La création
et le test utilisent l'extraction automatique, sans montage FUSE.

### Windows

Sous Windows, utiliser Visual Studio 2022, CMake et NSIS. Le manifeste `vcpkg.json`
fixe les versions des dépendances via une baseline. Depuis PowerShell :

```powershell
cmake -S . -B build-windows -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-windows --config Release --parallel 2
ctest --test-dir build-windows -C Release --output-on-failure
cpack --config build-windows/CPackConfig.cmake -C Release
```

Les DLL non système et le runtime Visual C++ sont copiés avec les exécutables.
`BLENDNET_RUNTIME_SEARCH_DIRS` permet d'indiquer des dossiers de DLL supplémentaires
si les dépendances ne viennent pas de vcpkg.

CMake vérifie la compilation et l'édition des liens de `std::jthread`,
`std::stop_token` et des attentes annulables. Une bibliothèque standard incompatible
est signalée dès la configuration.

## Ressources distribuées

Seul le composant CMake **Runtime** est inclus : application, deux CLI, bibliothèques
nécessaires, manuel, documentation et licences. Les en-têtes, tests et exemples des
dépendances FetchContent ne sont pas installés dans les paquets utilisateurs.
Une police **DejaVu Sans** libre et sa licence sont distribuées. L'application la
cherche relativement à son exécutable, y compris après déplacement du dossier.
Les polices système restent un repli pour les exécutions depuis l'arbre de compilation.

Une archive des sources du projet peut être créée séparément :

```sh
cpack --config build/CPackSourceConfig.cmake
```

Les sources des dépendances et leurs versions sont référencées dans CMake/vcpkg ;
voir aussi les [notices tierces](../packaging/THIRD-PARTY.md).

## Robustesse du protocole et des connexions

La suite quotidienne couvre les payloads malformés des **22 types de messages**
envoyés sur de vraies connexions TLS. Le pair fautif est fermé et un client valide
peut ensuite se connecter. Les délais restent actifs pendant une authentification
incomplète ou une trame dont le corps n'arrive jamais. Les tests existants vérifient
aussi les PONG absents côté client et serveur, la reconnexion et les transferts repris.
Les valeurs normales restent : PING toutes les 10 s, expiration à 30 s et délai
initial d'authentification de 10 s.

Le fuzzing est optionnel et nécessite **Clang avec libFuzzer**. Le codec est lui-même
compilé avec la couverture et ASan/UBSan ; le harnais teste le framing et les schémas,
puis le réencodage des messages acceptés :

```sh
cmake -S . -B build-fuzz -G "Unix Makefiles" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBLENDNET_BUILD_GUI=OFF \
  -DBUILD_TESTING=OFF -DBLENDNET_BUILD_FUZZER=ON
cmake --build build-fuzz --target blendnet-protocol-fuzz --parallel 2
python3 tests/protocol-fuzz/seed_corpus.py .cache/p9/fuzz-corpus
mkdir -p .cache/p9/fuzz-artifacts
build-fuzz/tests/protocol-fuzz/blendnet-protocol-fuzz .cache/p9/fuzz-corpus \
  -seed=12345 -max_total_time=60 -max_len=65548 -timeout=5 \
  -artifact_prefix=.cache/p9/fuzz-artifacts/
```

Le corpus initial couvre les types de messages, les champs valides, les JSON cassés,
les clés dupliquées, la profondeur excessive et le framing invalide. Ce fuzz léger
est borné à 65 548 octets ; les limites des gros blocs binaires ont leurs tests unitaires.
Un échec produit une entrée reproductible dans le dossier des artefacts.

Pour instrumenter l'ensemble du code et des tests :

```sh
cmake -S . -B build-sanitize -G "Unix Makefiles" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DBLENDNET_BUILD_GUI=OFF -DBLENDNET_SANITIZERS=ON
cmake --build build-sanitize --parallel 2
LSAN_OPTIONS="suppressions=$PWD/tests/sanitizers/lsan-system.supp" \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  DEBUGINFOD_URLS= \
  ctest --test-dir build-sanitize --output-on-failure
```

La suppression vise uniquement l'allocation système libp11-kit documentée en
[P6](blender-p6.md). `DEBUGINFOD_URLS=` évite les téléchargements de symboles de
débogage système pendant l'arrêt des processus ; ASan, UBSan et la détection de
fuites restent actifs. Dans un bac à sable qui empêche les sockets locales ou l'inspection
des threads par LeakSanitizer, exécuter les vérifications avec les permissions adaptées.

## Quota et journaux

Un test concurrent vérifie que deux sources ne peuvent pas réserver simultanément
le même espace disponible dans une instance Client. Les réservations persistent
après interruption ; le refus `disk_full` ne ferme pas la connexion TLS. Le quota
couvre l'espace de travail déclaré, pas le cache Blender ; il ne remplace pas un
quota imposé par le système d'exploitation.

L'export des événements RNF-07 écrit un fichier temporaire dans le même dossier,
puis remplace la destination après succès. Une destination invalide ne tronque pas
un ancien export. Les retours à la ligne sont échappés et les tokens connus de
l'instance sont masqués. Les fichiers exportés sont privés sous Unix ; les liens
symboliques en destination sont refusés. Le dossier parent doit déjà exister.

## CI et état de validation

Le workflow [ci.yml](../.github/workflows/ci.yml) est déclenché sur push, pull request
et lancement manuel. Il compile avec CMake, exécute CTest, installe le composant
Runtime, génère les paquets et vérifie les installations. Les runners sont Ubuntu
24.04 et Windows 2022. Le job Ubuntu dédié
exécute également ASan/UBSan et 60 secondes de fuzzing. Les paquets et journaux sont
conservés comme artefacts pendant 14 jours ; aucun déploiement ni publication de
release n'est automatique.

Les contrôles graphiques automatiques sont exécutés sous Linux avec Mesa/Xvfb.
Les tests d'installation Windows lancent les points d'entrée réels avec
`--version` et `--help` ; cela ne valide pas leurs pilotes OpenGL ou GPU.
Les tests contrôleur P8 restent soumis aux exclusions de plateforme existantes.

Validation locale Linux : **12/12 tests CTest réussis** en compilation complète
CMake/Make, puis **12/12 en headless Clang ASan/UBSan** avec la suppression système
ci-dessus. La suite comprend 91 tests unitaires actifs, un test Blender réel
optionnel ignoré par défaut, 15 tests réseau, 6 tests transfert, 4 tests jobs,
4 tests contrôleur et les scénarios Python. Le fuzzing instrumenté a terminé
**715 565 exécutions en 61 secondes**, sans erreur détectée.

Les paquets **DEB, TGZ et AppImage x86_64** ont été générés. L'installation dans un
préfixe indépendant et l'extraction du DEB ont passé les contrôles de bibliothèques
et de ressources. L'interface du DEB extrait et l'AppImage réelle ont démarré sur
trois images, avec une configuration temporaire.

Un workflow ajouté au dépôt ne prouve pas qu'il est vert sur GitHub : les résultats
Windows, son installateur, RPM et les GPU physiques restent
à confirmer. Le critère de validation complète de P9 reste conditionné au résultat
de ces exécutions ; l'état local ne les remplace pas.

Références : [CPack](https://cmake.org/cmake/help/latest/module/CPack.html),
[générateur External](https://cmake.org/cmake/help/latest/cpack_gen/external.html),
[linuxdeploy](https://docs.appimage.org/packaging-guide/from-source/linuxdeploy-user-guide.html),
[runners GitHub](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
