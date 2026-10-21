# OmaCustos for Proton Drive

Back up your documents, photos, projects, and other personal files to Proton Drive
from your Omarchy desktop. Choose what to save, run backups manually or on a
schedule, and restore files from an earlier copy when you need them.

OmaCustos uses Proton's official `proton-drive` CLI and runs as your user. It backs
up selected files and folders, not system images or live databases.

**[User manual](MANUAL.md)** · [Technical notes and development](TECHNICAL.md)

## Get started

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

Fresh manual and scheduled runs use whole-file incremental backups with immutable
tar.gz archives (gzip level 3). The first run saves a fresh baseline; later runs
reuse unchanged contents. An unchanged check creates no new saved state. The
restore picker still shows individual files and their download cost.

By default, history lives under `/my-files/backups` and the newest **three saved
states** are retained. Cleanup needs an individual review of its exact deletion
proposal: use **⋯ → Review incremental cleanup…** when attention is reported.
Older formats remain restorable and are included in mixed-format retention.
Imported, copied, and pre-ownership settings stay read-only until explicit offline
writer enrollment/takeover. See [upgrade and safety details](docs/incremental-default-rollout.md)
and the [staging and archive details](MANUAL.md#advanced-settings).

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

**Before reinstalling or moving computers, keep a copy of `~/.config/omacustos`.**
It contains the backup definitions and run history needed to access copies through
the app. A fresh installation cannot yet discover backups on its own. See
[reinstall and migration instructions](MANUAL.md#reinstall-or-move-to-another-computer).

Advanced v4 shared-archive repositories also support [read-only recovery without
local configuration](docs/incremental-restore.md) through `omacustos-incremental`.
This does not change default UI backups or enable writes on a replacement machine.

## Things to know

- Folder backups include hidden files such as `.env` and `.git`. Add exclusions
  for anything you do not want backed up.
- **Incomplete** means some files were verified but others failed; **Failed** means
  there is no verified restorable copy. Use **View issues** for details.
- Failed or incomplete runs do not remove older successful copies. The first
  cleanup requires your confirmation.
- Deleting a backup set removes its settings and schedule, but leaves its remote
  copies in Proton Drive.

## Help and more options

The **[user manual](MANUAL.md)** covers exclusions, backup settings, resource usage,
import/export, managing copies, and uninstalling.

- [Troubleshooting](MANUAL.md#troubleshooting)
- [Import and export backup sets](MANUAL.md#import-and-export-backup-sets)
- [Backup-set template](backup-sets.template.json)
- [Technical notes and developer instructions](TECHNICAL.md)
