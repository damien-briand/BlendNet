# BlendNet — protocole v1 (P2)

Références : cahier des charges §5.2, RF-10/12/13/25/42/50/54, RNF-04 et RNF-S2.
Le code de référence est `src/core/protocol/`. Ce module est headless et sans socket.
La [couche TLS et la politique de session](reseau-p3.md) sont implémentées en P3 ;
les [transferts avec reprise](transfert-p4.md) sont implémentés en P4.
L'[orchestration des jobs](jobs-p5.md) est implémentée en P5.

## En-tête

Tous les entiers binaires sont non signés et encodés **big-endian**, sans padding.

| Offset | Taille | Champ | Valeur |
| --- | --- | --- | --- |
| 0 | 2 | magic | `0x5253` (`RS`) |
| 2 | 1 | reserved | `0` |
| 3 | 1 | msgType | ID du message |
| 4 | 4 | flags | `0` |
| 8 | 4 | length | Taille exacte du payload, hors en-tête |

L'en-tête fait **12 octets**. L'ancienne mention de 14 octets dans le guide de développement
était une erreur de calcul ; aucun champ supplémentaire n'est ajouté.

Les 22 types et leurs schémas JSON sont définis au §4.2/4.3 du
[guide de développement](blendnet-spécification-agent-ia-guide-developpement.md).
Les structures C++ de `Messages.h` reprennent ces champs ; `StatusUpdate.preferences`
est sérialisé directement comme un `CLIENT_PREFS`, sans objet JSON englobant.

## JSON de contrôle

- Objet JSON UTF-8, champs requis et sans champ supplémentaire en version 1.
- Les champs optionnels sont présents et contiennent soit une valeur, soit `null`.
- Les clés dupliquées, l'UTF-8 invalide, les documents concaténés et une profondeur
  supérieure à 64 sont rejetés. L'ordre des clés et les espaces ne sont pas significatifs.
- Taille maximale du payload : **65 536 octets**, vérifiée à la lecture de l'en-tête.
- Les chaînes sont non vides et sans NUL. Les enums textuelles sont celles des schémas.
- Les compteurs `vramMB`, `ramMB`, `benchmarkScore`, `framesDone`, `etaSec` sont positifs
  ou nuls ; les limites `maxFramesPerJob`, `maxConcurrentJobs`, `quotaGB` sont strictement
  positives lorsqu'elles sont renseignées.
- Les frames sont des entiers signés 32 bits (frames négatives autorisées),
  `frameEnd >= frameStart`, `frameStep >= 1`, `framesTotal >= 1`,
  `0 <= framesDone <= framesTotal` et `percent` est fini entre 0 et 100 inclus.
- `zipBytes` et `totalBytes` sont des entiers non signés 64 bits ; `nextSeq` est un u32.
  Un booléen ou un nombre décimal n'est jamais accepté comme entier.
- Les SHA-256 sont des chaînes de 64 chiffres hexadécimaux.
- `protocolVersion` est un u32 strictement positif (version courante : **1**).
  Une autre version reste décodable : la négociation P3 répondra `HELLO_ERR/bad_version`.

## FILE_DATA

Le payload est binaire :

```text
[u32 fileIdLen][fileIdLen octets UTF-8 fileId][u32 seq][u32 dataLen][dataLen octets]
```

`fileId` est le même identifiant textuel que dans `FILE_BEGIN/END/ACK/ERR`, par exemple
`jobId_PROJECT_ZIP`. L'ancien champ `u32 fileId` était incompatible avec ce contrat.
Pour des résultats successifs, P5 utilise `chunkIdDeTentative_RESULT_ZIP` : chaque
attribution porte un identifiant distinct, afin de conserver des offres de fichiers
immuables et de rejeter les résultats tardifs d'une attribution précédente.

- `fileIdLen` : 1 à **256 octets** ; UTF-8 valide, sans NUL.
- `seq` : u32, à partir de 0 (la cohérence de séquence relève du transfert P4).
- `dataLen` : 1 à **1 048 576 octets** (1 MiB).
- Taille exacte du payload : `12 + fileIdLen + dataLen` ; maximum **1 048 844 octets**.
- `FILE_BEGIN.chunkSize` est compris entre 1 et 1 MiB. P4 vérifie aussi que chaque
  bloc respecte le `chunkSize` annoncé, le quota disque et l'état du transfert.
- Un fichier de taille nulle n'envoie aucun bloc.

## Utilisation du codec

`ProtocolCodec::encode(Message)` valide le message et retourne les octets d'une trame,
ou une erreur typée sans payload. Tous les types partagent le même chemin de validation
en émission et réception.

Pour un transport asynchrone :

1. Lire les 12 octets de l'en-tête.
2. Appeler `decodeHeader` **avant d'allouer le payload**. Rejeter les tailles excessives,
   flags non supportés, octets réservés non nuls, types inconnus et magic invalide.
3. Lire exactement `payloadBytes` octets, puis appeler `decodePayload`.
4. En cas d'erreur fatale, journaliser uniquement le code/texte de l'erreur et fermer
   la connexion TLS. Le codec ne journalise jamais les données reçues ni les tokens.

`decode(span)` traite la première trame d'un tampon ; `DecodedMessage.bytesConsumed`
permet de conserver le suffixe et de traiter plusieurs trames concaténées.
`decodePayload` exige au contraire une taille exacte (un surplus donne `TrailingData`).

`NeedMoreData` signifie que le tampon est incomplet : conserver les octets et attendre
la suite. Un EOF ou timeout avant complétude sera traité comme une coupure par P3.
Les trames invalides ne donnent jamais de message partiellement décodé.

Le codec vérifie la forme des messages. Il ne décide ni de l'authentification, ni de
l'ordre des messages, ni de l'éligibilité d'un client, ni des droits sur les chemins de
sortie ; ces contrôles restent dans les couches de session, transfert et métier.
