# Publishing the documentation

The GitHub Pages site uses MkDocs Material: task pages, sidebar navigation,
search, light/dark themes, and Mermaid diagrams. The README remains readable on
GitHub and is the single source for the task instructions.

## Enable publishing once

1. Build and publish locally using the commands below. This creates or updates
   the `gh-pages` branch with the generated site, not the Markdown sources.
2. In the repository, open **Settings → Pages**.
3. Under **Build and deployment → Source**, select **Deploy from a branch**.
4. Select **gh-pages**, folder **/ (root)**, and save.
5. After deployment succeeds, visit <https://jcergolj.github.io/omacustos/>.

There is no custom Actions workflow. GitHub still handles the branch-based Pages
deployment internally.

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

Pushing Markdown to `master` does **not** update the live site. After editing,
commit and push the source changes, then publish the built documentation:

```bash
python tools/prepare-pages.py
/tmp/opencode/omacustos-docs-venv/bin/mkdocs gh-deploy --strict
```

Install the preview environment below before the first publication. The deploy
command builds the site and pushes the generated HTML to `origin/gh-pages`;
it does not switch your working branch or commit source changes. It also creates
`.nojekyll` so GitHub serves the generated site without processing it with Jekyll.

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
