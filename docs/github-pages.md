# Publishing the documentation

The GitHub Pages site uses MkDocs Material: task pages, sidebar navigation,
search, light/dark themes, and Mermaid diagrams. The README remains readable on
GitHub and is the single source for the task instructions.

## Enable publishing once

1. Commit and push the documentation configuration to `master`.
2. In the repository, open **Settings → Pages**.
3. Under **Build and deployment → Source**, select **GitHub Actions**.
4. In **Actions → Documentation**, run the workflow if it has not already run.
5. After deployment succeeds, visit <https://jcergolj.github.io/omacustos/>.

See GitHub's [Pages setup instructions](https://docs.github.com/en/pages/getting-started-with-github-pages/creating-a-github-pages-site).
The site is public; do not put private configuration or credentials in the docs.

## Update the content

- Edit `README.md` for the getting-started and everyday-use task pages.
- Edit `MANUAL.md`, `TECHNICAL.md`, or `docs/*.md` for reference material.
- Edit `mkdocs.yml` to change sidebar navigation or theme settings.
- If you add or remove a numbered README section, update `GUIDES` in
  `tools/prepare-pages.py` and the sidebar navigation too.

The preparation script splits the README into short task pages, including separate
pages for cross-computer recovery, writer handoff, and each group of controls.
It updates links and adapts GitHub alerts and expandable sections. Other repository-file links go to
GitHub. It only stages documentation and the settings template, never binaries
or local configuration. Do not edit the generated `.pages-docs/` or `site/` files.

Pushes to `master` affecting documentation rebuild and deploy the site. Pull
requests build it without deployment. An Actions deployment failure does not
replace the last successfully published site.

## Preview locally

From the repository root, with Python and venv support installed:

```bash
python -m venv /tmp/opencode/omacustos-docs-venv
/tmp/opencode/omacustos-docs-venv/bin/pip install -r requirements-docs.txt
python tools/prepare-pages.py
/tmp/opencode/omacustos-docs-venv/bin/mkdocs serve
```

Open the local URL printed by MkDocs. After editing source Markdown, run the
preparation script again; MkDocs watches its generated inputs. To check the
production build:

```bash
python tools/prepare-pages.py
/tmp/opencode/omacustos-docs-venv/bin/mkdocs build --strict
```
