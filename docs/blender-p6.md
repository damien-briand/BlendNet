# BlendNet — Blender local (P6)

Références : RF-12/25/41/42, RNF-03/05 et RNF-S5.

## Services

`core/blender/` est headless ; il utilise libcurl pour les téléchargements officiels
et reproc pour les sous-processus, avec des arguments séparés et sans shell.

| Service | Rôle |
| --- | --- |
| `BlenderInstaller` | Version exacte, archive officielle, SHA-256, cache et capacité `VerifiedBlender` |
| `GpuInfoService` | Appareil et backends réellement disponibles dans la version installée |
| `RenderProcess` | Rendu asynchrone, progression, annulation, timeout et validation des images |
| `Benchmark` | Scène de référence de l'application, rendu et score persisté |

L'installation, la sonde et le benchmark proposent des appels bloquants destinés aux
workers. `RenderProcess::start()` possède son thread ; l'UI pourra lire `snapshot()`
et `drainEvents()` et appeler `cancel()` sans attente. `wait()` est réservé au thread
propriétaire hors boucle graphique. La CLI pilote ces services sans interface.

## Installation vérifiée

L'installateur construit l'URL depuis une version numérique `majeure.mineure.patch`
et la plateforme compilée. Aucun chemin ni URL de téléchargement n'est accepté depuis
un pair. Il sélectionne le fichier exact dans la liste `.sha256` publiée à côté des
archives sur [download.blender.org](https://download.blender.org/release/Blender4.2/).

Les requêtes utilisent exclusivement HTTPS vers `download.blender.org`, avec
vérification du certificat et du nom d'hôte ; les redirections ne sont pas suivies.
Le document de checksums est borné à 64 Kio, l'archive à 2 Gio, l'arborescence extraite
à 8 Gio et 100 000 entrées. Un checksum faux supprime l'archive et ne produit aucune
capacité d'exécution. Un verrou de dossier empêche deux installations simultanées
de la même version dans un cache partagé par les processus du même utilisateur.

Sous Linux, l'archive TAR/XZ **déjà vérifiée** est listée puis extraite avec `tar`/`xz`
via reproc. Les noms, types d'entrée, tailles et cibles des liens sont contrôlés avant
extraction. Sous Windows, le ZIP passe par libzip ; les chemins traversants, liens,
doublons et entrées chiffrées sont refusés. Sous macOS, le DMG vérifié est monté en
lecture seule avec `hdiutil`, puis l'application est copiée dans le cache. Les chemins
Windows/macOS sont implémentés mais restent à valider sur ces plateformes.

La publication du cache est atomique après extraction et contrôle du binaire attendu.
Un manifeste conserve le hash de l'archive et une empreinte de toute l'arborescence :
fichiers, bibliothèques, scripts et liens internes. Le logiciel installé est placé en
lecture seule. Les préférences et caches temporaires de chaque processus sont séparés
de cette arborescence. Le cache est privé ; il suppose que le compte utilisateur et
l'application restent de confiance.

Une installation valide peut être réutilisée hors ligne. Son arborescence est
revérifiée à l'ouverture et avant chaque lancement. Un cache modifié provoque un refus,
sans exécuter son contenu. Un Blender du `PATH` ou installé par un gestionnaire système
n'est pas utilisé automatiquement : cette phase exécute les versions du cache vérifié.

## Matériel et backend

La sonde lance un script **fourni par l'application** dans le Blender vérifié et
interroge les appareils Cycles. Elle expose les backends ayant un appareil disponible :
CPU, OptiX, CUDA, HIP, oneAPI et Metal. Le choix appartient au client ; un backend
indisponible est refusé, sans basculement silencieux vers CPU.

Le modèle, le fournisseur, la version Blender, l'OS et la RAM alimentent `GPU_INFO`.
La VRAM NVIDIA est interrogée via NVML lorsqu'elle est disponible. Sous Linux, les
compteurs DRM peuvent fournir la VRAM AMD/Intel lorsqu'un appareil est identifiable
sans ambiguïté. Une mémoire non déterminable reste à zéro ; elle n'est pas estimée.
La détection des GPU et leurs rendus demandent encore une validation matérielle ;
la validation de cette phase utilise Cycles CPU.

`--use-blender VERSION --connect INDEX` envoie ces informations réelles au serveur
TLS. Le mode `--simulate-jobs` de P5 reste explicite et distinct.

## Commande et progression

Le guide plaçait `--cycles-device` parmi les options générales de Blender. Le parser
Cycles le lit après `--`, comme le montre le
[parser Cycles de Blender](https://github.com/blender/blender/blob/v4.2.1/intern/cycles/blender/addon/engine.py).
La commande effective suit cet ordre :

```text
blender --factory-startup --disable-autoexec -noaudio
  -b scene.blend -E CYCLES -s 1 -e 3 -j 1
  -o output/frame_#### -F PNG -x 1 -t 2
  --python-exit-code 72 --python-expr <configuration fixe de l'application> -a
  -- --cycles-device CPU --blendnet-request <paramètres JSON>
```

Le court script de configuration ne reçoit que des données et applique la plage
absolue ; aucun code Python reçu du réseau n'est exécuté. Le chargement automatique
des scripts du `.blend` et les préférences utilisateur sont désactivés.
Le moteur EEVEE utilise l'identifiant annoncé par la sonde ; son rendu ignore le
backend Cycles. PNG, JPEG et EXR restent les formats autorisés.

Les limites de frame et de pas sont lues dans l'API de Blender et vérifiées avant
rendu. Blender 4.2.1 impose notamment un début d'animation non négatif : les entiers
signés acceptés par le codec ne garantissent pas une plage rendable par ce backend.
Une plage non supportée est refusée, sans produire une autre séquence à sa place.

`Fra:NNN` indique la frame en cours, pas une frame validée. Les lignes `Saved:`
mettent à jour le compteur des fichiers annoncés, sans compter les doublons. Une
progression est publiée au moins toutes les cinq secondes pendant le processus,
y compris pour une frame longue sans nouvelle ligne stdout. P5 accepte ces
heartbeats sans les transformer en preuve de résultat.

Stdout et stderr sont drainés simultanément, avec captures bornées et lignes limitées
à 16 Kio. L'annulation et le timeout arrêtent puis récoltent le sous-processus.
Le timeout de rendu concerne le processus de rendu ; l'installation et la sonde ont
leurs propres délais. Après un retour zéro, **toutes** les images attendues sont
revérifiées (noms, en-têtes, dimensions et CRC PNG) avant `Completed`.
Un dossier contenant déjà une frame attendue est refusé pour préserver les résultats
existants et empêcher de valider une ancienne image comme nouveau rendu.

## CLI locale

Prérequis ajoutés sous Debian/Ubuntu : `libcurl4-openssl-dev`, `tar` et `xz-utils`.
CMake utilise reproc installé ou télécharge la version fixe du dépôt (tag v14.2.5).
Exemples avec un cache explicitement local au projet :

```sh
./build/blendnet-cli --blender-cache .cache/blender --install-blender 4.2.1
./build/blendnet-cli --blender-cache .cache/blender --probe-blender 4.2.1 --backend CPU
./build/blendnet-cli --blender-cache .cache/blender --blender-smoke 4.2.1 \
    --output-dir .cache/demo-p6 --backend CPU
```

La démo crée une scène de l'application puis rend trois PNG de 32×32 pixels dans
`.cache/demo-p6/frames/`. Choisir un nouveau dossier pour la relancer.
Pour rendre une scène locale existante :

```sh
./build/blendnet-cli --blender-cache .cache/blender --render-local 4.2.1 \
    --scene /chemin/scene.blend --output-dir /chemin/nouvelle-sortie \
    --backend CPU --engine CYCLES --format PNG \
    --frame-start 1 --frame-end 30 --frame-step 1 \
    --render-threads 2 --render-timeout-sec 3600
```

Le benchmark est volontaire et persiste son score dans le fichier de configuration :

```sh
./build/blendnet-cli --config .cache/settings.json --blender-cache .cache/blender \
    --benchmark-blender 4.2.1 --backend CPU
```

La scène `benchmark-v1` produit trois frames Cycles de 256×256 pixels, 32 samples,
deux threads CPU au maximum demandé au processus et sans débruitage. Le score propre
à BlendNet vaut `100000000 / duréeMs`, borné à un entier positif. Il inclut le
démarrage du renderer, mais pas l'installation, les hashes ni la sonde. Ce score
n'est pas celui de Blender Open Data ; comparer la même version/scène/backend.
Le backend choisi et le score sont sauvegardés sans perdre les autres paramètres.

## Validation et périmètre

Les tests unitaires utilisent un vrai sous-processus de fixture pour contrôler les
arguments, sorties partielles, erreurs, flux saturés, annulation, timeout, checksum
faux, cache modifié et score persisté, sans téléchargement ni Blender.

Le test réel est optionnel pour les compilations quotidiennes :

```sh
cmake -S . -B build -DBLENDNET_TEST_BLENDER=ON
cmake --build build --parallel 2
ctest --test-dir build -R '^blendnet-blender-real$' --output-on-failure
cmake -S . -B build -DBLENDNET_TEST_BLENDER=OFF
```

Il utilise `BLENDNET_BLENDER_TEST_VERSION` (4.2.1 par défaut) et
`BLENDNET_BLENDER_TEST_CACHE` (`.cache/blender` par défaut). Au premier lancement, il
peut télécharger Blender. Il vérifie un vrai chunk CPU de trois frames, un chunk
avec pas de deux, les PNG, un backend absent, le benchmark sauvegardé et la connexion
TLS avec la sonde réelle. Le rendu d'un job reçu, son extraction puis le renvoi de
son ZIP résultat et le nettoyage automatique sont raccordés dans le
[client P7](client-p7.md). La popup de proposition
du benchmark reste en P8.

Sur l'environnement Ubuntu de validation, la libcurl système charge `libp11-kit`,
dont l'initialisation laisse une allocation de locale de 46 octets signalée par LSan.
Le même diagnostic est reproduit avec un programme vide lié à libcurl, sans code
BlendNet. Pour vérifier les autres allocations avec cette version de la dépendance,
une suppression **optionnelle et limitée à cette bibliothèque** est fournie :

```sh
LSAN_OPTIONS="suppressions=$PWD/tests/sanitizers/lsan-system.supp" \
    ctest --test-dir build-p6-sanitize --output-on-failure
```

Elle n'est pas activée par CMake ni par le programme en production.

Validation Linux x64 avec Blender officiel **4.2.1** : **11/11 tests CTest réussis**
avec le test réel activé, dont 67 tests unitaires et les scénarios réseau/transfert/jobs
existants. Le build headless ASan/UBSan passe **10/10 tests CTest** avec la suppression
ci-dessus. Les trois images réelles de 32×32 pixels, le pas de deux, le benchmark
persisté et l'envoi des informations réelles sur TLS ont été contrôlés. Les rendus
GPU et les exécutions Windows/macOS restent à vérifier.
