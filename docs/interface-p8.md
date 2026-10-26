# BlendNet — interface Source et Client (P8)

L'application `blendnet` utilise Dear ImGui, GLFW et OpenGL 3.3. La CLI et le cœur
restent utilisables sans affichage. L'interface française et anglaise partage les
mêmes identifiants de widgets et peut être parcourue au clavier (Tab, flèches,
Entrée, Échap) ou à la souris.

```sh
./build.sh
./build/blendnet
# Configuration indépendante pour une seconde instance :
./build/blendnet --config .blendnet/client/settings.json
```

## Parcours Source

1. Dans **Paramètres → Général**, choisir la version exacte de Blender et son cache.
   Une version absente est téléchargée depuis blender.org et vérifiée selon P6.
2. Dans **Paramètres → Serveur**, enregistrer le port puis démarrer la source.
   Le token reste masqué ; son affichage et sa copie demandent une action explicite.
   Le régénérer exige l'arrêt de la source. Communiquer adresse, port et token hors
   application et configurer la redirection TCP sur le routeur.
3. Dans **Projets**, saisir un dossier, cliquer **Ajouter le dossier**, puis sélectionner
   un `.blend`. La bibliothèque conserve nom, chemin et modification ; les dossiers
   et les projets retirés sont persistés. Retirer un projet ne supprime aucun fichier.
4. Blender lit les métadonnées sans auto-exécution des scripts : moteur, plage de
   frames, résolution, format, dépendances et estimation du poids.
5. Ajuster début/fin/pas, moteur, format d'images, taille des chunks et dossier de
   sortie. Un dossier de dépendances supplémentaires permet d'inclure les caches
   propres à une extension que Blender ne déclare pas dans ses chemins.
6. **Share Render** prépare le ZIP sur un worker et publie le job. La vue **Job courant**
   montre les frames validées par le serveur, la vitesse moyenne observée, l'ETA et
   chaque chunk. Pause, reprise, annulation et finalisation avec échecs confirmée
   sont raccordées à l'orchestrateur P5.
7. La vue **Clients** affiche nom, machine, GPU, backend, état et progression. Elle
   permet de déconnecter un client, d'annuler son chunk ou de l'exclure d'un job.
   Le journal d'événements est consultable et exportable.

### Préparation portable du projet

`core/projects/ProjectLibrary` prépare seulement le `.blend`, ses dépendances
détectées et les dossiers supplémentaires explicitement choisis. Les autres fichiers
voisins ne sont pas ajoutés automatiquement. Les textures, bibliothèques liées et
caches déclarés restent à leurs chemins relatifs. Une copie privée de chaque
`.blend` transforme les chemins absolus en chemins relatifs ; les originaux restent
inchangés. Le manifeste P7 sélectionne la scène principale.

Les dépendances doivent exister dans le dossier du projet ou ses sous-dossiers.
Les dépendances externes doivent être regroupées dans ce dossier avant le partage.
Les séquences/UDIM non résolues et les caches d'extensions non déclarés demandent
une préparation explicite du projet. Aucun fichier manquant n'est silencieusement
ignoré. La destination du ZIP doit être hors du dossier du projet. Les limites et
contrôles d'extraction P7 sont aussi appliqués au ZIP avant publication.

## Parcours Client

1. Choisir le rôle **Client**, puis **Paramètres → Client**. Ajouter une source,
   saisir adresse, port et token, puis **Enregistrer** et **Connecter**. Plusieurs
   sources peuvent être enregistrées et connectées. La connexion prépare Blender
   et détecte le matériel sur un worker si nécessaire.
2. La première connexion épingle le certificat. Les écritures de paramètres, de pin
   et de score utilisent une transaction commune dans le processus : une sauvegarde
   graphique ne supprime pas le pin acquis par la connexion.
3. Une offre apparaît dans une popup, avec projet, moteur, frames, poids et délai.
   **Accepter** ou **Entrée** lance l'exécuteur P7 ; **Refuser**, **Plus tard** ou
   **Échap** refuse cette offre. Sans réponse, l'offre expire et est refusée.
   La fenêtre demande l'attention via GLFW lorsqu'elle n'a pas le focus.
4. **Mes rendus** affiche la progression réelle du processus, les transferts et
   l'historique de la session. **Annuler ma participation** interrompt le travail.
   Les résultats sont supprimés après ACK final selon la rétention choisie en P7.

Le premier lancement propose le benchmark optionnel. Le report est mémorisé et le
benchmark reste disponible dans **Paramètres → Général**. Seuls les backends détectés
sont proposés ; CPU reste disponible avant la sonde. Changer les dossiers, le backend,
la version ou les limites d'un exécuteur exige de déconnecter les sources concernées.
Changer le backend ou la version invalide le score du benchmark précédent.

### Paramètres et limites explicites

- Disponibilité, exclusions de moteurs, plafond de frames et quota sont envoyés
  aux sources. Une plage horaire locale peut traverser minuit ; deux heures égales
  signifient une journée entière.
- Le mode arrière-plan applique la limite de threads. Il ne réserve pas le GPU
  et ne plafonne pas son utilisation ou sa VRAM.
- L'option d'activité réagit aux interactions dans **BlendNet**. Elle n'observe pas
  l'activité dans les autres applications ni les sessions Wayland à l'échelle du système.
- L'adresse publique est **renseignée manuellement**. Aucune requête n'est faite
  vers un service de détection d'IP : la stack contractuelle limite les accès HTTP
  à blender.org. La redirection de port reste manuelle, sans UPnP ni relais.
- L'historique graphique et le journal affiché sont bornés pour la session.
  Le journal disque habituel reste dans `logs/blendnet.log` près de la configuration.
- Les polices Unicode du système sont utilisées (Noto Sans/DejaVu sous Linux,
  Segoe UI sous Windows, Arial Unicode sous macOS). La distribution des polices
  dans les paquets reste du ressort de P9.

## Architecture et vérification

`core/application/DesktopController` possède les connexions, le serveur, les offres
avec leurs délais et les snapshots. Son acteur échange avec les services réseau ;
un worker séparé scanne, installe, sonde, inspecte et compresse. L'interface ne lance
aucun processus ni transfert dans sa boucle de dessin. `JobExecutor` expose sa
progression avec verrou pour la vue Client. Les vues ne contiennent aucune décision
d'ordonnancement ou de validation d'image.

`src/ui/I18n` contient les catalogues FR/EN. `Views` dessine les neuf vues du guide.
`Automation` est un pilote de test : il injecte les événements clavier/souris normaux
d'ImGui et ne déclenche aucune action du contrôleur directement. Il ne s'active
qu'avec `--ui-test` ; `--screenshot` produit une capture PPM.

La suite quotidienne ajoute les tests de catalogues, paramètres persistants,
transactions concurrentes, scan, horaires, préparation du projet et contrôleur
réseau. Les tests du contrôleur vérifient expiration sans exécution, refus sans
déconnexion puis acceptation d'un autre job, rendu par le processus enfant de test,
validation et conservation du pin après sauvegarde.

Le test graphique avec Blender réel est optionnel et nécessite un affichage :

```sh
python3 tests/integration/gui_flow.py ./build/blendnet ./build/blendnet-cli \
    4.2.1 "$PWD/.cache/blender" "$PWD/.cache/p8/gui"
```

Il ouvre deux instances, ajoute le dossier par les widgets, sélectionne le projet,
configure la source par saisie clavier, accepte la popup avec Entrée et vérifie les
trois PNG 32×32 de deux chunks réels. Il contrôle aussi le pin, FR/EN, la rétention
et les captures. Les scripts de saisie temporaires contenant le token de test sont
supprimés à la fin. Pour isoler les fenêtres sous X11, un affichage Xephyr/Xvfb peut
être fourni via `DISPLAY` ; `LIBGL_ALWAYS_SOFTWARE=1` permet le rendu logiciel Mesa.

Le test de démarrage sur trois frames reste disponible :

```sh
./build/blendnet --config .cache/p8/smoke/settings.json --smoke-test \
    --screenshot .cache/p8/smoke.ppm
```

Les validations GPU physiques, Windows et macOS restent à effectuer. La vérification
ASan/UBSan utilise la suppression optionnelle et ciblée de libp11-kit documentée en
[P6](blender-p6.md), sans désactiver les autres contrôles de fuite.

Validation Linux CPU : **11/11 tests CTest réussis** en compilation complète
CMake/Make et en headless ASan/UBSan avec la suppression système ci-dessus.
La suite couvre 88 tests unitaires, 12 réseau, 6 transfert, 4 jobs et 4 scénarios
du contrôleur graphique. Les fenêtres Source et Client ont aussi exécuté le
parcours réel avec Blender officiel 4.2.1 : deux chunks, 3/3 PNG 32×32 vérifiés,
acceptation avec Entrée, passage FR/EN, pin sauvegardé et nettoyage après ACK.
