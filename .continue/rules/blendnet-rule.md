---
description: Cadrage et conventions du projet RenderShare (rendu Blender distribué) — à appliquer systématiquement
globs:
alwaysApply: true
---

# BlendNet — règles du projet

Projet : application de bureau C++ (Dear ImGui) de **rendu Blender distribué entre pairs via Internet**.
Le document de référence est le **cahier des charges**. Les exigences y sont numérotées **RF-xx** (fonctionnelles) et **RNF-xx** (non fonctionnelles) — cite l'identifiant concerné quand tu implémentes ou modifies une fonctionnalité.

## Terminologie (ne pas confondre)

- **Source** : l'utilisateur qui demande un rendu. Il joue le rôle de **serveur** (écoute TCP/TLS, distribue les tâches, collecte les résultats) ET d'interface Qt.
- **Client** : l'utilisateur qui prête son GPU pour rendre une partie du job.
- **Job** : un rendu demandé (projet .blend + paramètres). **Chunk** : lot de frames attribué à un client. **Frame** : une image.
- Un seul et même binaire joue les deux rôles.

## Stack — ne pas y déroger

- **C++20**, **CMake**.
- **Qt interdit** : l'interface est en **Dear ImGui + GLFW + OpenGL 3.3**. Tout autre framework UI est interdit.
- Réseau : **Asio** (standalone ou Boost) + OpenSSL. **Interdit : Qt, toute lib réseau autre qu'Asio/OpenSSL.**
- Chiffrement : **OpenSSL natif** (TLS 1.3 recommandé, 1.2 minimum).
- Sérialisation : **JSON** (nlohmann/json uniquement).
- ZIP : **libzip** pour le stockage local ; **zstd en option** pour le transport réseau.
- Téléchargements : **libcurl** (uniquement pour blender.org).
- Process externes : **reproc** (sous-processus multiplateforme).
- Threads : `std::jthread` + **files de messages thread-safe** entre réseau/rendus et l'UI ; pool de threads pour SHA-256 et zip.
- Tests : **Catch2 ou GoogleTest**, CI GitHub Actions (Linux, Windows, macOS).

## Architecture — dépendances strictement descendantes

    ui/ (Dear ImGui) → server/ + client/ (rôles) → core/ (headless)

- **`src/core/`** : bibliothèque **headless, aucune dépendance à l'UI**. Sous-modules : `protocol/`, `network/`, `transfer/`, `jobs/`, `blender/`, `archive/`, `config/`, `model/`, `log/`.
- **`src/server/`** : `RenderServer`, `ClientSession`, `JobOrchestrator`, `EligibilityFilter`, `ResultAssembler`.
- **`src/client/`** : `SourceConnection`, `JobExecutor`, `RenderProcess`, `BlenderInstaller`.
- **`src/ui/`** : initialisation ImGui/GLFW (thème, polices UTF-8) + vues en mode immédiat. **Les vues ne contiennent AUCUNE logique métier** : elles lisent des snapshots d'état (rafraîchis depuis la file d'événements) et traduisent les interactions utilisateur en appels vers server/client.
- Toute logique métier doit être accessible **sans interface graphique** (tests automatisés, `tools/blendnet-cli`).

## Règles C++

- Style cohérent sur tout le dépôt (`.clang-format` à la racine) ; nommage : classes en `PascalCase`, membres `m_camelCase`, callbacks/événements nommés `onQuelqueChose`.
- **Aucun appel bloquant dans le thread UI** (RNF-05) : le réseau vit sur un thread Asio dédié et communique via files de messages thread-safe consommées à chaque frame ImGui ; rendus via `reproc` ; calculs lourds (SHA-256, zip) via un pool de threads.
- Écriture disque en **flux** pour les gros fichiers (zip de plusieurs Go jamais chargés entièrement en mémoire).
- Erreurs : codes d'erreur typés (enum + `std::optional`/`expected`), jamais de crash silencieux ; tout événement important est journalisé (RNF-07).
- Toute nouvelle classe va dans le bon module selon la frontière ci-dessus ; le core **ne dépend jamais** de server/client/ui.

## Réseau et protocole

- Toutes les communications en **TCP/IP sécurisé TLS** (RNF-S1) — jamais de socket en clair.
- Authentification par **token d'appairage** saisi par le client avec l'IP:PORT (RNF-S2) ; **pinning du certificat serveur** au premier appairage (RNF-S3).
- Messages typés : `HELLO`, `GPU_INFO`, `CLIENT_PREFS`, `PING/PONG` (10 s / timeout 30 s), `JOB_OFFER`, `JOB_ACCEPT/JOB_DECLINE`, `JOB_FILE`, `TASK_ASSIGN`, `PROGRESS`, `RESULT_FILE`, `TASK_CANCEL`, `JOB_PAUSE`, `CLIENT_KICK`.
- Fichiers volumineux : transfert **par blocs** avec **SHA-256**, **reprise sur interruption** et progression (RNF-04).
- Le protocole est versionné (`HELLO` porte la version) ; tout encodage/décodage passe par `ProtocolCodec` uniquement.

## Sécurité — non négociable

- **RNF-S4** : extraction ZIP dans un dossier de travail dédié, validation systématique des chemins (rejeter `../` et les chemins absolus).
- **RNF-S5** : Blender téléchargé **uniquement depuis blender.org**, **checksum vérifié avant exécution**. Aucune exécution de code arbitraire reçu du réseau.
- **RNF-S6** : le client garde toujours la priorité sur sa machine (limites de ressources respectées, `CLIENT_KICK`/annulation honorés immédiatement).

## Blender — règles d'exécution

- Blender est lancé **uniquement en ligne de commande, headless** (`blender -b`), jamais en interface graphique.
- Le **backend de calcul est choisi par le client dans ses paramètres** (OptiX, CUDA, HIP, oneAPI, Metal ou CPU — RF-25) et appliqué via **`--cycles-device <BACKEND>`** (ex. `--cycles-device METAL`).
- Le backend choisi est déclaré au serveur dans `GPU_INFO` (RF-12). Le job **n'impose pas** de backend ; le serveur peut filtrer par backend (RF-55).
- La **version exacte de Blender est fixée par le job** : si absente ou différente chez le client, `BlenderInstaller` la télécharge et la vérifie (RF-41).
- Sortie : **séquences d'images uniquement** (PNG/JPEG/EXR), jamais de vidéo directe.

## Comportement fonctionnel clé (rappels)

- Toute déconnexion ou échec d'un client entraîne la **redistribution automatique** de ses frames non rendues (RF-52) ; aucune frame perdue (RNF-03).
- Le filtrage d'éligibilité (RF-51) prend en compte : refus, moteurs exclus, backend, VRAM insuffisante, indisponibilité, absence de réponse au popup.
- Un refus de job n'entraîne **jamais** de pénalité (RF-14).
- Benchmark GPU : **optionnel**, proposé au premier lancement, score stocké dans la config locale du client (RF-12).

## Tests

- Chaque module du core est testable sans GPU ni Blender (mock de `RenderProcess`, clients simulés via `rendershare-cli`).
- Toujours prévoir : test unitaire pour `ProtocolCodec` (trames valides **et malformées**), chunks/redistribution, éligibilité, extraction ZIP sécurisée.
- Les tests réseau incluent les cas dégradés : coupure en cours de transfert, reprise, timeout keep-alive.

## Interdits (récapitulatif)

- Qt (toute la bibliothèque), tout GUI autre que Dear ImGui.
- Socket non chiffrée, token en clair dans les logs, exécution d'un Blender non vérifié.
- Logique métier dans les vues ImGui ou dépendance montante du core.
- Rendu vidéo direct, découverte LAN (le périmètre est **Internet uniquement**, connexion manuelle IP:PORT + token).