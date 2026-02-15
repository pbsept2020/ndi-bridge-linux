# Prompt : Mesure de latence test pattern EC2 → Mac (observation seule)

## Contexte — Lis d'abord

1. **Lis `~/Projets/CLAUDE.md` section "QUI EST PIERRE"** — c'est nouveau et ça oriente tout.
2. **Lis `~/Projets/skills/clockmaster/SKILL.md` section 0 "DEUX CULTURES"** — ça t'évitera les erreurs habituelles (30fps, "pas grave", etc.)

## Ma direction

Je ne travaille pas juste sur un bridge NDI. Le LTC dans le test pattern n'est pas "juste un repère visuel" — c'est le premier maillon d'une chaîne où le timecode broadcast devient un canal de feedback pour du smoothing réseau (SmoothBond). La qualité de ce timecode, sa précision par rapport à UTC, et la latence mesurable de bout en bout sont des données architecturales, pas du debug.

## Ce que je veux — observation uniquement

Je veux **quantifier la latence end-to-end** entre le test pattern généré sur EC2 et ce que je vois sur mon moniteur NDI Mac. Pas de modification de code. Pas de refactoring. Juste mesurer avec ce qu'on a.

### Questions auxquelles répondre

1. **Qu'est-ce qu'on peut déjà mesurer sans rien changer ?**
   - Le `sendTimestamp` qu'on vient d'ajouter au header UDP donne quoi dans les stats côté Mac ? Montre-moi les derniers logs si dispo.
   - Le timecode LTC visuel incrusté dans le test pattern EC2 : quelle est sa source d'horloge exacte dans le code actuel ? (`clock_gettime(CLOCK_REALTIME)` à chaque frame ? Compteur initialisé au démarrage ? Autre ?)
   - Le timecode NDI metadata (`videoFrame.timecode`) : c'est `NDIlib_send_timecode_synthesize` ou une valeur custom ?

2. **Quels sont les maillons de latence identifiables ?**
   Pour chaque étape, donne-moi ce que tu sais ou peux déduire du code :
   ```
   Génération frame CPU (test pattern + LTC) → ?ms
   Encodage H.264 libx264 sur EC2        → ?ms  
   Empaquetage UDP + envoi réseau         → ?ms (le sendTimestamp mesure à partir d'ici)
   Transit réseau EC2→Mac (Tailscale?)    → ?ms
   Réception + décodage côté Mac          → ?ms
   Republication NDI locale               → ?ms
   Rendu dans le moniteur NDI             → ?ms (hors de notre contrôle)
   ```

3. **Le delta Mac timed (+612ms) impacte quoi exactement dans cette chaîne ?**
   Le `sendTimestamp` utilise `CLOCK_REALTIME` des deux côtés. Si le Mac a +612ms de dérive, est-ce que la latence affichée dans les stats est faussée de 612ms ? Dans quel sens ?

### Ce que tu ne fais PAS

- Tu ne modifies aucun fichier
- Tu ne proposes pas de "fix" ou d'amélioration
- Tu ne décides pas que quelque chose "n'est pas grave"
- Si tu identifies un problème ou une opportunité, tu me le présentes avec les options

Commence par lire les deux documents demandés, puis réponds aux questions.
