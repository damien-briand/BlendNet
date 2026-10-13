# BlendNet

Tu va m'aider a organiser mes idées et structurer plus en détail mon projet.
Tu fera a la fin un cahier des charges pour que je puisse m'y référer pendant le développement de l'application.
J'ai pour projet de créé une application qui va m'aider a accéléré mes rendus blender. Le but est de partagé des fichiers blender entre plusieurs PC et de pouvoir faire des rendus partager. 

Cette application va étre développer en C++, l'interface avec Qt. L'application devra avoir une version Linux, Windows et Mac.

L'utilisateur qui emmet la demander de partage de rendu sera appelé la "source" et sera un client-serveur et les utilisateurs qui acceptent de partager leurs GPU pour faire le rendu seront appelés les "clients".

Quand la source veux faire un rendu, il va sur l'application et a la liste de tout les projets blender qu'il a, il peut cliqué sur le projet qu'il veut rendre pour avoir les informations du projet, comme le moteur de rendue choisie (EEVEE, Cycle), le nombre de frame et autre. 
Il va pouvoir ensuite appuiyer sur le bouton share render. Quand le bouton share render va étre trigger, tout les utilisateurs qui ont l'application vont recevoire un petit pop up sur leurs écran pour leurs demander si il veut partager leurs GPU pour faire un rendu. Et les utilisateurs qui acceptent vont recevoir le projet blender et vont pouvoir faire le rendu sur leurs GPU.

L'utilisateur source qui demande le rendu est le "serveur" et les utilisateurs qui acceptent de partager leurs GPU sont les "clients". Pour connecter les clients au serveur, le serveur devra leurs envoyer l'adresse IP et le port sur lequel il est en écoute. Les clients vont ensuite se connecter au serveur pour recevoir les fichiers nécessaires au rendu. Les transferts de fichiers se feront via le protocole TCP/IP et devraient étre sécurisé.

Le serveur pourra voir tout les clients connectés et pourra géré les connexions, comme déconnecter un client ou voir l'état des clients (en cours de rendu, en attente, etc.). Lors de la premiére connexion d'un client, le client va envoyer les caractéristiques de son GPU au serveur pour que le serveur puisse savoir si le client est capable de faire le rendu demandé.

Le client a la possibilité de paramétrer le logiciel pour informer le serveur de ceux qu'il peux faire ou pas, par exemple si le client ne veux pas faire de rendu avec EEVEE, il pourra le spécifier dans les paramètres. Ou si le client ne peux pas faire de rendu car son GPU est deja utilisé, en jeu ou car il travaille, il pourra le spécifier dans les paramètres. Le serveur va donc prendre en compte ces informations pour savoir si le client est capable de faire le rendu demandé.

Lors d'un rendu, le serveur va diviser le projet en plusieurs parties (frames) et les envoyer aux clients pour qu'ils puissent les rendre. Il faudra aussi que les clients si il n'ont pas blender le téléchargent automatiquement pour pouvoir faire le rendu. Le serveur va donc envoyer les fichiers nécessaires au rendu aux clients, comme le fichier .blend et les textures utilisées dans le projet, dans un dossier zipé. Le client va donc recevoir le fichier zip, le dézipper et lancer le rendu sur son GPU.

Le client va télécharger ou pas la bonne version de blender. A la fin du rendu, le client va envoyer le résultat du rendu au serveur, donc un dossier d'image compréssé en zip. Le serveur va donc recevoir le dossier zip et le dézipper pour récupérer les images rendues. 