# BlendNet — Spécification pour agent de développement

> **Document d'entrée pour un agent de code.** Tout ce qui suit est contractuel : implémente exactement ces structures, ce protocole et ces critères d'acceptation. Le document de référence fonctionnel est le cahier des charges (RF-xx / RNF-xx) ; cette spécification en est la transcription exécutable.

## 0. Instructions pour l'agent

1. Suis les **phases P1 → P9** dans l'ordre. Ne passe à la phase suivante que si les critères « Done quand » sont satisfaits.
2. Toute fonctionnalité implémentée cite son identifiant (RF-xx / RNF-xx / RNF-Sx) dans le message de commit.
3. Mets à jour la **checklist de la phase** (§13) au fil du développement.
4. En cas d'ambiguïté non couverte ici : choisir l'option la plus simple qui satisfait les exigences de sécurité (§9) et de fiabilité (RNF-03), et la documenter dans le code.
5. Ne jamais introduire de dépendance hors stack (§2). Ne jamais mettre de logique métier hors de `core/`.

## 1. Identité du projet

- **Nom :** BlendNet
- **One-liner :** application de bureau (Linux/Windows/macOS) qui distribue le rendu d'animations Blender sur les GPU de plusieurs machines via Internet.
- **Modèle :** un seul binaire, deux rôles — **source** (celui qui demande le rendu ; il fait tourner le serveur TCP/TLS) et **client** (celui qui prête son GPU).
- **Réseau :** Internet uniquement, connexion manuelle (IP:PORT + token). Pas de LAN, pas de découverte, pas de relais.
- **Sortie :** séquences d'images uniquement (PNG/JPEG/EXR). Pas de vidéo.
- **Licence :** GPL v3+.

## 2. Stack (fixe, sans exception)

| Domaine | Choix |
|---|---|
| Langage | C++20 |
| GUI | **Dear ImGui** + **GLFW** + **OpenGL 3.3** (tout autre framework UI interdit) |
| Build | CMake ≥ 3.21, CPack pour le packaging |
| Réseau | **Asio** (standalone ou Boost) : `io_context`, acceptor, sockets asynchrones |
| TLS | OpenSSL natif, TLS ≥ 1.2, 1.3 recommandé |
| Sérialisation | JSON (**nlohmann/json**) |
| ZIP | libzip ; zstd optionnel pour le transport |
| Hash | SHA-256 (OpenSSL) |
| HTTP | **libcurl** (téléchargements blender.org uniquement) |
| Process | **reproc** (sous-processus multiplateforme) |
| Threads | `std::jthread` + **files de messages thread-safe** ; pool pour SHA-256/zip ; thread dédié pour l'`io_context` |
| Tests | GoogleTest ou Catch2 |
| Interdits | Qt (toute la bibliothèque), tout GUI autre que Dear ImGui, sockets en clair |

## 3. Architecture et frontières

```text
src/
├── main.cpp                  init GLFW/OpenGL/ImGui + boucle de rendu (~60 fps)
├── core/                     LIB STATIQUE headless — zéro dépendance UI
│   ├── protocol/             ProtocolCodec, MessageTypes, framing binaire
│   ├── network/              TlsServer, ClientSocket, ReconnectPolicy
│   ├── transfer/             TransferEngine (chunk, SHA-256, reprise)
│   ├── jobs/                 Job, Chunk, FrameRange, JobScheduler, ResultAssembler
│   ├── blender/              GpuInfoService, BlenderInstaller, RenderProcess, Benchmark
│   ├── archive/              ZipService (création, extraction sécurisée)
│   ├── config/               AppSettings (settings.json), sources connues, token
│   ├── model/                ProjectInfo, ClientInfo, GpuInfo, ClientPrefs, Progress
│   └── log/                  Logger niveaux réglables
├── server/                   rôle source : RenderServer, ClientSession, JobOrchestrator,
│                             EligibilityFilter, ResultAssembler(wiring)
├── client/                   rôle client : SourceConnection, JobExecutor, RenderProcess(host), BlenderInstaller(host)
└── ui/                        imgui/ (init, thème, polices UTF-8) + views/ (aucune logique métier)
tools/blendnet-cli            client simulé headless (tests, démos)
tests/                         unit/, integration/, protocol-fuzz/
```

**Règles de dépendance (vérifiées par le build) :** `ui → server|client → core`. Le core n'inclut jamais server/client/ui. `blendnet-cli` dépend de `core` + `client` uniquement.

## 4. Protocole réseau (spécification binaire)

### 4.1 Framing

Tout octet circulant sur le socket TLS est un **message** :

```text
HEADER (14 octets, big-endian):
  [0..2)  magic    : u16 = 0x5253 ('RS')
  [2..3)  reserved : u8 = 0
  [3..4)  msgType  : u8  (voir §4.2)
  [4..8)  flags    : u32 = 0 (réservé)
  [8..14) length   : u32 = taille en octets du payload
PAYLOAD : length octets — JSON UTF-8 pour les messages de contrôle ;
          binaire pour FILE_DATA (voir §4.4).
```

`ProtocolCodec` est la SEULE classe autorisée à lire/écrire ce framing. Toute trame dont le magic/length est invalide → fermeture immédiate de la connexion + log.

### 4.2 Messages de contrôle (payload JSON)

| ID | Type | Sens | Schéma JSON (champs obligatoires) |
|---|---|---|---|
| 0 | `HELLO` | C→S | `{"protocolVersion":int, "userName":str, "machineName":str, "token":str}` |
| 1 | `HELLO_OK` | S→C | `{"protocolVersion":int, "serverName":str}` |
| 2 | `HELLO_ERR` | S→C | `{"reason":"bad_token"|"bad_version"|"banned"}` puis fermeture |
| 3 | `GPU_INFO` | C→S | `{"gpuModel":str, "gpuVendor":"nvidia"|"amd"|"intel"|"apple"|"other", "vramMB":int, "ramMB":int, "os":"linux"|"windows"|"macos", "blenderVersion":str\|null, "backend":"OPTIX"|"CUDA"|"HIP"|"ONEAPI"|"METAL"|"CPU", "benchmarkScore":int\|null}` |
| 4 | `CLIENT_PREFS` | C→S | `{"excludedEngines":["EEVEE"|"CYCLES", ...], "availability":"AVAILABLE"|"BUSY_GAME"|"BUSY_WORK"|"DND"|"OFFLINE", "maxFramesPerJob":int\|null, "maxConcurrentJobs":int, "quotaGB":int\|null}` |
| 5 | `PING` / 6 `PONG` | bi | `{}` — toutes les 10 s, timeout 30 s → déconnexion |
| 10 | `JOB_OFFER` | S→C | `{"jobId":str, "projectName":str, "engine":"CYCLES"|"EEVEE", "frameStart":int, "frameEnd":int, "frameStep":int, "zipBytes":int, "blenderVersion":str, "outputFormat":"PNG"|"JPEG"|"EXR"}` |
| 11 | `JOB_ACCEPT` | C→S | `{"jobId":str}` |
| 12 | `JOB_DECLINE` | C→S | `{"jobId":str}` |
| 20 | `TASK_ASSIGN` | S→C | `{"jobId":str, "chunkId":str, "frameStart":int, "frameEnd":int, "frameStep":int, "outputPattern":str, "outputFormat":str}` |
| 21 | `PROGRESS` | C→S | `{"jobId":str, "chunkId":str, "currentFrame":int, "framesDone":int, "framesTotal":int, "percent":float, "etaSec":int\|null}` |
| 22 | `TASK_CANCEL` | S→C | `{"jobId":str, "chunkId":str}` → le client stoppe le processus de rendu et supprime les fichiers du chunk |
| 23 | `JOB_PAUSE` / 24 `JOB_RESUME` | S→C | `{"jobId":str}` |
| 25 | `CLIENT_KICK` | S→C | `{"reason":str}` puis fermeture |
| 26 | `STATUS_UPDATE` | C→S | changement de `CLIENT_PREFS` en cours de session (même schéma que ID 4) |

### 4.3 Transfert de fichiers (binaires)

Un « fichier » = un flux identifié, envoyé par blocs, reprise possible :

| ID | Type | Sens | Contenu |
|---|---|---|---|
| 30 | `FILE_BEGIN` | les 2 | JSON : `{"fileId":str, "kind":"PROJECT_ZIP"\|"RESULT_ZIP", "totalBytes":int, "sha256":hexstr, "chunkSize":int (défaut 1 MiB)}` |
| 31 | `FILE_DATA` | les 2 | BINAIRE (pas JSON) : `[u32 fileId][u32 seq][u32 dataLen][dataLen octets]` |
| 32 | `FILE_END` | les 2 | JSON : `{"fileId":str, "sha256":hexstr}` |
| 33 | `FILE_ACK` | récepteur→émetteur | JSON : `{"fileId":str, "nextSeq":int}` — accusé par lot ; sert aussi de **reprise** : le récepteur qui se reconnecte renvoie `FILE_ACK` avec son `nextSeq` courant, l'émetteur reprend à ce point |
| 34 | `FILE_ERR` | les 2 | JSON : `{"fileId":str, "reason":"sha_mismatch"\|"disk_full"\|"aborted"}` |

Règles : à la réception de `FILE_END`, vérifier SHA-256 → `FILE_ACK` final ou `FILE_ERR`. Un fichier reçu s'écrit en flux dans le dossier de travail (jamais entièrement en RAM). Numéroter les fichiers `fileId = jobId + "_" + kind`.

### 4.4 Ordre imposé d'une session client

```text
connect(TLS) → HELLO → HELLO_OK → GPU_INFO → CLIENT_PREFS
→ (attente ; PING/PONG périodiques)
→ JOB_OFFER → JOB_ACCEPT
→ FILE_BEGIN(PROJECT_ZIP) → FILE_DATA* → FILE_END → FILE_ACK
→ TASK_ASSIGN → PROGRESS* → [Nouveau TASK_ASSIGN possible]*
→ FILE_BEGIN(RESULT_ZIP) → FILE_DATA* → FILE_END → FILE_ACK
```

## 5. Modèle de données

```cpp
struct Job {
    std::string id;               // UUID
    std::string projectName;
    std::string blendPath;        // chemin local source
    std::vector<std::string> dependencies; // chemins relatifs (textures, HDRI, caches)
    std::string engine;           // "CYCLES" | "EEVEE"
    int frameStart, frameEnd, frameStep;
    int chunkSize;                // frames par chunk (défaut 20)
    std::string outputFormat;     // "PNG" | "JPEG" | "EXR"
    std::string blenderVersion;   // ex "4.2.1" — imposée à tous les clients
    enum class State { Draft, Published, Recruiting, Rendering, Paused, Assembling, Done, Cancelled } state;
};

struct Chunk {
    std::string id;               // jobId + "_c" + n
    int frameStart, frameEnd;
    enum class State { Pending, Assigned, Downloading, Rendering, Uploading, Done, Failed } state;
    std::string assignedClientId; // "" si Pending
    int attempts = 0;             // max 3 tentatives avant marquage Failed
};

struct GpuInfo { std::string model, vendor; int vramMB, ramMB; std::string os;
                 std::string blenderVersion; std::string backend; std::optional<int> benchmarkScore; };

struct ClientPrefs { std::set<std::string> excludedEngines; std::string availability;
                    std::optional<int> maxFramesPerJob; int maxConcurrentJobs; std::optional<int> quotaGB; };

struct ClientSession {
    std::string id;               // UUID attribué par le serveur
    std::string userName, machineName;
    GpuInfo gpu; ClientPrefs prefs;
    enum class State { Idle, Downloading, Rendering, Uploading, Inactive } state;
    std::optional<std::string> currentChunkId;
};
```

## 6. Règles métier (critères d'acceptation)

Chaque règle ci-dessous est **vérifiable par un test**. Numéros alignés sur le cahier des charges.

- **RF-10/11** — Le client stocke ses sources connues (IP:PORT + token) dans sa config. Après une déconnexion, il retente avec backoff exponentiel : 1 s, 2 s, 4 s, … plafond 5 min. *Test : couper le serveur, vérifier les délais de reconnexion.*
- **RF-12** — `GPU_INFO` envoyé systématiquement après `HELLO_OK`. Benchmark : au premier lancement, l'UI propose un benchmark (scène de référence, rendue via `RenderProcess`) ; score persisté dans la config locale, joint à `GPU_INFO` si présent.
- **RF-13/14** — `JOB_OFFER` → popup ImGui + `glfwRequestWindowAttention()` si la fenêtre n'est pas focalisée ; sans réponse après 60 s (réglable) → `JOB_DECLINE` implicite. Un refus ne modifie jamais l'état ni les futures offres.
- **RF-20/21/25** — `EligibilityFilter::isEligible(client, job)` retourne `false` si : `job.engine ∈ client.prefs.excludedEngines` ; `client.prefs.availability != AVAILABLE` ; `client.gpu.backend` incompatible avec `job.engine` (EEVEE ignore le backend — pas de filtre backend pour EEVEE) ; `client.prefs.maxFramesPerJob` dépassé par la taille du chunk ; VRAM < seuil configuré par la source. *Test unitaire sur chaque cas.*
- **RF-41** — `BlenderInstaller::ensureVersion(v)` : si `v` absente localement → télécharger l'archive officielle blender.org (URL construite par OS/arch), vérifier le SHA-256 publié, extraire dans le cache, exposer le chemin du binaire. Échec de checksum → suppression et refus d'exécution (RNF-S5).
- **RF-42** — Commande de rendu exacte :
  ```bash
  blender -b <scene.blend> --cycles-device <BACKEND> -s <frameStart> -e <frameEnd> -j <frameStep> -o <outputDir>/<prefix>_#### -F <FORMAT> -x 1 -a
  ```
  `<BACKEND>` = backend choisi dans les paramètres du client (RF-25), jamais imposé par le job. EEVEE : l'option `--cycles-device` est ignorée par Blender, la passer quand même est sans effet. La progression est parsée depuis stdout : lignes `Fra:NNN` → calcul `percent` + `etaSec` → message `PROGRESS` toutes les 5 s minimum.
- **RF-43/44** — Fin de chunk : zipper uniquement les frames rendues du chunk → `RESULT_ZIP` → après `FILE_ACK` final du serveur (`kind=RESULT_ZIP`), suppression locale selon préférence de rétention.
- **RF-50/52** — `JobScheduler` : créer les chunks au publish ; un chunk reste `Assigned` à UN seul client à la fois ; sur déconnexion/timeout/`FILE_ERR` → retour à `Pending`, `attempts++`, réattribution au prochain client éligible ; à `attempts >= 3` → `Failed` + liste explicite à la source. En dernier recours (plus aucun client), la source rend elle-même le chunk. *Tests : client simulé qui meurt en cours de chunk (via blendnet-cli).*
- **RF-53** — `ResultAssembler` : dézipper, vérifier que chaque frame attendue du job existe, non vide, dimensions cohérentes (lire l'en-tête de l'image) ; frames manquantes → nouveau chunk `Pending` ; assembler dans `output/` du projet avec numérotation continue `<prefix>_####`.
- **RNF-03 (invariant)** — un job ne passe à `Done` QUE si 100 % des frames sont valides, OU si l'utilisateur confirme explicitement la finalisation avec une liste d'échecs.

## 7. Machines à états

**Job** : `Draft → Published → Recruiting → Rendering ⇄ Paused → Assembling → Done | Cancelled`. `Assembling → Rendering` si frames manquantes (redistribution).

**Chunk** : `Pending → Assigned → Downloading → Rendering → Uploading → Done` ; toute transition vers `Failed` depuis `Assigned/Downloading/Rendering/Uploading` ; `Failed → Pending` (retentée, max 3).

**ClientSession (vue serveur)** : `Idle ⇄ Downloading ⇄ Rendering ⇄ Uploading ; → Inactive` sur timeout PING ou `CLIENT_KICK`.

**Connexion client** : `Disconnected → Connecting → Authenticating → Ready → (Ready ⇄ Reconnecting)`.

## 8. Sécurité (checklist bloquante)

- [ ] Tout socket = TLS (OpenSSL via Asio) ; aucun socket nu sur le réseau public.
- [ ] Token d'appairage : généré par la source (32+ octets aléatoires, affiché dans l'UI), vérifié au `HELLO` ; échec → `HELLO_ERR` + fermeture. Le token ne JAMAIS apparaître dans les logs.
- [ ] Pinning : au premier appairage réussi, stocker l'empreinte SHA-256 du certificat serveur dans la config ; toute empreinte différente ensuite → refus de connexion + alerte UI.
- [ ] Extraction ZIP : rejeter toute entrée avec chemin absolu ou composant `..` ; extraire uniquement dans le dossier de travail dédié du job.
- [ ] Blender : uniquement blender.org + checksum vérifié avant toute exécution.
- [ ] Limiter la taille des messages : payload > 64 KiB (hors FILE_DATA) → fermeture. `FILE_BEGIN.totalBytes > quota disque configuré` → `FILE_ERR("disk_full")`.

## 9. UI Dear ImGui (spécification des vues)

UI en mode immédiat : la boucle de rendu appelle chaque vue à chaque frame ; les vues lisent des **snapshots d'état** (rafraîchis depuis la file d'événements) et appellent directement `server`/`client` pour les actions. Aucune logique métier côté vues.

| Vue | Contenu minimal |
|---|---|
| `MainView` | Navigation : Projets / Job courant / Clients / Paramètres |
| `ProjectLibraryView` | Liste des projets (nom, chemin, date) + scan de dossiers (RF-01/03) |
| `ProjectDetailView` | Détails (moteur, frames, résolution, format, poids estimé) + paramètres job + bouton **Share Render** (RF-02/04/05) |
| `ClientsTableView` | Sessions : nom, machine, GPU, backend, état, progression + actions déconnecter/annuler chunk (RF-30/31) |
| `JobProgressView` | Progression globale, vitesse, ETA, répartition des chunks (RF-32) |
| `SharePopupView` | Popup acceptation : projet, moteur, frames, taille, Accepter/Refuser, auto-dismiss 60 s (RF-13) |
| `SettingsClientView` | Sources connues (IP:PORT+token), moteurs exclus, **backend de calcul**, disponibilité/plages, limites, dossiers/quota, rétention |
| `SettingsServerView` | Port d'écoute, token d'appairage (régénérable), IP publique détectée |
| `ClientJobsView` | Côté client : chunk courant, progression, annuler ma participation, historique |

i18n : chaînes passées par une fonction de traduction (catalogues FR + EN) ; polices ImGui chargées avec les glyphes UTF-8 complets.

## 10. Tests exigés

- **Unitaires** : `ProtocolCodec` (trames valides, trames tronquées, magic invalide, length mensongère), `JobScheduler` (découpage, réattribution, limite attempts), `EligibilityFilter` (tous les cas §6), `ZipService` (chemins traversants rejetés), `ResultAssembler` (frame manquante).
- **Intégration** (headless, sans GPU ni Blender — `RenderProcess` mocké) : serveur + 3 clients simulés `blendnet-cli` ; un client meurt en cours de chunk → job terminé à 100 % quand même ; coupure réseau en cours de `FILE_DATA` → reprise via `FILE_ACK.nextSeq` sans re-télécharger le fichier entier.
- **Fuzz léger** : payloads JSON malformés sur tous les types de messages → jamais de crash, fermeture propre.

## 11. Ce qui est explicitement HORS périmètre (ne pas implémenter)

Découverte LAN, mode relais, UPnP, rendu vidéo direct, exécution de scripts Python arbitraires reçus du réseau, système de comptes/utilisateurs persistants côté serveur, chiffrement au repos des jobs, assemblage vidéo, UI Widgets.

## 12. Phases de construction

| Phase | Contenu | Done quand |
|---|---|---|
| **P1 — Socle** | CMake (toutes les cibles §3 + tests + cli), `.clang-format`, `core/log`, `core/config`, squelette des dossiers | `cmake --build` passe sur Linux ; cible `blendnet` lance une fenêtre ImGui vide |
| **P2 — Protocole** | `protocol/` : framing §4.1, tous les messages §4.2, tests unitaires + trames invalides | `ProtocolCodec` encode/décode tous les messages ; tests verts sur trames valides et corrompues |
| **P3 — Réseau** | `network/` + `server/RenderServer` + `client/SourceConnection` : TLS, token, pinning, PING/PONG, reconnexion backoff | `blendnet-cli` se connecte à un serveur de test : HELLO→HELLO_OK→GPU_INFO ; mauvais token refusé ; reconnexion après coupure |
| **P4 — Transfert** | `transfer/` : FILE_* complet, SHA-256, reprise, écriture en flux | transfert d'un zip de 1 Go bouclé en local ; coupure à 50 % → reprise ; checksum faux détecté |
| **P5 — Jobs** | `jobs/` + `JobOrchestrator` + `EligibilityFilter` : découpage, attribution, redistribution, états §7 | tests d'intégration §10 (3 clients simulés, mort d'un client) passent |
| **P6 — Blender** | `blender/` : GpuInfoService, BlenderInstaller (checksum), RenderProcess (`--cycles-device`, parse `Fra:`), Benchmark | un chunk rendu en local de bout en bout (job factice 3 frames) ; échec checksum = refus d'exécuter |
| **P7 — Client complet** | `JobExecutor` : zip→extraction sécurisée→rendu→zip résultat→envoi→nettoyage | `blendnet-cli` (non mocké) rend un vrai chunk sur une machine avec Blender |
| **P8 — UI** | Toutes les vues §9 + i18n FR/EN | parcours complet au clavier/souris : ajouter projet → Share Render → voir clients/progression ; côté client : config source → popup → rendu |
| **P9 — Fiabilité & packaging** | Fuzz, timeout keep-alive, quota disque, logs exportables, CPack (.deb/.rpm/AppImage/.exe/.dmg), CI GitHub Actions 3 OS | pipeline CI vert sur Linux+Windows+macOS ; un paquet s'installe et l'application démarre |

## 13. Checklist de suivi (l'agent met à jour ce tableau)

| Phase | État | Notes |
|---|---|---|
| P1 | validée sous Linux | Cibles CMake, config JSON (RF-10/12/24/25), logs (RNF-07), CLI, fenêtre ImGui vide et tests GoogleTest. Compilation complète GUI + CLI réussie via build.sh ; 4/4 tests CTest réussis, dont 11 tests unitaires. Démarrage/arrêt ImGui vérifié sur 3 frames. Windows/macOS restent à vérifier. |
| P2 | à faire | |
| P3 | à faire | |
| P4 | à faire | |
| P5 | à faire | |
| P6 | à faire | |
| P7 | à faire | |
| P8 | à faire | |
| P9 | à faire | |
