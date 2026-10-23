# BlendNet — réseau TLS (P3)

Références : RF-10/11/12/25/30/31, RNF-05/07, RNF-S1/S2/S3.

## Architecture

`core/network/` contient le transport Asio standalone + OpenSSL, la politique de session,
les timers, le backoff et la persistance du pin. `RenderServer` et `SourceConnection`
sont les façades des rôles source/client. La CLI client dépend uniquement de client/core ;
`blendnet-server-cli` est un outil de source headless distinct pour les tests et les démos.

Chaque service possède son `asio::io_context` et un `std::jthread`. Les lectures,
écritures, résolutions DNS, connexions et handshakes sont asynchrones. Toutes les
opérations d'un stream TLS sont sérialisées sur son thread réseau. Les vues futures
consommeront `snapshot()` et `drainEvents()` ; les callbacks réseau ne touchent pas l'UI.

Le logger doit vivre plus longtemps que les services. Les opérations de cycle de vie
`start/stop` appartiennent au thread propriétaire ; `stop` annule les opérations puis
rejoint le thread. La préparation de l'identité et le démarrage sont à effectuer hors
de la boucle de rendu. Les commandes `disconnect` et `updatePreferences` sont postées
sur le thread réseau. Les événements sont bornés à 1 024 entrées (les plus anciennes
sont remplacées si le consommateur tarde) ; le snapshot conserve le dernier état.

## Appairage et sécurité

1. La source crée une identité locale : clé EC P-256, certificat auto-signé valable un an
   et token de 32 octets aléatoires, encodé en 64 caractères hexadécimaux.
   L'identité existante est conservée aux démarrages suivants.
2. Le client établit TLS (minimum 1.2 ; 1.3 est négocié lorsqu'il est disponible).
3. Un certificat déjà connu doit avoir exactement l'empreinte SHA-256 épinglée.
   Au premier contact, le certificat doit être auto-signé avec une signature et des
   dates valides. La confiance repose sur le premier appairage (TOFU), puis le pin,
   sans autorité de certification externe ni vérification du nom DNS.
4. Une fois le certificat validé, le client envoie `HELLO` avec le token.
   La source compare les empreintes SHA-256 des tokens en temps constant.
5. Mauvais token/version : `HELLO_ERR`, puis fermeture. Aucune empreinte n'est persistée
   dans ces cas. Aucun contenu reçu ni token n'est inclus dans les logs réseau.
6. Après `HELLO_OK`, le client enregistre atomiquement le pin dans `settings.json`
   (sources connues). Si cette sauvegarde échoue, il ferme la connexion sans s'inscrire.
7. Le client envoie `GPU_INFO`, puis `CLIENT_PREFS`. La source expose alors sa session
   enregistrée (UUID, utilisateur, machine, GPU, backend et préférences).

Les fichiers clé/token/config créés sont privés (0600 sous Unix). Sous Windows, les
ACL du répertoire utilisateur s'appliquent. Le token n'est pas passé sur la ligne de
commande et son contenu n'est pas affiché par les outils.

Un certificat changé ou expiré arrête la connexion sans modifier le pin ni réessayer
automatiquement. La rotation d'identité et le réappairage sont des actions explicites :
vérifier l'identité de la source, arrêter le client, puis modifier sa configuration.

Le serveur attend strictement `HELLO → GPU_INFO → CLIENT_PREFS`. Les messages hors
séquence et les trames invalides ferment la connexion. Les allocations de payload
passent par `ProtocolCodec::decodeHeader`, qui applique les limites P2. Les écritures
sont sérialisées et bornées à 4 MiB ; le nombre de connexions, y compris non authentifiées,
est borné à 64 par défaut.

## Délais et reconnexion

| Mécanisme | Valeur par défaut |
| --- | --- |
| Handshake TLS + inscription (DNS/connexion inclus côté client) | 10 s |
| Envoi PING (les deux côtés répondent PONG) | toutes les 10 s |
| Absence de PONG | déconnexion après 30 s, vérifiée au tick suivant |
| Reconnexion | 1, 2, 4, 8… secondes, plafond de 5 minutes |

Le backoff est réinitialisé après une inscription réussie. Les coupures, erreurs de
connexion et timeouts déclenchent une reconnexion. Les refus de token/version, pins
différents, certificats rejetés, erreurs de persistance, violations du protocole et
`CLIENT_KICK` sont terminaux. Un arrêt explicite annule les reconnexions en attente.
Les délais sont injectables via `NetworkTimings` pour les tests.

## Démo locale

Compiler avec `./build.sh`, puis lancer la source dans un premier terminal :

```sh
./build/blendnet-server-cli --identity-dir .blendnet/server --port 47050
```

Le programme affiche le port et le **chemin** du token. Copier le contenu de
`.blendnet/server/pairing-token.txt` dans le champ `token` de la configuration client
suivante, enregistrée dans `.blendnet/client/settings.json` :

```json
{
    "knownSources": [{
        "host": "127.0.0.1",
        "port": 47050,
        "token": "REMPLACER_PAR_LE_TOKEN_DE_LA_SOURCE",
        "certificateSha256": null
    }]
}
```

Lancer dans un second terminal :

```sh
./build/blendnet-cli --config .blendnet/client/settings.json --connect 0
```

Le client affiche `READY TLSv1.3` (ou 1.2) et complète la configuration avec le pin.
`--connect 0` sélectionne la première source connue. `--seconds 10` limite la durée ;
Ctrl+C arrête proprement le processus. Arrêter puis relancer la source avec la même
identité permet de voir `RECONNECTING`, puis une nouvelle connexion `READY`.

Par défaut la source de démonstration écoute seulement `127.0.0.1`. Pour l'utiliser
via Internet, choisir `--bind 0.0.0.0`, utiliser l'adresse publique côté client et
configurer la redirection du port TCP sur le routeur. Il n'y a pas de découverte LAN,
de relais ni d'UPnP.

Les caractéristiques GPU de la CLI sont **simulées** en P3 : backend et score viennent
de la configuration, sans lancer Blender. La détection réelle est prévue en P6 et
les commandes graphiques en P8. Le [transfert des fichiers est disponible en P4](transfert-p4.md) ;
les [jobs et clients simulés sont disponibles en P5](jobs-p5.md).

## Tests

`ctest --test-dir build --output-on-failure` couvre le backoff, la persistance de
l'identité/pin, les permissions privées, les événements entre threads et les sessions
TLS locales : mauvais token/version, certificat changé/expiré, pin non enregistrable,
ordre des messages, framing invalide, PING/PONG, timeout de handshake, client silencieux,
source silencieuse, reconnexion après redémarrage, kick et arrêt avec opérations en cours.
Si Python 3 est installé, `blendnet-cli-network` lance également les deux exécutables
et vérifie la connexion, le pin sauvegardé et le refus d'un token invalide.

Références d'implémentation : [Asio SSL](https://think-async.com/Asio/asio-1.28.0/doc/asio/overview/ssl.html),
[OpenSSL X509_digest](https://docs.openssl.org/4.1/man3/X509_digest/).
