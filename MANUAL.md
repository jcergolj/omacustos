# OmaCustos user manual

This manual covers installing OmaCustos for Proton Drive, managing backups,
and restoring your files. For a shorter walkthrough, see the [README](README.md).
For implementation details and developer commands, see [Technical Notes](TECHNICAL.md).

## Contents

- [Install and sign in](#install-and-sign-in)
- [Create and run a backup](#create-and-run-a-backup)
- [Schedule automatic backups](#schedule-automatic-backups)
- [Desktop theme](#desktop-theme)
- [Exclude files and folders](#exclude-files-and-folders)
- [Import and export backup sets](#import-and-export-backup-sets)
- [Restore](#restore)
- [Manage copies](#manage-copies)
- [Daily operation](#daily-operation)
- [Troubleshooting](#troubleshooting)
- [Uninstall](#uninstall)

## Install and sign in

### Install OmaCustos

On Arch Linux or Omarchy, build and install the package:

```bash
sudo pacman -S --needed base-devel git
git clone https://github.com/jcergolj/omacustos.git
cd omacustos/pkgbuild
makepkg -Csi
```

Open **OmaCustos for Proton Drive** from your app launcher, or run `omacustos`.

### Authenticate Proton Drive

Install the `proton-drive` CLI using the package or release source supported by
your system. Authenticate it as the same user who will run OmaCustos.

When OmaCustos cannot connect to Proton Drive, it shows a visible error with a
suggested fix. If the CLI is missing, install `proton-drive` and press **Retry**.
If you need to authenticate, press **Sign in to Proton** to open the CLI login
in your terminal, which launches your browser. Keep the terminal open until
authentication completes. OmaCustos checks the connection again automatically;
use **Retry** to check immediately. No connection status is shown when healthy.

You can also sign in from a terminal:

```bash
command -v proton-drive
proton-drive auth login
proton-drive filesystem info /my-files
```

OmaCustos uses this login session. It never asks for or stores your Proton password.
The sign-in button uses `xdg-terminal-exec`, available on Omarchy. If it cannot
open a terminal, use the commands above.

## Create and run a backup

1. Open **OmaCustos** and choose **New backup set** from the top-right **⋯** menu,
   or press **Create your first backup set** on an empty dashboard.
2. Name the set, such as **Documents** or **Projects**.
3. Use **+** under **Source files and folders** to choose what to back up.
4. Select exclusions with **+** under **Exclusions**, or enter names or paths,
   one per line.
5. Choose a schedule, or leave scheduling disabled for manual backups.
6. Press **Save**, return to the dashboard, and press **Back up now** on the set's card.

The bottom **Cancel** button returns to the dashboard. When creating a new backup
set, Cancel discards it unless Save succeeded. Preview alone does not create a
saved set.

Hidden paths such as `~/.config` are valid sources; select the folder or enter its
full path in a source field. Folder backups include hidden files and hidden
subdirectories by default, including `.env` and `.git`; use exclusions to omit
any you do not want backed up. Use **Preview** to review included, excluded,
skipped, and missing paths before running.

Preview shows separate **Included**, **Excluded**, **Skipped**, and **Missing**
sections with counts and an explanation when a section is empty. Scroll each
nonempty list to inspect its full paths; paths wrap and can be selected and copied.
The **Included** heading also shows the total size of files to back up across the
selected files and folders. Overlapping sources count each file once; excluded, skipped, and missing
paths do not contribute to the total. This is the source data size at preview time.
Only included files will be attempted. Preview does not save settings, start a
backup, or enable scheduling; press **Preview** again after editing the selection.

Each named backup set has its own files, exclusions, and schedule. Every run
starts a separate copy; recovery and Resume continue that unfinished copy.
By default, copies are stored under `/my-files/backups`
in Proton Drive, and the latest three verified successful copies are kept.

### Dashboard and results

The dashboard shows a card for each backup set with its sources, latest attempt,
last successful backup when relevant, schedule, next scheduled run, and retention policy.
**Back up now** starts that set; click its name or **⋯ → Edit settings** to edit it.

A card summarizes the latest run, not every stored copy. **Restore copies…**
opens the copy picker so you can choose the latest or an older available copy.
See [Restore](#restore) for file selection and destination options.
“Retention: keep 3 successful copies” is the configured policy, not a live count
of copies in Proton Drive. Deleting the latest copy keeps the set's card and access
to older copies. **⋯ → Open latest copy in Proton Drive** and **Delete latest copy**
refer specifically to the most recently recorded copy.

Results distinguish **Successful**, **Incomplete** (some verified files, with
failures), and **Failed** (no verified restorable copy). Completed copies show the
verified-file count and failed-item count. Press **View issues** or open
**⋯ → View latest run details** for affected
paths, failure reasons, and the next automatic retry. An incomplete copy still
allows restoring its verified files; it does not replace an older successful copy
for retention purposes. Failed items can include missing or unreadable folders,
so they are counted as items rather than always as individual files.

### Backup progress

While a backup runs, its card shows its current phase: scanning, preparation,
uploading, verification, saving verified progress, or finalization. It shows
processed/total files, verified payloads, and verified data. The CLI does not
provide reliable live byte progress, so the app does not show live upload speed,
a byte percentage, or an ETA for these transfers.

The running backup's card shows a work-progress bar, processed and verified file
counts, and failures. **⋯ → View latest run details** shows the current file's path. Its phase distinguishes
reading, uploading, and verification. The bar advances as file attempts finish;
failed attempts count as processed work, never as verified files. A large file
may stay at its current step while being transferred.

### Advanced settings

**Advanced settings** lets you change the remote folder and number of copies
to keep, run only on AC power, or choose a **staging directory** and **staging
budget in bytes**. The default budget is **1 GB (1000000000 bytes)**.
Every fresh manual or scheduled run, including future runs of existing saved
backups, creates bounded **tar.gz archives using gzip level 3**. There is no
storage-format or compression-level setting. Selected source roots remain
separate; small directories share archives rather than creating remote objects
for every directory or file.

The normal grouping target is approximately **1 GB of uncompressed contents**.
Groups are smaller when necessary to fit immutable snapshots, compressed output,
and tar/gzip overhead together within the staging budget. Preparation/compression,
upload, verification, and saving verified progress finish for one archive before
the next is prepared. The progress counts describe original source files and
bytes; preparation is not completed upload. Total selection size does not
determine required temporary payload space. Final-index work also uses the
staging disk and requires additional free space.

Automatic staging uses disk-backed application storage outside your sources,
falling back to a private directory in `/var/tmp` when necessary. RAM-backed
storage such as `/tmp` on Omarchy, source overlaps, and unusable locations are
rejected. Leave the staging directory blank to select it automatically.

A file larger than the normal target or preparation allowance stays whole in its
own oversized archive. It is never split. This exception can exceed the normal
budget: the staging disk must accommodate the entire immutable snapshot plus
compressed output and overhead, without assuming useful compression. If space
or your disk quota is insufficient, the copy waits with completed
work preserved. Free space or choose another staging disk, save the settings,
then press **Resume**. Files are never silently omitted for being too large.

Fresh daily runs still create **full copies**, including unchanged files.
Changed-content/incremental backups and cross-copy deduplication are not provided.
Existing version-1/version-2 individual-file copies remain browsable, restorable,
deletable, and subject to retention without conversion or re-upload. An unfinished
older attempt resumes its original format and exact copy namespace; a fresh run
after success, Incomplete, failure, or cancellation uses archives.

### Pause, Resume, and Cancel

- **Pause** stops the current transfer and releases temporary payloads while
  preserving verified progress. The scheduler respects Pause until you press Resume.
- **Resume** re-scans sources and exclusions and continues the same remote copy.
  New/changed files are included; deleted/excluded files are omitted from the final
  manifest. Reuse requires matching local content and fresh remote verification.
  Size-only remote metadata may require a verification download.
- **Cancel** permanently ends that attempt. The next requested or scheduled backup
  starts a new copy. Run details retain cancelled remote paths so you can explicitly
  remove partial data in Proton Drive when no longer needed.

Closing the window lets the systemd worker continue. Interruptions and transient
transfer failures keep the copy unfinished and retry with backoff through the
installed scheduler. Authentication and staging-space problems show actionable
waiting states. Payload checkpoints alone do not make a copy restorable: its final
manifest and payloads must also pass verification. Incomplete source-read outcomes
can expose their verified files for restore, without removing older successful copies.

### Resource usage

**Resource usage (all backups)** controls how much CPU and disk priority backups
receive. Leave **Use system defaults** checked for normal system scheduling,
or uncheck it and choose a preset. Lower presets reduce backup resource usage;
higher presets allow more CPU time.

Press **Save** to apply your choice to all manual and scheduled backups. A running
backup keeps its current limits; the next worker uses the new setting. Tick
**Use system defaults** and save to remove custom limits. The desktop interface
always uses normal system scheduling. For exact preset limits, see
[Services and packaging](TECHNICAL.md#services-and-packaging).

## Schedule automatic backups

Choose a **daily**, **weekly**, or **monthly** schedule and press **Save**.
OmaCustos automatically enables and starts its scheduler; no terminal setup is
needed. Importing backup sets with enabled schedules also activates scheduling.
Manual-only backup sets do not activate it.

If a monthly day does not exist in the current month, the last day of that month
is used.

The scheduler area stays hidden when scheduling is working. If activation fails,
the timer is paused, or it is not enabled to start at login, a visible error
explains what to do. Your backup settings remain saved. Press **Enable scheduling**
to retry. OmaCustos checks scheduling automatically while the app is open.

The timer runs as your user while logged in and catches up overdue backups after
your next login. It does not run while the computer is off or require user
lingering for this login-session behavior. Make sure Proton Drive is signed in
before expecting scheduled backups to succeed.

You can close OmaCustos: systemd starts the scheduler automatically at login and
runs scheduled backups independently of the app. The app does not need to start
at login or stay running in the background.

## Desktop theme

OmaCustos follows your active Omarchy theme and updates its colors while open when
you switch themes. On other desktops, it uses the system's Qt color palette.

## Exclude files and folders

Enter exclusions one per line:

```text
node_modules
vendor
/home/you/projects/cache
```

A folder name excludes every matching folder at any depth. A full path excludes
only that file or folder. You can also select a specific path with **+**.

## Import and export backup sets

Open the top-right **⋯** menu and choose **Export** to save your backup sets as a
JSON file, or **Download template** to save an annotated example for editing.
Use **Import** to select a JSON file, then choose how to apply it:

- **Merge** keeps other existing sets, adds sets with new IDs, and updates sets
  whose IDs match the imported file. Matching names alone do not combine sets.
- **Replace** replaces the entire configured set list with the imported sets.

OmaCustos automatically validates the file before applying either choice. If it
is invalid, the app shows an error and keeps your existing settings.

The file contains sources, exclusions, schedules, and settings. Your backed-up
files, Proton login, and this computer's global resource preset are not included.

### Create an import file

In OmaCustos, open **⋯ → Download template** and choose where to save the file.
The template is bundled with the app, so this works offline and before you have
created any backup sets.

You can also download the [annotated backup-set template](backup-sets.template.json)
([raw file](https://raw.githubusercontent.com/jcergolj/omacustos/HEAD/backup-sets.template.json))
and save a copy as `my-backup-sets.json`. On GitHub, open the template and use
**Download raw file**, or save the raw link from your browser.

1. Replace the example source paths with the full paths of your folders and files,
   such as `/home/alex/Documents` or `/home/alex/notes.txt`. Use your actual username;
   do not use `~` or environment variables such as `$HOME`.
2. Set the backup's `name` and `id`. Each set must have a nonempty, unique `id`.
   To create multiple sets, duplicate the object inside `sets` and edit each copy.
3. Choose exclusions, the remote folder, retention, and schedule. The template runs
   **daily at 02:00 in your computer's local time**. Set `frequency` to `disabled`
   for manual backups, or use `daily`, `weekly`, or `monthly`. Hours are 0–23 and
   minutes are 0–59; weekdays are 1–7 (Monday–Sunday), and monthly days are 1–31.
4. Save the file, press **Import** in OmaCustos, and select your file. Choose
   **Merge** to keep other existing sets or **Replace** to use only this file's
   sets. Export your current sets first if you want to keep a copy before replacing
   them. Enabled schedules activate automatic scheduling.
5. Open each imported set and use **Preview** to check its sources and exclusions.

The template includes `_comment` fields explaining the format and giving LLMs
instructions for generating a file. OmaCustos ignores these fields; you can keep
or remove them. JSON does not allow `//` or `/* … */` comments or trailing commas.

### Generate an import file with an LLM

Attach the downloaded template to your LLM conversation and use a prompt like
this, replacing the paths with your own:

```text
Using the attached OmaCustos template and its _comment instructions, create
one backup set named "Personal files" for the following folders and files.
Run it daily at 02:00 local time, keep 3 successful copies, use
/my-files/backups as the remote root, and allow backups on battery power.
Exclude folders named node_modules and vendor.

/home/alex/Documents
/home/alex/projects
/home/alex/.config/hypr/hyprland.conf

Return only the complete importable JSON, without Markdown code fences.
```

Save the response as `my-backup-sets.json`, review the paths and schedule, then
import it and check **Preview** as described above. Use **Merge** to keep other
existing sets. To modify an existing set, attach an export and ask the LLM to
preserve that set's ID so merging updates it.

### Automatic import validation

When you choose **Merge** or **Replace**, OmaCustos checks JSON syntax, the
application and version, required fields, unique IDs within the file, value
types, and schedule and retention ranges before saving any changes. Errors point
to the field to fix, such as `sets[0].schedule.hour`; incorrect values are rejected
rather than silently replaced with defaults. The template's `_comment` fields
are accepted.

Use **Preview** after importing to check local source paths, and authenticate
Proton Drive before running a backup. An empty `sets` array is valid: **Replace**
clears the configured set list, while **Merge** keeps it.

## Restore

1. Press **Restore copies…** on a backup set's card that has a recorded run.
2. The restore panel opens immediately and loads copies for that backup in the
   background. Select a remote copy to load and verify its files.
3. All verified files are selected by default. Untick any files or folders you
   want to leave out. A folder checkbox includes its subfolders;
   **Select all files** selects or clears the whole copy.
4. Use **Choose folder…** to select a separate destination folder, or enter its
   full path.
5. Press **Start restore** at the bottom. The button is available when at least
   one file is selected and a destination folder is specified.

After a successful restore, the restore panel closes and OmaCustos returns to the
dashboard. If a restore fails, the panel and your selection stay open for retry.

**Required download** updates as you change the selection. For archive copies it
shows compressed bytes and the number of unique archives required: selecting
several files from one archive downloads that archive only once. Only selected
files are restored, but downloading even one small file requires downloading its
entire containing compressed archive. This is the selective-restore download
amplification shown by **Required download**; it is not a transfer-time estimate.
Each archive's temporary workspace is
released before the next archive is downloaded. Older individual-file copies
show the total bytes of the selected files. No download-time estimate is shown.

Incomplete copies expose only verified entries. Missing, failed, malformed, or
unverifiable items are not presented as successful restores.

### While restoring

You can close the Restore panel, edit a backup, or open another backup while files
are restoring. The active restore keeps running; its file-count progress bar and
**View restore** action stay available above the current screen. Returning to a
backup preserves the Restore destination, valid ticks, and file-list position.
Cached file information is marked and must be verified again before a new restore.
Loading other copies may wait for an active restore to finish.

### Reinstall or move to another computer

For a reinstall or move to another computer, keep or transfer `~/.config/omacustos`
so the backup definitions and run history remain available, then install OmaCustos
and authenticate `proton-drive` as the new machine's user. The current UI opens
remote discovery through **Restore copies…** on a recorded backup; it does not yet have
a standalone remote-root discovery action for a fresh installation.

## Manage copies

- **⋯ → Open latest copy in Proton Drive** opens the latest recorded copy in your browser.
- **⋯ → Delete latest copy** confirms the exact copy before moving it to Proton Drive Trash.
- **⋯ → Delete backup set** removes its configuration and schedule; remote copies remain.

## Daily operation

- Use **Preview** before a first backup or after changing sources and exclusions.
- Retention keeps three verified successful copies by default.
- Failed or incomplete runs do not remove older successful copies.
- The first cleanup proposal requires confirmation; later cleanups use the saved
  decision.
- To stop scheduling without uninstalling:

```bash
systemctl --user disable --now omacustos.timer
```

OmaCustos shows a scheduling-paused error. Use **Enable scheduling** to resume. Saving
backup settings while an enabled schedule exists also reactivates the timer.
To make a backup manual-only, choose **disabled** for its schedule and save.

## Troubleshooting

### Connection or scheduling errors

Follow the suggested fix shown in the app. Install `proton-drive` if it is missing,
use **Sign in to Proton** if authentication is needed, then press **Retry**.
For a paused or inactive scheduler, press **Enable scheduling**.

### Missing files or a waiting backup

Use **Preview** to check your sources and exclusions. After a run, use
**View issues** or **⋯ → View latest run details** for affected paths and reasons.
An AC-power requirement makes a backup wait while on battery power.

### Logs and command-line checks

Check the worker log:

```bash
journalctl --user -u omacustos.service
```

Check the timer and Proton CLI:

```bash
systemctl --user status omacustos.timer
command -v proton-drive
proton-drive filesystem info /my-files
```

### Slow transfers and timeouts

Uploads and downloads can run for up to 24 hours per CLI command. A large file
may remain at the same progress step during transfer. Timeout errors are shown
separately from start failures and crashes, and a timed-out backup does not
remove older successful copies. To adjust the limit, see
[Transfer timeout configuration](TECHNICAL.md#transfer-timeout-configuration).

## Uninstall

Stop scheduling first if it is enabled, then remove the package:

```bash
systemctl --user disable --now omacustos.timer
sudo pacman -Rns omacustos-git
```

Package removal does not delete `~/.config/omacustos`, the user-systemd resource
drop-in, or remote backups. Keep your configuration and backups if you may need
to restore later.
