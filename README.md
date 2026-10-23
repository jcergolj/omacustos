# OmaCustos for Proton Drive

Back up your documents, photos, projects, and other personal files to Proton Drive
from your Omarchy desktop. Choose what to save, run backups manually or on a
schedule, and restore files from an earlier copy when you need them.

OmaCustos uses Proton's official `proton-drive` CLI and runs as your user. It backs
up selected files and folders, not system images or live databases.

**[User manual](MANUAL.md)** · [Technical notes and development](TECHNICAL.md)

**Jump to:** [Get started](#get-started) · [Restore files](#restore-your-files) ·
[Things to know](#things-to-know) · [Help](#help-and-more-options)

---

## Get started

[1. Install](#1-install) → [2. Sign in](#2-sign-in) →
[3. First backup](#3-make-your-first-backup) → [4. Schedule](#4-make-it-automatic)

### 1. Install

On Arch Linux or Omarchy:

```bash
sudo pacman -S --needed base-devel git
git clone https://github.com/jcergolj/omacustos.git
cd omacustos/pkgbuild
makepkg -Csi
```

Install Proton's official `proton-drive` CLI separately using a package or release
source supported by your system, then open **OmaCustos for Proton Drive** from
your app launcher.

### 2. Sign in

If the app asks you to authenticate, press **Sign in to Proton**. Complete the
browser login and keep the terminal open until it finishes. Press **Retry** if
needed. OmaCustos uses the CLI's login session; it never stores your Proton password.

If the terminal does not open, see [sign-in instructions](MANUAL.md#authenticate-proton-drive).

### 3. Make your first backup

1. Press **Create your first backup set**, or choose **New backup set** from the
   top-right **⋯** menu.
2. Give it a name and use **+** under **Source files and folders** to choose files
   or folders.
3. Add any **Exclusions**, such as `node_modules` or `vendor`, one per line.
   To save only specific contents of a parent folder, use **Include only**, for
   example `.env` for all its `.env` files, including subfolders.
4. Use **Preview** to check what will be included.
5. Press **Save**, then **Back up now** on the dashboard card.

#### How backup history works

The first backup saves everything you selected. Later backups upload only new or
changed files. If nothing changes, no new restore point is created.

- **Location:** `/my-files/backups`
- **History:** Keeps the latest three restore points, including older backups.
  Extra history stays until you approve cleanup.
- **Cleanup:** Use **⋯ → Review incremental cleanup…** when prompted.

> [!WARNING]
> **Upgrading or moving computers?** Read the
> [upgrade and migration guide](MANUAL.md#advanced-settings) before starting backups.

### 4. Make it automatic

Edit your backup, choose a **daily**, **weekly**, or **monthly** schedule, and
press **Save**. Scheduling starts automatically, and you can close the app.

Scheduled backups run while you are logged in and catch up after your next login.
They cannot run while the computer is off. If scheduling needs attention, the app
shows a suggested fix and an **Enable scheduling** button.

## Restore your files

1. Press **Restore copies…** on the backup's dashboard card.
2. Choose a copy and leave the files you want selected.
3. Use **Choose folder…** to select a separate destination folder.
4. Press **Start restore**.

> [!IMPORTANT]
> **Before reinstalling or moving computers, keep a copy of `~/.config/omacustos`.**
> It preserves backup definitions and run history for access through the app. A
> fresh installation has no standalone discovery action in the UI. See
> [reinstall and migration instructions](MANUAL.md#reinstall-or-move-to-another-computer).

### Recovery without local configuration

Default incremental backups also support [read-only recovery without local
configuration](docs/incremental-restore.md). This advanced recovery path uses the
`build/omacustos-incremental` tool, which must be built from source and is not
included in the installed package. Recovery does not recreate schedules or grant
write access on a replacement computer.

## Things to know

### Included files and run results

- Folder backups include hidden files such as `.env` and `.git`. Add exclusions
  for anything you do not want backed up.
- A successful incremental run reports either a **New saved state** or a
  **Successful check; no saved change**. Historical copies may also show
  **Incomplete**, meaning only their verified files are restorable. Use
  **View issues** or **View latest run details** for failures and warnings.

### Verification and safety

- New uploads and restores are fully verified. Daily checks of reused archives
  can miss same-sized remote corruption. Full integrity checks advance every
  30 days when backups run; use **⋯ → Run full integrity check** to check now or
  resume unfinished coverage.
- Failed or incomplete runs do not remove older successful copies. Every cleanup
  proposal requires your confirmation.
- Deleting a backup set removes its settings and schedule, but leaves its remote
  copies in Proton Drive.

### Backup limitations

- Capture is file-by-file, not an atomic filesystem snapshot. Edited files are
  uploaded whole, and partly used archives keep obsolete contents until the
  entire archive is no longer needed; archives are not compacted.

## Help and more options

The **[user manual](MANUAL.md)** covers exclusions, backup settings, resource usage,
import/export, managing copies, and uninstalling.

| I want to… | Go to |
| --- | --- |
| Fix a problem | [Troubleshooting](MANUAL.md#troubleshooting) |
| Transfer backup settings | [Import and export backup sets](MANUAL.md#import-and-export-backup-sets) |
| Start from a settings template | [Backup-set template](backup-sets.template.json) |
| Build or contribute | [Technical notes and developer instructions](TECHNICAL.md) |
