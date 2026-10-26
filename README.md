# BlendNet

Le rendu d'animations ou de scènes complexes sous Blender est une opération très coûteuse en ressources. Un seul poste de travail peut mettre plusieurs heures, voire plusieurs jours, à rendre une animation longue. BlendNet répartit les images à calculer entre plusieurs ordinateurs, avec un rendu CPU ou GPU selon leur matériel.

- [Manuel d'utilisation](#manuel-dutilisation)
- [Paramètres](#paramètres)
- [Dépannage](#dépannage)
- [Compilation](#compilation)
- [Configuration et journal](#configuration-et-journal)

## Manuel d'utilisation

### Comprendre les deux rôles

La **Source** est l'ordinateur qui possède le projet Blender : elle propose le rendu,
répartit les images entre les participants et récupère les résultats. Le **Client**
est un ordinateur qui accepte de calculer une partie du rendu. Le même programme
propose les deux rôles ; chaque participant lance BlendNet sur sa machine.

Un **job** désigne une demande de rendu. Il est découpé en **chunks**, c'est-à-dire
des lots d'images attribués aux clients. Les résultats sont des séquences d'images
PNG, JPEG ou EXR ; le montage en vidéo se fait ensuite dans Blender ou un autre outil.

### Premier démarrage

Après la [compilation](#compilation), lancer l'interface :

```sh
./build/blendnet
```

Sur Windows, lancer `build\Debug\blendnet.exe` avec la configuration Debug.
Le premier démarrage crée automatiquement la configuration et propose un benchmark
facultatif. **Plus tard** permet de continuer sans mesurer la puissance de la machine.

Dans **Paramètres → Général**, choisir les dossiers de travail et de cache Blender,
puis cliquer sur **Enregistrer**. Utiliser un dossier de travail distinct du dossier
contenant les projets `.blend`. Une version de Blender absente du cache est téléchargée
depuis blender.org et vérifiée automatiquement ; cette première préparation peut
prendre plusieurs minutes et nécessite Internet.

Les champs de chemin se remplissent en saisissant ou en collant le chemin du dossier.
Un chemin absolu évite les ambiguïtés ; les chemins relatifs de travail et de cache
sont interprétés depuis le dossier de configuration, pas depuis celui de l'exécutable.

### Se repérer dans l'interface

La barre latérale contient les boutons **Source** et **Client**, puis les pages suivantes :

| Page | Utilisation |
| --- | --- |
| **Projets** | Ajouter des dossiers, choisir un fichier Blender et préparer son partage. |
| **Job courant** | Suivre un job publié, ses images validées et ses chunks ; mettre en pause, reprendre ou annuler. |
| **Clients** | Voir les machines connectées à la source et gérer leur participation. |
| **Mes rendus** | Suivre les calculs et transferts effectués pour les sources auxquelles ce client est connecté. |
| **Paramètres** | Régler l'application, les connexions et les limites de calcul. |

Les boutons de rôle orientent la navigation ; ils ne démarrent pas automatiquement
une source ni une connexion. Le bas de la barre indique si la source locale est
**À l'écoute** ou **Arrêtée**. Ce statut est indépendant des connexions du rôle Client.

En haut de la zone principale, un message indique l'opération en cours ou la dernière
erreur. Pendant une préparation longue, **Annuler l'opération** permet de l'interrompre.
L'interface s'utilise à la souris ou au clavier avec Tab, les flèches et Entrée.

### Partager un projet depuis la Source

1. Enregistrer le projet dans Blender. Rassembler ses textures, bibliothèques liées
   et caches dans le dossier du `.blend` ou ses sous-dossiers. Toute dépendance externe
   doit être déplacée et ses chemins corrigés avant le partage.
2. Dans BlendNet, choisir **Source**, puis **Paramètres → Général**. Indiquer la version
   exacte de Blender à utiliser pour lire le projet et **Enregistrer**.
3. Dans **Paramètres → Serveur**, choisir le port, **Enregistrer**, puis cliquer sur
   **Démarrer la source**. Le port par défaut est `47050`.
4. Cliquer sur **Copier le token** et transmettre aux participants l'adresse de la source,
   le port et ce token d'appairage. Voir les [réglages réseau](#connexion-sur-le-réseau).
5. Dans **Projets**, saisir le chemin du dossier dans **Dossier**, puis cliquer sur
   **Ajouter le dossier**. Les fichiers `.blend` sont recherchés dans ses sous-dossiers.
   **Actualiser** relance la recherche après l'ajout ou la modification de fichiers.
6. Sélectionner un projet et attendre sa lecture par Blender. Le panneau de droite
   affiche sa résolution, son moteur, ses dépendances et une estimation du poids.
7. Ajuster les options du tableau ci-dessous, puis cliquer sur **Share Render**.
   BlendNet prépare l'archive, publie le job et affiche **Job courant**.

| Option de partage | Signification |
| --- | --- |
| **Début / Fin** | Première et dernière images de la plage à rendre. |
| **Pas** | Écart entre deux images : de 1 à 10 avec un pas de 2 produit les images 1, 3, 5, 7 et 9. Utiliser 1 pour toutes les images. |
| **Frames par chunk** | Nombre d'images confiées à un client par lot, 20 initialement. Des lots courts rendent les premiers résultats plus vite ; des lots longs réduisent le nombre d'échanges. |
| **Moteur** | Cycles ou EEVEE. Choisir un moteur compatible avec la scène et les machines participantes. |
| **Format** | PNG, JPEG ou EXR pour les images finales. |
| **Dépendances supplémentaires** | Dossier optionnel du projet à inclure, par exemple pour les caches d'une extension qui ne sont pas détectés automatiquement. |
| **Dossier de sortie** | Destination de la séquence d'images assemblée sur la Source. Vérifier ce chemin avant chaque partage. |

La résolution et les autres réglages de scène se préparent dans Blender. L'archive
contient le `.blend`, ses dépendances détectées et le dossier supplémentaire choisi ;
les autres fichiers voisins ne sont pas ajoutés automatiquement. Les originaux restent
inchangés : les chemins sont adaptés dans des copies destinées au partage.
Les références aux brosses **Essentials** fournies avec Blender servent à l'édition :
elles ne sont pas à transférer et ne bloquent pas le partage, même si leur chemin
pointe vers une installation Snap. Une bibliothèque réellement liée à la scène reste
une dépendance à fournir, quelle que soit son origine.
Les séquences de textures, UDIM et caches d'extensions peuvent nécessiter une préparation
manuelle. Le dossier de travail utilisé pour créer l'archive doit rester **hors du dossier
du projet**. **Retirer** masque un projet dans la bibliothèque sans effacer ses fichiers.

### Participer au rendu depuis un Client

1. Choisir **Client**, puis ouvrir **Paramètres → Client** et cliquer sur **Ajouter une source**.
2. Saisir l'**Adresse IP ou nom d'hôte**, le **Port** et le **Token d'appairage** communiqués
   par la Source. `127.0.0.1` convient uniquement si la Source tourne sur le même ordinateur.
3. Choisir la disponibilité **Disponible**, puis cliquer sur **Enregistrer** et **Connecter**.
   La connexion prépare Blender et détecte le matériel si nécessaire.
4. À réception d'une **Proposition de rendu**, vérifier le projet, le moteur, la plage
   d'images et le poids annoncé. Cliquer sur **Accepter** ou appuyer sur Entrée pour participer.
   **Refuser**, **Plus tard** ou Échap déclinent cette offre ; **Plus tard** ne la met pas
   en attente. Sans réponse avant la fin du délai, elle est refusée.
5. Ouvrir **Mes rendus** pour suivre la réception du projet, sa préparation, le rendu
   et l'envoi des résultats. Une acceptation permet de recevoir plusieurs chunks du
   même job sans nouvelle confirmation pour chaque lot.

**Annuler ma participation** interrompt la participation au job en gardant la connexion
à la Source pour d'autres propositions. **Déconnecter**, dans les paramètres Client,
ferme la connexion. Après une coupure réseau, une reconnexion est tentée automatiquement ;
après une déconnexion demandée ou une exclusion de la connexion par la Source, utiliser
**Connecter** pour revenir.

Plusieurs sources peuvent être enregistrées et connectées. L'historique de **Mes rendus**
concerne la session en cours. Par défaut, les fichiers de travail terminés sont nettoyés
après confirmation de réception ; activer la conservation dans les paramètres pour les garder.

### Suivre et terminer un job

Dans **Job courant**, choisir le projet dans la liste si plusieurs jobs ont été publiés.
La barre **Frames validées** compte les images effectivement reçues et vérifiées par
la Source. Le tableau détaille les chunks, leur client, leur état et leurs échecs.
La vitesse et le temps restant sont des estimations fondées sur les résultats déjà validés.

- **Pause / Reprendre** suspend ou reprend le job.
- **Annuler le job** arrête ce rendu pour tous ses participants.
- **Finaliser avec échecs**, lorsqu'elle est proposée, demande une confirmation pour
  terminer avec une séquence incomplète : certaines images manqueront.

Un client peut avoir terminé son calcul ou afficher **Réception confirmée** alors que
la Source vérifie encore ses résultats. Attendre la fin du job côté Source avant
d'utiliser la séquence finale dans le dossier de sortie. Un chunk interrompu ou en
échec peut être redistribué ; son avancement local ne signifie pas que ses images sont validées.

Dans **Clients**, consulter le matériel, le backend, l'état et la progression de chaque
machine. **Annuler le chunk** retire son lot en cours pour permettre une redistribution ;
**Exclure du job** retire sa participation au job sélectionné tout en gardant sa connexion ;
**Déconnecter** ferme sa connexion à la Source.

## Paramètres

Cliquer sur **Enregistrer** après modification, avant de se connecter ou de lancer une
détection ou un benchmark. Un message en haut de l'écran signale une valeur invalide
ou une sauvegarde refusée. Les valeurs ci-dessous sont celles d'une configuration neuve.

### Général

| Paramètre | Valeur initiale | Utilisation |
| --- | --- | --- |
| Langue | Français | Français ou English. Le changement s'affiche immédiatement ; enregistrer pour le conserver au prochain démarrage. |
| **Dossier de travail** | `jobs` | Stocke les archives, projets reçus et fichiers temporaires de rendu. Prévoir suffisamment d'espace et le placer hors des dossiers de projets à partager. |
| **Cache Blender** | `blender-cache` | Conserve les versions de Blender téléchargées pour les réutiliser. Il est distinct du dossier de travail. |
| **Version Blender** | `4.2.1` | Version exacte, par exemple `4.2.1`. Choisir une version adaptée au projet. |
| **Backend de calcul** | `CPU` | Dispositif utilisé pour le calcul. Les choix GPU disponibles dépendent du matériel et de Blender : CUDA/OPTIX, HIP, ONEAPI ou METAL. |

**Détecter les backends** prépare Blender et recherche les dispositifs utilisables.
Avant cette détection, seul CPU est proposé. Après la détection, choisir le backend
souhaité et **Enregistrer**. Les informations de RAM, VRAM et GPU aident à vérifier
le matériel reconnu ; la détection ne garantit pas que toute scène tiendra en mémoire.

**Lancer le benchmark** calcule une scène de référence et conserve un score local.
Ce test est facultatif et peut être lancé plus tard. Un changement de backend ou de
version Blender efface le score précédent ; relancer le benchmark pour le nouveau choix.

### Client : sources connues

| Champ ou action | Utilisation |
| --- | --- |
| **Ajouter une source** | Crée une entrée à compléter ; choisir une entrée dans la liste pour la modifier. |
| **Adresse IP ou nom d'hôte** | Adresse de l'ordinateur Source, par exemple `192.168.1.20` sur le réseau local. |
| **Port** | Port TCP de la Source, `47050` par défaut. |
| **Token d'appairage** | Code fourni par la Source ; **Afficher** permet de vérifier la saisie masquée. |
| **Connecter / Déconnecter** | Ouvre ou ferme la connexion à la source sélectionnée. Enregistrer les coordonnées avant de se connecter. |
| **Retirer** | Supprime une source de la liste après sa déconnexion. |

### Client : disponibilité et ressources

| Paramètre | Valeur initiale | Utilisation |
| --- | --- | --- |
| **Disponibilité** | Disponible | Seul **Disponible** permet l'attribution de travail. **Occupé — jeu**, **Occupé — travail**, **Ne pas déranger** et **Hors ligne** signalent l'indisponibilité. |
| **Exclure Cycles / Exclure EEVEE** | Décochés | Refuse les jobs utilisant les moteurs cochés. |
| **Limiter à une plage horaire locale** | Désactivé | Rend la machine disponible pendant les heures choisies, sous réserve du statut de disponibilité. En dehors, elle est signalée comme Ne pas déranger. |
| **De (heure) / À (heure)** | 22 / 7 | Heures locales de 0 à 23. `22 → 7` traverse minuit ; deux heures identiques couvrent la journée entière. |
| **Frames maximum par job (0 : illimité)** | 0 | Plafond communiqué à la Source. Dans la version actuelle, le contrôle porte sur la taille de chaque chunk ; ce n'est pas un quota cumulé sur tout le job. |
| **Quota disque en Gio (0 : illimité)** | 0 | Limite contrôlée pour les projets reçus et leur espace de travail. Le cache Blender est séparé ; conserver aussi de l'espace libre sur le disque. |
| **Threads de rendu** | 2 | Nombre de threads CPU utilisé en mode arrière-plan, de 1 à 1024. Une valeur basse laisse davantage de ressources aux autres applications. |
| **Timeout de rendu (secondes)** | 3600 | Délai maximal accordé au rendu d'un chunk, de 1 à 86400 secondes. Augmenter pour les scènes lentes. |
| **Délai de réponse aux offres (secondes)** | 60 | Temps de réponse local à une proposition, de 1 à 3600 secondes. La Source possède également son délai d'expiration. |
| **Rendre en arrière-plan avec la limite de threads** | Activé | Applique la limite de threads configurée. Désactivé, le rendu utilise le nombre de threads matériels détecté. |
| **Arrêter lors d'une interaction avec BlendNet** | Désactivé | Signale temporairement Ne pas déranger lors d'une interaction avec cette interface et permet le retrait du travail en cours. Ne détecte pas l'activité dans les autres applications. |
| **Conserver les projets et résultats après confirmation** | Désactivé | Garde les fichiers de travail après confirmation de réception. Prévoir leur nettoyage et l'espace disque nécessaire. |

La limite de threads concerne le CPU. Le mode arrière-plan ne plafonne ni le pourcentage
d'utilisation du GPU ni sa VRAM. Choisir un backend GPU peut donc fortement solliciter
la carte graphique pendant un rendu.

Pour changer les dossiers, le backend, la version Blender, le quota, les threads, le
timeout de rendu, le mode arrière-plan ou la conservation, **déconnecter les sources**,
modifier, enregistrer puis reconnecter. Il faut également déconnecter une source avant
de modifier ses coordonnées ou son token. Les préférences de disponibilité, d'horaires
et de moteurs sont transmises aux sources connectées et peuvent modifier le travail attribué.

### Serveur

| Paramètre ou action | Utilisation |
| --- | --- |
| **Port** | Port TCP d'écoute, `47050` par défaut, entre 1 et 65535. Arrêter la source pour le modifier, enregistrer puis redémarrer. |
| **Adresse publique à communiquer (manuelle)** | Adresse à transmettre aux clients distants. Elle n'est pas détectée automatiquement et sa saisie ne configure pas le routeur. |
| **Démarrer la source / Arrêter la source** | Active ou coupe l'écoute et les connexions de la source locale. |
| **Token d'appairage / Afficher** | Code demandé aux clients pour se connecter. Il reste masqué tant que son affichage n'est pas demandé. |
| **Copier le token** | Copie le code à transmettre aux participants. |
| **Régénérer le token** | Disponible lorsque la source est arrêtée. Remplace le code ; communiquer le nouveau token aux participants avant leur reconnexion. |

### Connexion sur le réseau

Sur un **réseau local**, communiquer l'adresse locale de la Source et autoriser son
port TCP dans son pare-feu. Les clients saisissent ces coordonnées manuellement.

Pour une connexion par **Internet**, communiquer une adresse publique ou un nom d'hôte
joignable, puis configurer le pare-feu et la redirection du port TCP sur le routeur
de la Source vers son ordinateur. BlendNet ne propose ni détection automatique des
sources, ni relais, ni redirection UPnP. Une connexion entrante impossible avec le
réseau utilisé doit être résolue avant de connecter les participants.

Les échanges utilisent TLS. Lors de la première connexion authentifiée, le Client
mémorise le certificat de la Source ; un changement ultérieur est refusé. Si le certificat
change, vérifier la situation avec le propriétaire de la Source avant de refaire
l'appairage. Transmettre le token uniquement aux participants autorisés.

## Dépannage

| Situation | Vérifications |
| --- | --- |
| Un bouton est grisé | Enregistrer les modifications, attendre la fin de l'opération en cours et consulter le message en haut de la fenêtre. |
| **Share Render** est indisponible | Démarrer la Source, sélectionner un projet, attendre sa lecture et vérifier la plage d'images, le pas, la taille des chunks et le dossier de sortie. |
| Le projet n'apparaît pas | Vérifier le chemin du dossier et la présence de fichiers `.blend`, puis **Actualiser**. Les dossiers cachés et liens symboliques sont exclus de la recherche. |
| La préparation du partage échoue | Consulter le détail de l'erreur : dépendance absente ou externe au dossier du projet, cache à ajouter, ou dossier de travail situé dans le projet. |
| Impossible de se connecter | Vérifier l'écoute de la Source, l'adresse, le port, le token, les pare-feux et, sur Internet, la redirection TCP. |
| Le certificat est refusé | Faire confirmer l'identité de la Source et un éventuel changement de certificat par son propriétaire. |
| Un client connecté ne reçoit pas de travail | Vérifier **Disponible**, les horaires, les exclusions de moteurs, le plafond de frames par rapport aux chunks, le matériel requis et l'acceptation de la proposition. |
| Le premier lancement semble long | Le téléchargement de Blender, la détection et le benchmark peuvent prendre du temps. Consulter l'état en haut de l'écran. |
| Le rendu échoue ou expire | Consulter le journal, vérifier les ressources et dépendances, puis augmenter le timeout si le calcul dépasse la durée autorisée. |
| Le disque se remplit | Vérifier le quota, les dossiers de travail et de cache ainsi que l'option de conservation. Le quota de travail ne limite pas le cache Blender. |
| Le client affiche 100 %, mais le job continue | Attendre le transfert, la vérification et l'assemblage côté Source. Le compteur **Frames validées** fait foi. |

Le **Journal d'événements** est disponible dans **Job courant**, **Clients** et les
paramètres **Serveur**. Pour l'exporter, saisir un chemin de fichier dont le dossier
parent existe dans **Fichier d'export du journal**, puis **Exporter**. Ce journal et
l'historique visuel sont limités à la session ; le journal disque est décrit
[plus bas](#configuration-et-journal).

Le parcours complet est actuellement validé sous **Linux avec rendu CPU**. La validation
sur GPU physique, Windows et macOS reste à effectuer. Pour les détails du parcours
graphique, consulter le [guide de l'interface](docs/interface-p8.md).

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
La phase **P7** raccorde le client complet : projet reçu par TLS, extraction ZIP
sécurisée, Blender réel, ZIP résultat et nettoyage après accusé de réception.
Le mode `blendnet-cli --execute-jobs` et les préférences de rétention sont décrits
dans la [démo client P7](docs/client-p7.md).

La phase **P8** raccorde les vues Dear ImGui Source et Client, les propositions de
rendu, le suivi des jobs, les paramètres persistants et les traductions FR/EN.
Voir le [parcours graphique P8](docs/interface-p8.md) pour préparer un projet,
connecter une source et accepter un rendu.

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
Le [fichier d'exemple](docs/settings.example.json) présente les paramètres persistants.
Les chemins relatifs de travail et de cache Blender sont résolus depuis le dossier de
configuration. L'interface permet de régler les options décrites dans le manuel sans
modifier le JSON à la main. Pour utiliser une configuration indépendante :

```sh
./build/blendnet --config .blendnet/client/settings.json
```

Le dossier de travail accueille les projets, archives et résultats ; le cache conserve
les versions vérifiées de Blender. Dans la CLI, `--render-local` lance un rendu local
et `--execute-jobs` exécute les jobs reçus d'une source. Le backend par défaut est `CPU` ;
`--probe-blender` détecte les backends disponibles et `--benchmark-blender` enregistre
un score. Voir les [exemples du client](docs/client-p7.md) pour les commandes complètes.

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
exposent les services réseau aux outils et à l'interface graphique.
Le style C++ est défini par `.clang-format`.
