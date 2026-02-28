# BHC Scheduler — Orchestrateur de jobs

Démon léger écrit en C qui expose une API REST pour soumettre, suivre et annuler des jobs sur un parc de machines. Repose sur [Mongoose](https://mongoose.ws/) (HTTP), SQLite (persistance) et cJSON.

---

## Prérequis

| Outil | Version minimale |
|---|---|
| CMake | 3.16 |
| Visual Studio / MSVC (Windows) | VS 2019+ (toolset v142+) |
| GCC + Make (Linux) | GCC 9+ |

---

## Installation

### Windows

```powershell
# 1. Cloner le dépôt
git clone https://github.com/<votre-org>/BHC_SCHEDULER.git
cd BHC_SCHEDULER

# 2. Générer les fichiers de build
cmake -S . -B build

# 3. Compiler (Debug)
cmake --build build --config Debug

# L'exécutable est déposé dans :
#   build\bin\Debug\orchestrator.exe
# Le dossier config\ est copié automatiquement à côté de l'exe.
```

### Linux

```bash
git clone https://github.com/<votre-org>/BHC_SCHEDULER.git
cd BHC_SCHEDULER

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# L'exécutable :  build/bin/orchestrator
```

---

## Configuration

Le fichier `config/orchestrator.conf` est copié automatiquement à côté de l'exécutable à chaque build. Tous les paramètres sont optionnels — les valeurs par défaut sont utilisées si le fichier est absent.

```ini
# ── HTTP ──────────────────────────────────────────
listen_port         = 8080

# ── Chemins ───────────────────────────────────────
# Chemins relatifs à l'exécutable (Windows)
work_dir            = jobs
db_path             = orchestrator.db
provisioning_json   = provisioning.json

# ── Logs ──────────────────────────────────────────
# debug | info | warn | error
log_level           = info

# ── Scheduler ─────────────────────────────────────
scheduler_poll_ms   = 500

# ── Nettoyage automatique ─────────────────────────
# Supprime work_dir/input+output après N secondes (0 = désactivé)
cleanup_ttl_seconds = 3600
```

---

## Provisioning des machines

Décrivez le parc de machines dans `provisioning.json` (placé à côté de l'exe) :

```json
{
  "machines": [
    {
      "id":        "srv-01",
      "hostname":  "server01.local",
      "ip":        "192.168.1.10",
      "enabled":   true,
      "cores":     8,
      "gpu_count": 1,
      "ram_mb":    16384,
      "disk_mb":   204800
    },
    {
      "id":        "local",
      "hostname":  "localhost",
      "ip":        "127.0.0.1",
      "enabled":   true,
      "cores":     4,
      "gpu_count": 0,
      "ram_mb":    8192,
      "disk_mb":   102400
    }
  ]
}
```

Il est aussi possible d'ajouter/supprimer des machines à chaud via l'API (`POST /provision` / `DELETE /provision/:id`).

---

## Démarrage

```powershell
# Windows — mode console (sans droits admin)
.\build\bin\Debug\orchestrator.exe

# Avec un fichier de config personnalisé
.\build\bin\Debug\orchestrator.exe --conf C:\chemin\vers\orchestrator.conf
```

```bash
# Linux
./build/bin/orchestrator
./build/bin/orchestrator --conf /etc/orch/orchestrator.conf
```

---

## Génération d'une clé API

L'authentification utilise un header `X-API-Key`. La clé brute n'est affichée **qu'une seule fois** — seul son hash SHA-256 est stocké en base.

```powershell
.\build\bin\Debug\orchestrator.exe keygen --label "mon-application"
```

Sortie :
```
API Key generated for label 'mon-application':
a3f8c2e1d4b79f0e...  (64 caractères hex)
Store this key — it will not be shown again.
```

Options :

| Paramètre | Description | Défaut |
|---|---|---|
| `--label "nom"` | Étiquette lisible associée à la clé | `default` |
| `--conf "chemin"` | Chemin vers un fichier de config alternatif | auto-détecté |

---

## API REST

Toutes les routes requièrent le header :
```
X-API-Key: <votre-clé>
```

### Jobs

#### Soumettre un job
```http
POST /jobs
Content-Type: application/json

{
  "command":    "python3 /scripts/train.py",
  "priority":   1,
  "req_cores":  2,
  "req_gpu":    0,
  "req_ram_mb": 2048,
  "req_disk_mb":10240
}
```

| Champ | Type | Obligatoire | Description |
|---|---|---|---|
| `command` | string | ✅ | Commande à exécuter |
| `priority` | int | — | Priorité (0 = plus haute). Défaut : `0` |
| `req_cores` | int | — | Cœurs CPU requis. Défaut : `1` |
| `req_gpu` | int | — | GPU requis. Défaut : `0` |
| `req_ram_mb` | int | — | RAM en Mo. Défaut : `0` |
| `req_disk_mb` | int | — | Disque en Mo. Défaut : `0` |

Réponse `201` :
```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "command": "python3 /scripts/train.py",
  "status": "IN_QUEUE",
  "priority": 1,
  "req_cores": 2,
  "req_gpu": 0,
  "req_ram_mb": 2048,
  "req_disk_mb": 10240,
  "machine_id": "",
  "submitted_at": 1740744229,
  "started_at": 0,
  "ended_at": 0,
  "exit_code": 0
}
```

---

#### Lister les jobs
```http
GET /jobs
```

---

#### Consulter un job
```http
GET /jobs/:id
```

États possibles : `IN_QUEUE` → `STARTING` → `RUNNING` → `FINISHED` / `FAILED` / `CANCELLED`

---

#### Annuler un job
```http
DELETE /jobs/:id
```

---

#### Uploader un fichier d'entrée
```http
POST /jobs/:id/input/:filename
Content-Type: application/octet-stream

<contenu binaire>
```

---

#### Télécharger un fichier de sortie
```http
GET /jobs/:id/output/:filename
```

---

### Ressources

#### Lister les machines
```http
GET /resources
```

---

### Provisioning (à chaud)

#### Ajouter / mettre à jour une machine
```http
POST /provision
Content-Type: application/json

{
  "id":        "srv-02",
  "hostname":  "server02.local",
  "ip":        "192.168.1.11",
  "enabled":   true,
  "cores":     16,
  "gpu_count": 2,
  "ram_mb":    32768,
  "disk_mb":   512000
}
```

#### Supprimer une machine
```http
DELETE /provision/:id
```

---

## Exemples PowerShell

```powershell
$key = "votre-clé-api-ici"
$base = "http://localhost:8080"
$headers = @{ "X-API-Key" = $key; "Content-Type" = "application/json" }

# Soumettre un job
$body = '{"command":"echo hello","req_cores":1,"req_ram_mb":512}'
Invoke-RestMethod -Uri "$base/jobs" -Method POST -Headers $headers -Body $body

# Lister les jobs
Invoke-RestMethod -Uri "$base/jobs" -Method GET -Headers $headers

# Consulter un job spécifique
Invoke-RestMethod -Uri "$base/jobs/<id>" -Method GET -Headers $headers

# Annuler un job
Invoke-RestMethod -Uri "$base/jobs/<id>" -Method DELETE -Headers $headers

# Uploader un fichier d'entrée
Invoke-RestMethod -Uri "$base/jobs/<id>/input/data.csv" -Method POST `
    -Headers @{ "X-API-Key" = $key } `
    -ContentType "application/octet-stream" `
    -InFile "C:\data\data.csv"

# Télécharger un fichier de sortie
Invoke-RestMethod -Uri "$base/jobs/<id>/output/result.json" -Method GET `
    -Headers @{ "X-API-Key" = $key } `
    -OutFile "C:\data\result.json"

# Lister les machines
Invoke-RestMethod -Uri "$base/resources" -Method GET -Headers $headers
```

---

## Structure du projet

```
BHC_SCHEDULER/
├── src/
│   ├── main.c                  # Point d'entrée
│   ├── core/                   # Scheduler, job state machine, queue, executor
│   ├── http/                   # Serveur HTTP, routes, auth, réponses
│   ├── persistence/            # SQLite, config, logs
│   ├── resources/              # Registre de machines, allocateur
│   ├── transfer/               # Upload / download de fichiers
│   └── platform/               # Abstraction Windows Service / Linux daemon
├── include/                    # En-têtes publics
├── vendor/                     # Mongoose, SQLite, cJSON (amalgamés)
├── config/
│   ├── orchestrator.conf       # Configuration principale
│   └── provisioning.json       # Parc de machines initial
└── CMakeLists.txt
```

---

## Licence

Voir [LICENSE](LICENSE) et [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md).
