# BlendNet — client de rendu complet (P7)

P7 relie les transferts P4 au rendu Blender P6 : `JOB_OFFER` accepté explicitement,
réception du projet vérifié par SHA-256, extraction, installation de la version exacte,
rendu headless, ZIP des images inspectées, transfert du résultat et nettoyage après
le `FILE_ACK` final. Le critère de validation est un vrai chunk rendu depuis une offre
réseau par `blendnet-cli`, sans `--simulate-jobs`.

## Architecture

`src/core/jobs/JobExecutor` contient la logique ; `SourceConnection` reste une façade.
`handle()` consomme les événements de la connexion et retourne rapidement. Un worker
effectue l'extraction, l'installation, le rendu et la compression. Les callbacks
postent les messages/fichiers sur la connexion ; `drainEvents()` expose les changements
d'état à la CLI et aux futures vues P8. Un heartbeat de progression couvre aussi
l'installation et l'extraction longues.

Chaque exécuteur crée un dossier privé aléatoire ; les identifiants de job et de
tentative deviennent des empreintes, jamais des chemins fournis par le serveur.
Une tâche doit correspondre à une offre acceptée : plage, pas, format et motif
`frame_####`. Le backend reste le choix local du client. `BlenderInstaller` vérifie
la version exacte de l'offre ; seuls ses exécutables vérifiés sont lancés.

La pause annule le processus et attend la reprise. La même tentative est alors
relancée dans un dossier frais, avec progression annoncée monotone. L'annulation
et la déconnexion invalident immédiatement la tentative ; aucun ancien résultat
ne part après sa réattribution. Une reconnexion attend une nouvelle offre et une
nouvelle attribution. Les checkpoints P4 permettent de reprendre le projet.
L'annulation d'un envoi retire aussi sa demande de la file de reconnexion. Les
messages d'annulation sont ordonnés après le `FILE_BEGIN` en préparation, avant
celui du fichier suivant ; les ACK tardifs ne retirent pas ce fichier de la file.

À l'arrêt de l'application, arrêter la connexion **avant de détruire l'exécuteur** :
son destructeur annule/rejoint le worker, puis nettoie les fichiers dont le transport
a libéré les lecteurs. C'est l'ordre appliqué dans la CLI.

## Format du projet ZIP

L'archive contient le `.blend` et ses dépendances avec leurs chemins relatifs.
Lorsqu'il n'y a qu'un seul `.blend`, il est sélectionné automatiquement. S'il y en
a plusieurs (bibliothèques liées, par exemple), ajouter à la racine :

```json
{
    "schemaVersion": 1,
    "blendPath": "scenes/main.blend"
}
```

Le fichier s'appelle `blendnet-project.json`. Le chemin sélectionné doit désigner
un `.blend` présent dans l'archive. Un manifeste invalide ou un projet ambigu est
refusé ; aucun chemin de scène arbitraire n'est accepté depuis le réseau.

`src/core/archive/ZipService` prévalide toutes les entrées avant d'écrire :

- Refus des chemins absolus, `..`, antislashs, lecteurs Windows, noms réservés,
  doublons, alias de casse et conflits fichier/dossier.
- Refus des liens symboliques, fichiers spéciaux et archives chiffrées.
- Destination neuve, parents sans liens symboliques, permissions privées et
  suppression du dossier incomplet en cas d'erreur ou d'annulation.
- Lecture par blocs de 64 KiB avec contrôle des tailles et CRC ZIP ; maximum
  100 000 entrées, profondeur 32, 2 GiB par fichier et 8 GiB décompressés.
- Budget réduit au quota restant et vérification de l'espace disque avant extraction.

Les images de résultat sont sélectionnées uniquement dans la plage du chunk,
avec leurs noms attendus, signatures/dimensions et CRC PNG inspectés. Les autres
fichiers ne sont pas envoyés. La compression reste en flux, bornée à 512 MiB par
image et 2 GiB décompressés par ZIP résultat, comme le validateur P5 du serveur.

En cas de rendu partiel, les frames valides partent avec `blendnet-errors.json`
(`schemaVersion`, `jobId`, `chunkId`, `error`, `missingFrames`). Sans frame valide,
le client envoie `FILE_ERR/aborted`. Le serveur P5 exige toujours un chunk complet :
un résultat partiel est conservé dans le transfert reçu, puis le chunk entier est
réattribué selon sa limite de trois échecs. Les frames partielles ne comptent pas
comme chunk validé ; la récupération frame par frame n'est pas implémentée.

## Lancer un client réel

Utiliser une source appairée dans `knownSources` comme dans la
[démo TLS P3](reseau-p3.md). L'acceptation automatique est activée explicitement :

```sh
./build/blendnet-cli --config .blendnet/client/settings.json --connect 0 \
    --execute-jobs --use-blender 4.2.1 --backend CPU --blender-cache .cache/blender
```

`--use-blender` choisit la version de la sonde matérielle initiale (4.2.1 par défaut
en mode `--execute-jobs`). Chaque job utilise ensuite la version exacte de son offre,
téléchargée si nécessaire et vérifiée avant exécution. La sonde fournit un vrai
`GPU_INFO` au serveur avant les offres. Un backend indisponible est refusé.

Le mode accepte `--render-threads N`, `--render-timeout-sec N`, `--quota-bytes N`,
`--transfer-dir PATH` et `--keep-jobs`. `--exit-after-chunk` quitte après réception
confirmée d'un seul chunk ; l'omettre pour traiter plusieurs chunks. Le mode
`--simulate-jobs` demeure réservé aux tests P5 et est incompatible avec ce mode.

Le serveur lit un fichier job comme dans la [démo P5](jobs-p5.md), dont `projectZip`
désigne maintenant un véritable projet, et non le ZIP de fixture P5 :

```json
{
    "id": "rendu-p7",
    "projectName": "Mon animation",
    "projectZip": "project.zip",
    "outputDirectory": "output",
    "engine": "CYCLES",
    "frameStart": 1,
    "frameEnd": 3,
    "frameStep": 1,
    "chunkSize": 2,
    "outputFormat": "PNG",
    "blenderVersion": "4.2.1"
}
```

Les chemins sont relatifs à ce JSON. Lancer la source :

```sh
./build/blendnet-server-cli --identity-dir .blendnet/source --port 47050 \
    --publish-job chemin/job.json --wait-clients 1 --exit-after-job
```

Le client annonce `EXECUTOR accepted`, `extracting`, `installing`, `rendering`,
`uploading`, puis `completed` après l'ACK final. Cet ACK confirme la réception du
ZIP ; seul `JOB Done 3 3 ...` côté source confirme aussi la validation et l'assemblage
des trois frames. Après la sortie du serveur, arrêter le client avec Ctrl+C.

## Rétention et quota

Le dossier de travail, relatif au fichier de configuration par défaut, contient
`transfers/` pour les fichiers P4 et `executor/` pour les tentatives. Ces dossiers
restent distincts afin que les JSON du projet ne soient pas interprétés comme des
checkpoints. `--transfer-dir` remplace ce dossier de travail en mode réel.

Par défaut, l'ACK final d'un résultat déclenche la suppression de son projet extrait,
de ses images et de son ZIP résultat. Le ZIP projet vérifié reste en cache pendant
la session pour les chunks suivants : le protocole v1 n'envoie aucun message
« job terminé ». À la fermeture du client, ce cache et ses checkpoints complets sont
supprimés par l'API de transfert, après vérification de l'identifiant et de l'absence
de writer actif. Les transferts interrompus gardent leurs checkpoints de reprise.
Les tentatives abandonnées sont nettoyées à la fermeture.

`keepCompletedJobs: true` dans les paramètres, ou `--keep-jobs`, conserve les dossiers
et projets après confirmation et à l'arrêt. Valeur par défaut : `false`, compatible
avec les anciennes configurations de schéma 1. Le quota compte réception, extraction,
images et archives ; il est vérifié avant réception/extraction, pendant la progression
du rendu et avant l'envoi. Il s'agit de contrôles applicatifs, pas d'un quota de système
de fichiers : un processus peut dépasser temporairement le budget entre deux contrôles.
Le cache partagé des installations Blender conserve son cycle de vie P6 séparé.

## Validation

La suite quotidienne couvre l'extraction, les chemins dangereux et liens, les budgets,
le manifeste, la sélection des images, le rapport partiel, le processus enfant, la
rétention, le nettoyage après ACK, deux chunks successifs, la pause/reprise, l'annulation,
la reconnexion et l'annulation d'un transfert suivie d'un autre en TLS.

Le test Blender réel optionnel de P6 est étendu au parcours P7 : archive avec scène
imbriquée et manifeste, deux chunks CPU, un seul téléchargement du projet, trois PNG
32×32 validés/assemblés côté serveur, suppression après ACK et à la fin de session.
Il utilise Blender officiel 4.2.1, sans moteur simulé :

```sh
cmake -S . -B build -DBLENDNET_TEST_BLENDER=ON
cmake --build build --parallel 2
ctest --test-dir build -R blendnet-blender-real --output-on-failure
cmake -S . -B build -DBLENDNET_TEST_BLENDER=OFF
```

Le test réutilise `.cache/blender` et peut télécharger la version officielle si elle
est absente. Les backends GPU physiques, Windows et macOS restent à valider. Les
limites de rendu et la suppression optionnelle de l'allocation système libp11-kit
en vérification ASan/UBSan restent décrites dans [Blender P6](blender-p6.md).

Validation Linux x64 : **83 tests unitaires, 12 réseau, 6 transfert et 4 jobs**,
soit **10/10 CTest réussis** en build complet CMake/Make et en headless ASan/UBSan
avec la suppression ciblée de libp11-kit. Le test réel ci-dessus a aussi été exécuté
avec Blender officiel 4.2.1 : deux chunks, 3/3 PNG validés, un seul projet reçu,
nettoyage des tentatives après ACK et du cache complet à la fermeture. La suite
TLS de transfert a passé cinq exécutions consécutives après la correction de
l'ordre des messages d'annulation.
