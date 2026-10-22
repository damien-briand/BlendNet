# BlendNet — transfert de fichiers (P4)

Références : cahier des charges §5.2, RF-24 (stockage et quota), RNF-02/04/05,
RNF-S1/S2/S3 (transport TLS de P3).

## Fonctionnement

`core/transfer/` fournit l'émetteur, le récepteur et le calcul SHA-256 en flux.
`core/network/TransferLink` les raccorde aux sessions TLS authentifiées de P3.
`RenderServer::sendFile(sessionId, request)` envoie de la source au client ;
`SourceConnection::sendFile(request)` envoie du client à la source.
La progression est disponible dans `snapshot().transfers` et les événements
`TransferProgress` : direction, état, octets acquittés, taille et octets repris.
Le chemin du fichier validé est fourni à la réception.

```text
émetteur                                  récepteur
SHA-256 du fichier
FILE_BEGIN -----------------------------> vérification offre, quota, checkpoint
           <----------------------------- FILE_ACK(nextSeq de reprise)
FILE_DATA(seq), … -----------------------> écriture + SHA-256 en flux
           <----------------------------- FILE_ACK(nextSeq), par lots
FILE_END --------------------------------> vérification SHA-256, promotion du fichier
           <----------------------------- FILE_ACK final
```

Les blocs font au maximum 1 Mio. Leur séquence et leur longueur exacte sont vérifiées,
y compris le dernier bloc partiel. Le récepteur acquitte tous les quatre blocs et le
dernier bloc ; l'émetteur conserve une fenêtre de huit blocs maximum. Le fichier vide
suit `BEGIN → ACK(0) → END → ACK(0)`. L'ACK du dernier bloc et l'ACK final ont le même
compteur : seul l'ACK reçu après `FILE_END` termine l'envoi.

Un pool de deux threads par service traite les lectures, écritures, checkpoints et
hashs. Un exécuteur sérialisé par pair conserve l'ordre entre l'ancienne session et sa
reconnexion. Le thread réseau reste disponible pour TLS et PING/PONG. La lecture TLS
est suspendue si huit messages de transfert attendent leur traitement ; les écritures
TLS restent bornées à 4 Mio. Aucun fichier complet n'est chargé en mémoire.

Une session permet un fichier entrant et un fichier sortant simultanés ; les envois
supplémentaires sont mis en file, avec une limite de seize demandes.

## Reprise et stockage

`TransferOptions.directory` désigne un dossier privé réservé aux fichiers reçus.
Une valeur vide désactive les transferts dans l'API. Les outils CLI choisissent par
défaut le dossier `received` de l'identité serveur, ou `workDirectory` côté client
(relatif au dossier de configuration si nécessaire).

Chaque fichier est identifié sur disque par un SHA-256 du pair et du `fileId` :

| Fichier | Contenu |
| --- | --- |
| `<clé>.part` | Octets reçus en cours de transfert |
| `<clé>.json` | Offre, prochain numéro de bloc et état de validation |
| `<clé>.zip` | Fichier promu après vérification SHA-256 |

Les chemins reçus ne sont jamais utilisés comme chemins locaux. Les fichiers partiels
et checkpoints sont créés privés sous Unix ; les liens symboliques préexistants aux
emplacements des fichiers sont refusés. Le dossier doit rester sous le contrôle du
processus. Un verrou empêche deux récepteurs du même processus d'écrire le même fichier.
Partager ce dossier entre plusieurs processus écrivains n'est pas pris en charge.

Les checkpoints sont remplacés atomiquement après vidage du flux de données, avant
l'ACK. Un arrêt brutal peut laisser une fin non acquittée : elle est tronquée au dernier
checkpoint à la reprise. Le préfixe conservé est relu pour reconstruire le SHA-256.
Cette persistance couvre les redémarrages de processus ; elle ne garantit pas la
durabilité après une panne électrique du stockage.

L'émetteur représente la même offre après reconnexion ; le récepteur répond avec son
`nextSeq`. L'offre doit conserver le même identifiant, type, taille, hash et taille de
bloc. Une offre différente pour un identifiant déjà présent est refusée. Une offre
déjà terminée est revérifiée puis acquittée sans retransmission du contenu ; elle se
termine également par `FILE_END` et son ACK final.

Les demandes d'envoi restent en mémoire pendant les reconnexions. Après redémarrage
du **processus émetteur**, l'appelant doit redemander l'envoi avec le même identifiant
et les mêmes données. Le checkpoint du récepteur reste utilisable. La persistance des
jobs et de leur file d'attente relève des phases suivantes.

L'identité de reprise côté client repose sur le pin du certificat source ; côté source,
elle repose sur le couple utilisateur/machine annoncé dans `HELLO`. Les clients d'une
même source doivent avoir des couples distincts. Ce mécanisme utilise l'authentification
par token de P3, sans système de comptes persistants.

## Quotas et erreurs

`TransferOptions.quotaBytes` borne la taille cumulée du stockage reçu. Les transferts
incomplets réservent leur taille totale, y compris après redémarrage. Les autres fichiers
du dossier comptent également. L'espace disque disponible est vérifié avant acceptation,
en tenant compte des réservations existantes ; les erreurs d'écriture sont aussi traitées.
Le quota local reste indépendant du champ de préférences `quotaGB` annoncé au pair.

| Erreur réseau | Comportement |
| --- | --- |
| `disk_full` | Offre refusée ou écriture arrêtée ; session TLS conservée |
| `sha_mismatch` | Aucun fichier promu ; partiel invalide et réservation supprimés |
| `aborted` | Offre conflictuelle, séquence/taille invalide, annulation ou erreur locale |

Une coupure réseau suspend le transfert et conserve le checkpoint. Un `FILE_ERR`
explicite visant le fichier entrant actif annule ce transfert et supprime son partiel.
Le fichier source doit rester inchangé pendant son envoi.

## Démo locale

Préparer la configuration et le token comme dans la [démo P3](reseau-p3.md).
Pour envoyer un résultat du client vers la source, lancer la source :

```sh
./build/blendnet-server-cli --identity-dir .blendnet/server --port 47050 \
    --transfer-dir .blendnet/source-received
```

Puis le client :

```sh
./build/blendnet-cli --config .blendnet/client/settings.json --connect 0 \
    --send-file /chemin/resultat.zip --file-id demo_RESULT_ZIP --kind RESULT_ZIP \
    --transfer-dir .blendnet/client-received --exit-after-transfer
```

Arrêter puis relancer la source avec la même identité, le même port et le même dossier
pendant l'envoi. Le client affiche `RECONNECTING`, puis reprend le fichier. Les lignes
`TRANSFER SEND|RECEIVE octets taille octetsRepris` indiquent l'avancement ;
`TRANSFER_DONE SEND|RECEIVE` confirme la validation finale.

Pour envoyer un projet dans l'autre sens, ajouter au lancement de la source :

```sh
--send-file /chemin/projet.zip --file-id demo_PROJECT_ZIP --kind PROJECT_ZIP
```

Lancer alors le client sans `--send-file`. Les deux outils acceptent
`--quota-bytes N` pour limiter leur réception. Les octets des archives sont transportés
tels quels : la création et l'extraction sécurisée des ZIP seront intégrées aux jobs
dans les phases suivantes.

## Validation

`ctest --test-dir build --output-on-failure` exécute notamment dix tests unitaires de
transfert, cinq tests d'intégration TLS (bidirectionnel, reprise dans les deux sens,
quota et hash erroné) et une démo automatisée des deux CLI avec un ZIP de 64 Mio dont
le processus récepteur est arrêté puis relancé.

Le critère volumineux P4 utilise une véritable archive ZIP contenant 1 Gio de données,
une coupure à mi-parcours, la reprise et la comparaison SHA-256 des fichiers complets.
Il est optionnel pour les compilations quotidiennes :

```sh
cmake -S . -B build -DBLENDNET_TEST_LARGE_TRANSFERS=ON
cmake --build build --parallel 2
ctest --test-dir build -R '^blendnet-transfer-large$' --output-on-failure
cmake -S . -B build -DBLENDNET_TEST_LARGE_TRANSFERS=OFF
```

Le script construit son archive en flux avec la bibliothèque standard Python ; aucune
dépendance ZIP supplémentaire n'est ajoutée au programme pour P4. Les neuf tests CTest
avec l'archive volumineuse passent sous Linux ; les huit tests habituels passent aussi
en build headless avec AddressSanitizer et UndefinedBehaviorSanitizer. Windows et macOS
restent à vérifier.
