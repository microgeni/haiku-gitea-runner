# End-to-end smoke-testing the Haiku Gitea runner

This directory contains helper scripts for driving `haiku-act-runner`
against a live Gitea server — useful for:

* verifying the Connect-RPC client matches what Gitea actually sends
* confirming the local `CacheServer` is picked up by jobs using
  `actions/cache`
* regression-testing matrix/`needs:`/step-output plumbing end-to-end

All scripts are plain POSIX `bash` — no extra runtime required on the
Haiku side beyond what `pkgman` already provides (`curl`, `python3`
for JSON parsing, `base64`).

---

## Layout

```
scripts/
├── gitea_up.sh                # spin up a throwaway Gitea in Docker (on a
│                                Linux/macOS box; Haiku can't run Docker)
├── e2e_smoke.sh               # main driver — runs on Haiku, exercises
│                                one or more sample workflows
├── workflows/                 # the sample workflow .yml files
│   ├── hello.yml              # simple run: step, prints uname
│   ├── cache-roundtrip.yml    # save → need → restore; exercises CacheServer cache API
│   ├── artifact-roundtrip.yml # upload → download; exercises CacheServer artifact API
│   ├── matrix.yml             # multiple matrix combinations
│   └── needs.yml              # two-job DAG with needs.<job>.outputs.*
└── .gitea-env.json            # produced by gitea_up.sh, read by e2e_smoke.sh
```

---

## Quick start (typical two-machine setup)

### On a Linux/macOS host with Docker

```console
$ ./scripts/gitea_up.sh
[gitea_up] Using Docker engine: docker
[gitea_up] Starting Gitea container...
[gitea_up] Waiting for /api/v1/version (up to 120 s)...
[gitea_up] Creating admin user 'smoketest'...
[gitea_up] Creating personal access token...
[gitea_up] Fetching runner registration token...
[gitea_up] Wrote ./scripts/.gitea-env.json
┌────────────────────────────────────────────────────────────────────┐
│ Gitea is up!                                                        │
│   URL        : http://192.168.1.10:3000                             │
│   UI login   : smoketest / SmokeTest-1234!                          │
│   Admin PAT  : a3f2b8c…                                             │
│   Runner reg : 1d4e77f…                                             │
└────────────────────────────────────────────────────────────────────┘

$ scp scripts/.gitea-env.json haiku:~/haiku-act-runner/scripts/
```

### On the Haiku box

```console
$ cd ~/haiku-act-runner
$ ./scripts/e2e_smoke.sh                # runs hello.yml
$ ./scripts/e2e_smoke.sh cache-roundtrip
$ ./scripts/e2e_smoke.sh --all          # every workflow in sequence
```

### Tear-down

```console
$ ./scripts/gitea_up.sh down            # on the Docker host
```

The `e2e_smoke.sh` script deletes the throwaway repo + registered runner
on exit, so only `gitea_up.sh down` is needed to clean up the server.

---

## What `e2e_smoke.sh` actually does

1. Reads `.gitea-env.json` (set `GITEA_ENV_FILE=…` to override the path).
2. Builds `build/act_runner` if not already present.
3. Creates a fresh repo `smoke-<timestamp>-<pid>` under the admin user.
4. Pushes `.gitea/workflows/<name>.yml` via the Gitea contents API
   (base64 upload — no git client needed on Haiku).
5. Registers our runner with `--config $HOME/config/settings/act_runner/config.yaml`
   using an isolated `$HOME` so it can't clobber your real runner cfg.
6. Starts `act_runner daemon` in the background, tailing its log into
   `/tmp/act_runner_smoke_*/daemon.log`.
7. Polls `GET /api/v1/repos/<owner>/<repo>/actions/runs` every 3 s until
   the run completes (timeout: `RUN_TIMEOUT`, default 180 s).
8. Prints the conclusion; on failure dumps the last 50 lines of
   daemon.log for fast diagnosis.
9. On exit: kills the daemon, deletes the repo (unless `--keep`), and
   removes the temp settings dir.

---

## Options / environment

| Variable / flag        | Effect                                                   |
|------------------------|----------------------------------------------------------|
| `--all`                | Run every workflow in `scripts/workflows/` in sequence   |
| `--keep`               | Don't delete the Gitea repo on exit (for manual inspection) |
| `GITEA_ENV_FILE=...`   | Path to the JSON file from `gitea_up.sh`                 |
| `BUILD_DIR=...`        | Where `act_runner` lives (default `build/`)              |
| `RUNNER_NAME=...`      | Override the runner's display name                       |
| `RUN_TIMEOUT=300`      | Seconds to wait for a run to complete                    |

---

## Running against an existing Gitea

You don't have to use `gitea_up.sh` — you can hand-author `.gitea-env.json`:

```json
{
  "gitea_url":                 "https://gitea.internal.example",
  "admin_user":                "ci-admin",
  "admin_pass":                "",
  "admin_token":               "ghp_…",
  "runner_registration_token": "…get from Site Admin → Actions → Runners…"
}
```

The `admin_token` needs these scopes: `write:admin`, `write:repository`,
`write:user`.  The `runner_registration_token` is the **global** runner
token; for per-repo or per-org runners, adapt `e2e_smoke.sh` accordingly.

---

## Troubleshooting

### "runner did not appear online"

Check `/tmp/act_runner_smoke_*/daemon.log`:

```
$ tail -f /tmp/act_runner_smoke_*/daemon.log
```

Common causes:
- Gitea URL typo in `.gitea-env.json`
- Haiku can't reach the Gitea host (firewall / VPN)
- `insecure: true` missing in config when Gitea uses a self-signed cert

### "cache-roundtrip fails with cache miss"

Jobs must run on the **same runner instance** for the local cache to be
shared.  If you run multiple Haiku runners, either reduce capacity to 1
or point them at a shared cache dir (symlink
`~/config/settings/act_runner/cache`).

### The runner gets stuck on FetchTask

Gitea Actions must be enabled for the repo:
*Settings → Actions → Enable*. `gitea_up.sh` enables it globally via
`GITEA__actions__ENABLED=true`; for existing servers, check the admin
panel.
