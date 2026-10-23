# BlendNet — jobs et distribution (P5)

Références : RF-13/14/20/21/25/50/51/52/53/54, RNF-03/05, RNF-S4.

## Services

Le métier vit dans `core/jobs/`, sans interface graphique ni Blender :

| Module | Responsabilité |
| --- | --- |
| `Job`, `Chunk`, `FrameRange` | États et comptage des frames, y compris pas et frames négatives |
| `EligibilityFilter` | Moteur exclu, disponibilité, backend, VRAM et limite de frames |
| `JobScheduler` | Découpage, propriété exclusive des chunks, tentatives et complétude |
| `JobOrchestrator` | Offres, acceptations, transferts, attribution, délais et redistribution |
| `ResultAssembler` | Validation bornée des ZIP et images, puis copie de la séquence finale |
| `SimulatedWorker` | Remplacement explicite de RenderProcess pour les tests P5 |

`RenderServer` expose `publishJob`, `pauseJob`, `resumeJob`, `cancelJob`, `cancelTask`
et `finalizeJobWithFailures`. `snapshot().jobs` fournit les états, chunks, frames
validées et échecs. Les événements `JobsChanged` signalent un changement de snapshot ;
`JobRejected` signale une publication invalide.

`SourceConnection::drainEvents()` fournit les offres et contrôles dans `JobMessage`.
`sendJobMessage` permet de répondre avec `JobAccept`, `JobDecline` ou `Progress`.
L'acceptation automatique appartient au simulateur, activé explicitement ; un client
ordinaire attend une décision de l'appelant. La popup viendra en P8.

L'orchestrateur est sérialisé sur le thread réseau. Les opérations disque, ZIP et
vérifications d'images passent par les workers. Aucun callback n'accède à la GUI.

## Attribution et fiabilité

La publication découpe le **nombre de frames à rendre**, en tenant compte de
`frameStep`, en chunks de `chunkSize` frames. Par exemple, `-5…8, pas 3`, par chunks
de deux, donne `[-5,-2]`, `[1,4]`, `[7]`.

Une offre n'est envoyée qu'aux pairs éligibles. L'absence de réponse devient un refus
après 60 secondes par défaut. Le refus porte sur cette offre, sans bannir le client
des prochains jobs. Les tâches expirent après cinq minutes sans progression valide
ni activité du transfert associé ; les délais sont injectables dans `JobTimings`.

Cycles respecte la compatibilité du backend annoncé avec le fournisseur GPU et l'OS
(CPU, NVIDIA CUDA/OptiX, AMD HIP, Intel oneAPI, Apple Metal sur macOS). EEVEE ignore
le backend. Le seuil VRAM reste appliqué. La limite de frames porte sur le chunk,
conformément aux critères du guide. L'ordonnanceur utilise un seul chunk actif par
client, tous jobs confondus ; il respecte ainsi `maxConcurrentJobs` sans chercher à
occuper plusieurs slots d'un même client dans cette phase.

Après acceptation, le projet est transféré et acquitté avant `TASK_ASSIGN`. Le même
projet n'est envoyé qu'une fois par client et job pendant une session. Après
reconnexion, une nouvelle offre précède le transfert ; le checkpoint P4 permet la
reprise des octets déjà reçus.

Un chunk n'appartient qu'à un client. Une déconnexion, un timeout, une erreur de
transfert ou un résultat invalide libère son attribution et incrémente son compteur
d'échecs. Il revient dans la file pour un autre client éligible. Au troisième échec,
il reste `Failed` dans une liste explicite. Le client ayant échoué ne reçoit plus de
chunk de ce job dans cette session. Les nouvelles préférences sont revérifiées.

Chaque attribution utilise un identifiant de tentative tel que `job_c0_a1` ; un
résultat ou une progression d'une ancienne tentative ne peut pas terminer la nouvelle.
Les archives de résultat utilisent `<identifiantTentative>_RESULT_ZIP`. Cette précision
du contrat v1 évite de réutiliser `job_RESULT_ZIP` pour des contenus différents, ce
qui serait incompatible avec les checkpoints immuables de P4. Le projet conserve
`job_PROJECT_ZIP`.

`PROGRESS(100 %)` signifie seulement que le client annonce avoir fini de rendre.
Le chunk reste inachevé jusqu'à la réception et à la validation de son résultat.
Un résultat incomplet fait reprendre le chunk entier. Une fois tous les résultats
validés, `Assembling` revérifie les fichiers et copie la séquence dans le dossier
de sortie, avec les numéros de frame d'origine (`frame_####.png`, `.jpg` ou `.exr`).
Un échec de cette étape déclenche une redistribution. `Done` exige toutes les frames
valides ; l'autre voie est l'appel **explicite** à `finalizeJobWithFailures`, uniquement
lorsque les chunks restants ont épuisé leurs trois essais. Le snapshot conserve alors
`finalizedWithFailures` et la liste des chunks échoués.
Cette finalisation passe également par l'assemblage des seules frames validées ; elle
ne transforme pas les frames manquantes en résultats réussis.

Une pause suspend les nouvelles attributions et le timeout des tâches en cours ; le
client reçoit `JOB_PAUSE`, puis `JOB_RESUME`. Une annulation invalide les attributions
et envoie `TASK_CANCEL`. Les complétions tardives ne réactivent pas le job.

La source peut fournir un callback `ServerOptions.localRenderer` pour rendre en
dernier recours lorsqu'aucun client distant ne peut prendre le chunk. Ses résultats
passent par la même validation. P5 le teste avec un renderer simulé ; le raccordement
au vrai Blender s'appuie désormais sur les [services P6](blender-p6.md) ; l'exécution
complète d'un job reçu sera raccordée en P7.

## Validation des archives

libzip vérifie les archives et leur CRC. Seuls les noms plats attendus du chunk sont
acceptés : chemins traversants/absolus, liens symboliques, doublons, fichiers étrangers,
entrées chiffrées ou manquantes sont refusés. L'extraction en flux est bornée à
512 Mio par image, 2 Gio par archive et 100 000 entrées. L'espace disponible est
vérifié avant extraction ; un staging rejeté est nettoyé.

Les signatures et dimensions sont lues dans les en-têtes PNG, JPEG et EXR, avec une
limite de 64 Kio pour la recherche des dimensions. Les CRC des chunks PNG sont aussi
contrôlés en flux. Les dimensions doivent être cohérentes dans le chunk et entre
chunks du job. Il s'agit d'une vérification d'en-têtes et d'intégrité, sans décodeur
complet des pixels JPEG/EXR. Références de format : [PNG, W3C](https://www.w3.org/TR/png-3/)
et [OpenEXR File Layout](https://openexr.com/en/latest/OpenEXRFileLayout.html).

Le dossier `.blendnet` de la sortie conserve les frames validées par tentative. Les
jobs et décisions restent en mémoire : la persistance/reconstruction de la file des
jobs et la politique de rétention feront partie de la suite. La reprise P4 des fichiers
reste persistante. Utiliser un dossier de sortie distinct par job.

## Démo headless

Compiler avec `./build.sh`, ou utiliser un dossier Make neuf si un cache d'un autre
générateur existe. libzip installé est utilisé ; sinon CMake télécharge la version
fixe 1.11.4. Zlib est requis (`zlib1g-dev` sous Debian/Ubuntu).

Créer un `job.json` local à côté d'une archive projet existante :

```json
{
    "id": "demo-job",
    "projectName": "Démo P5",
    "projectZip": "project.zip",
    "outputDirectory": "output",
    "engine": "CYCLES",
    "frameStart": 1,
    "frameEnd": 60,
    "frameStep": 1,
    "chunkSize": 5,
    "outputFormat": "PNG",
    "blenderVersion": "4.2.1",
    "minimumVramMB": 0
}
```

Les chemins sont relatifs au fichier JSON. Lancer la source :

```sh
./build/blendnet-server-cli --identity-dir .blendnet/server --port 47050 \
    --publish-job /chemin/job.json --wait-clients 3 --exit-after-job
```

Préparer trois configurations dans des dossiers distincts, avec le token de la source
comme dans la [démo P3](reseau-p3.md), puis lancer dans trois terminaux, en adaptant
le chemin et `--machine-name` à chaque client :

```sh
./build/blendnet-cli --config .blendnet/client-1/settings.json --connect 0 \
    --simulate-jobs --machine-name worker-1 --frame-delay-ms 100
```

Après `SIM_TASK_STARTED`, tuer un client ; les autres reprennent son chunk. La source
affiche `JOB état framesValidées framesTotales tentativesÉchouées`, puis
`JOB Done 60 60 …`. La sortie contient de vraies images PNG 1×1 : ce mode teste
l'orchestration, sans rendre la scène Blender. `--decline-jobs` simule un refus ;
`--simulate-local` côté source active le renderer local simulé.

## Tests

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'blendnet-(jobs|cli-jobs)$' --output-on-failure
```

Les tests couvrent le découpage, l'éligibilité, les limites de tentatives, les réponses
tardives, la pause, l'annulation, les ZIP invalides et l'invariant de complétude.
`blendnet-cli-jobs` démarre la source et trois clients, tue brutalement un client
pendant son chunk, puis contrôle les 60 fichiers PNG finaux. Il vérifie aussi que le
projet n'a été envoyé qu'une fois à chaque client survivant.

Validation Linux effectuée dans `build-p5` (GUI + CLI, Unix Makefiles) et
`build-p5-sanitize` (headless, AddressSanitizer/UndefinedBehaviorSanitizer) : **10/10
tests CTest réussis** dans les deux configurations, avec 52 tests unitaires, 12 tests
réseau, 5 tests de transfert et 4 tests de jobs, ainsi que les scénarios des CLI.
Windows et macOS restent à vérifier.
