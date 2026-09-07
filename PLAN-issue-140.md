# Plan — Issue #140 : exposition automatique HDR

Issue : https://github.com/gkehren/ft_vox/issues/140

Branche : `fix/140-auto-exposure`

> **Statut : implémenté (rebasé sur #150/#151).** Mesure GPU = réduction log-luminance
> bornée en 4 passes graphiques (pas de compute) + UN historique GPU partagé (l'état
> temporel est une valeur logique unique) + snapshots CPU par frame-in-flight pour le
> diagnostic, lus après fence wait uniquement. Coût mesuré : +0,088 ms GPU au total
> (sous-passe Exposure 0,120 ms, RTX 4070 Ti). Zéro erreur de validation, 30/30 tests
> verts dont 2 nouvelles scènes de référence auto et un banc d'adaptation temporelle
> (alternance de slots FIF, no-op dt=0, indépendance au découpage des frames). Détails :
> `docs/vulkan-graphics.md` §Post, `docs/visual-regression.md` §scènes. Les validations
> subjectives (vidéos de transition, scène immobile plusieurs minutes) restent à faire en jeu.

## Objectif

Implémenter une exposition automatique fondée sur la luminance HDR, avec une adaptation temporelle stable et un mode manuel déterministe. Aucun calcul d’exposition ne doit dépendre d’une lecture GPU vers CPU ni ajouter une attente par frame.

## 1. Vérifier les prérequis

- [ ] Confirmer que la correction du pipeline linéaire de l’issue #135 est présente.
- [ ] Relire `docs/vulkan-graphics.md`, `docs/engine-architecture.md` et `docs/visual-regression.md`.
- [ ] Identifier le passage HDR → composite dans `PostStack`, les paramètres de `EngineDefs.hpp` et les contrôles de `GameUI`.
- [ ] Relever le coût GPU du post-traitement avant modification, sur une configuration reproductible.

## 2. Définir les paramètres et leur sémantique

- [ ] Conserver `exposure` comme valeur exacte du mode manuel.
- [ ] Exprimer `exposureCompensation` en stops EV : 0 est neutre, +1 double l’exposition cible.
- [ ] Définir une valeur de gris moyen, des limites d’exposition et deux vitesses d’adaptation en secondes inverses.
- [ ] Valider les valeurs limites et réinitialiser correctement ces paramètres avec les préréglages.
- [ ] Définir le comportement au démarrage, au redimensionnement et lors d’un changement de mode : réactiver l’automatisme depuis la dernière exposition manuelle, sans adaptation cachée.

## 3. Mesurer la luminance sur GPU

- [ ] Mesurer la scène HDR après le rendu du monde et avant le tone mapping, sans inclure le bloom ajouté au composite.
- [ ] Choisir une réduction GPU bornée : histogramme avec exclusion des extrêmes, ou moyenne logarithmique écrêtée.
- [ ] Documenter l’échantillonnage, les bornes et la politique de rejet des pixels aberrants.
- [ ] Vérifier qu’un petit nombre de pixels de soleil ou de lave ne domine pas la mesure.
- [ ] Si un compute shader est retenu, vérifier les capacités de la famille de queues utilisée et les limites matérielles.

## 4. Calculer et adapter l’exposition

- [ ] Calculer la cible à partir du gris moyen, de la luminance mesurée et de la compensation EV, puis appliquer les limites.
- [ ] Conserver l’exposition précédente dans une petite ressource GPU.
- [ ] Utiliser une adaptation exponentielle dépendant du temps écoulé : `alpha = 1 - exp(-vitesse * dt)`.
- [ ] Utiliser des vitesses distinctes pour éclaircir une scène sombre et réduire l’exposition face à une scène claire.
- [ ] Traiter les premières frames, les pauses et les changements de mode sans lecture d’une ressource non initialisée.

## 5. Intégrer au rendu Vulkan

- [ ] Ajouter le shader et sa compilation SPIR-V dans CMake.
- [ ] Gérer la création, la destruction et la recréation des ressources et pipelines.
- [ ] Définir les barrières entre écriture HDR, mesure, adaptation et lecture par le composite, y compris entre frames en vol.
- [ ] Préparer les descripteurs sans mise à jour pendant l’enregistrement des commandes.
- [ ] Consommer l’exposition GPU dans tous les chemins du composite, notamment avec FXAA.
- [ ] En mode manuel, utiliser directement `exposure` et suspendre l’adaptation.

## 6. Ajouter les contrôles et le diagnostic

- [ ] Ajouter dans l’interface graphique : activation, exposition manuelle, compensation EV, limites et vitesses.
- [ ] Permettre d’inspecter la luminance mesurée, l’exposition actuelle, la cible et l’état des limites.
- [ ] Pour les diagnostics CPU, prévoir uniquement des captures optionnelles et asynchrones, exploitées après une synchronisation existante ; ne pas introduire de lecture bloquante par frame.
- [ ] Ajouter une mesure GPU dédiée à l’exposition et conserver la mesure globale du post-traitement.

## 7. Vérifier le comportement automatiquement

- [ ] Tester des luminances constantes, les limites, les valeurs extrêmes et la compensation EV.
- [ ] Vérifier la convergence, les vitesses asymétriques et l’indépendance au nombre de frames pour une même durée écoulée.
- [ ] Tester les transitions manuel → automatique → manuel, le démarrage et le redimensionnement.
- [ ] Vérifier le rejet d’une faible proportion de pixels très brillants sur des entrées HDR synthétiques.
- [ ] Exercer le shader réel dans des tests GPU ; réserver les lectures synchrones éventuelles au banc de test.
- [ ] Fixer explicitement le mode manuel dans les scènes de référence existantes et ajouter une validation distincte de l’adaptation temporelle.
- [ ] Ne pas régénérer les images de référence pour masquer une régression.

## 8. Valider visuellement et mesurer les performances

- [ ] Enregistrer des courbes d’exposition et des vidéos pour : extérieur de midi → grotte, grotte → extérieur, rotation vers le soleil, coucher de soleil → nuit, proximité de lave, entrée/sortie d’eau et déplacement sous la canopée.
- [ ] Observer une scène immobile pendant plusieurs minutes pour détecter dérive ou scintillement.
- [ ] Vérifier la lisibilité des ombres et des hautes lumières sans constantes arbitraires propres aux grottes, à la nuit ou à l’eau.
- [ ] Mesurer le coût GPU de l’exposition et du post-traitement total, puis comparer à la référence initiale.
- [ ] Rapporter la mémoire ajoutée, la résolution, le GPU et les paramètres des mesures.
- [ ] Compiler en Release, exécuter les tests pertinents et vérifier les couches de validation Vulkan.
- [ ] Confirmer qu’aucune attente CPU/GPU ni lecture synchrone n’a été ajoutée au chemin d’exécution par frame.

## 9. Documenter et livrer

- [ ] Documenter l’algorithme, les réglages, les transitions de mode et le cycle de vie des ressources.
- [ ] Reporter les résultats mesurés et les éventuelles limites de validation.
- [ ] Vérifier chaque critère d’acceptation de l’issue #140 avant de considérer la correction terminée.

