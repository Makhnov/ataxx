# Rendu BlobWar

Ce dossier contient notre rendu pour le projet BlobWar : le rapport, les
stratégies et les scripts qui permettent de lancer des matchs localement.

## Organisation du dossier

- `rapport.pdf` : rapport du projet.
- `strategies/` : code des différents algorithmes décrits dans le rapport.
- `MM0/` : client utilisé pour lancer une stratégie contre le serveur.
  Le fichier actif du client est `MM0/strategy/strategy.cc`.
- `server/` : serveur BlobWar local.
- `scripts/` : scripts de lancement et d'agrégation des résultats.
- `tablebase/` : tables utilisées par `strategies/NMP_TT_SF_v7.cc`.
- `logs/` : logs produits par les matchs.

Les cartes doivent être présentes dans `data/boards/`. Les cartes utilisées
dans nos tests sont principalement `standard` et `x`.

## Algorithmes dans `strategies/`

- `strategy_glouton.cc` : algorithme glouton.
- `strategy_minimax.cc` : min-max séquentiel.
- `strategy_AB_ID.cc` : alpha-beta avec iterative deepening.
- `strategy_minimax_par.cc` : min-max parallèle CPU.
- `strategy_AB_ultrapar.cc` : alpha-beta parallèle CPU (branche parallèle de AB_ID).
- `NMP_TT_v2.cc` : negamax avec table de transposition (branche MM0 de ultrapar, version stateless).
- `NMP_TT_SF_v1.cc` : première version negamax optimisée (branche stateful de NMP_TT_v2).
- `NMP_TT_SF_v2.cc` : optimisations autour de la recherche (branche de la v1).
- `NMP_TT_SF_v3.cc` : version orientée puissance brute (branche de la v1).
- `NMP_TT_SF_v4.cc` : variante expérimentale (branche de la v1).
- `NMP_TT_SF_v5.cc` : optimisation de l'ordonnancement des coups (branche depuis la v3).
- `NMP_TT_SF_v7.cc` : version avec ordre de coups appris depuis les matchs précédents (branche de la v5).

## Exécuter une stratégie

Le script de match lance des exécutables, pas directement les fichiers `.cc`.
Pour tester une stratégie compatible avec le client `MM0`, on la place donc
dans `MM0/strategy/strategy.cc`, on compile, puis on garde le binaire avec le
nom de la stratégie.

Exemple avec `NMP_TT_SF_v3.cc` :

```bash
cp strategies/NMP_TT_SF_v3.cc MM0/strategy/strategy.cc
make -C server
make -C MM0 legacy
cp MM0/mm0 MM0/NMP_TT_SF_v3
```

Pour rejoindre une partie "legacy" sur un server lancé (par exemple localhost, port 1234) il suffit de lancer l'exécutable avec 
les bons arguments 

```bash
./MM0/NMP_TT_SF_v3 SERVER=localhost PORT=1234
```

Optionnel : Si besoin (si l'adversaire n'a pas non plus d'interface graphique par exemple, on peut choisir la map) :

```bash
./MM0/NMP_TT_SF_v3 SERVER=localhost PORT=1234 MAP=standard
```

On peut aussi faire s'affronter nos algos, par exemple sur la map `standard` :

```bash
./scripts/blobwar.sh NMP_TT_SF_v3 NMP_TT_SF_v3 standard 1
```

Le dernier argument indique le nombre de parties par couleur. Ici, `1` lance
deux parties au total : une avec le premier bot en rouge, puis une avec les
couleurs inversées.

## Comparer deux stratégies

Exemple avec `NMP_TT_SF_v3.cc` contre `NMP_TT_SF_v7.cc` :

```bash
cp strategies/NMP_TT_SF_v3.cc MM0/strategy/strategy.cc
make -C MM0 legacy
cp MM0/mm0 MM0/NMP_TT_SF_v3

cp strategies/NMP_TT_SF_v7.cc MM0/strategy/strategy.cc
make -C MM0 legacy
cp MM0/mm0 MM0/NMP_TT_SF_v7

./scripts/blobwar.sh NMP_TT_SF_v3 NMP_TT_SF_v7 standard 5
```

Cette commande lance 10 parties au total : 5 dans un sens, puis 5 avec les
couleurs inversées.

Pour utiliser la carte `x`, il suffit de remplacer `standard` par `x` :

```bash
./scripts/blobwar.sh NMP_TT_SF_v3 NMP_TT_SF_v7 x 5
```

## Comparer deux stratégies sur plusieurs PCs

Une fois passé l'étape de la paralélisation et quand notre alog de test n'était plus `NMP_TT_v2` il n'était plus possible d'utiliser le script blobwar.sh pour faire s'affronter deux stratégies nécessitant chacune 12 coeurs (enfin c'est possible mais pas pertinent). Pour cela il faut utiliser le script serveur `/scripts/blobwar_remote_server.sh` et le script client `/scripts/blobwar_remote_client.sh` qui permettent de faire s'affronter deux stratégies sur des machines différentes. Leur fonctionnement est assez simple, une fois le serveur lancé sur une machine on reçoit dans le terminal la commande à copier sur les terminaux clients. 

Exemple : 

Sur le serveur : 

```bash
makh@PC-Fixe:~/BLOB$   BLOBWAR_PORT=2002 ./scripts/blobwar_remote_server.sh NMP_TT_SF_v1 NMP_TT_SF_v4 standard 10
```

Commandes reçues à copier sur les terminaux clients : 

```bash
# Player A machine:
./scripts/blobwar_remote_player.sh SERVER=172.23.183.104 PORT=2002 ALGO=NMP_TT_SF_v1 OPP=NMP_TT_SF_v4 PAIR=A MAP=standard GAMES_PER_SIDE=10 RUN_ID=20260502_194444_NMP_TT_SF_v1__vs__NMP_TT_SF_v4_standard_10x2_remote

# Player B machine:
./scripts/blobwar_remote_player.sh SERVER=172.23.183.104 PORT=2002 ALGO=NMP_TT_SF_v4 OPP=NMP_TT_SF_v1 PAIR=B MAP=standard GAMES_PER_SIDE=10 RUN_ID=20260502_194444_NMP_TT_SF_v1__vs__NMP_TT_SF_v4_standard_10x2_remote
```

## Résultats

Les résultats des matchs sont endregistrés dans `/logs/tournament/runs/[MATCH_ID]/[PARTIE_ID]`. Le script `scripts/update.sh` permet de les agréager et de les extraire au format json et js directement à la racine du réperoirre `logs/`.

Les résultats détaillés de nos benchmarks sont disponibles sur notre site :

https://makh.fr/blobwar/
