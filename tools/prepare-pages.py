"""Prepare documentation without duplicating the README's task instructions."""

from pathlib import Path
import os
import re
import shutil


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / '.pages-docs'
REPOSITORY = 'https://github.com/jcergolj/omacustos/blob/master/'
GUIDES = [
    'install', 'first-backup', 'restore', 'settings', 'schedules',
    'integrity', 'history', 'controls', 'troubleshooting',
]


def slug(heading):
    return re.sub(r'[^\w\- ]', '', heading.lower()).replace(' ', '-')


def prepare():
    # Only remove the dedicated, generated staging directory.
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    OUTPUT.mkdir()

    readme = (ROOT / 'README.md').read_text()
    sections = re.split(r'(?=^## \d+\. )', readme, flags=re.MULTILINE)
    if len(sections) != len(GUIDES) + 1:
        raise ValueError('Update GUIDES to match the numbered README sections')

    pages = {Path('index.md'): sections[0]}
    for name, section in zip(GUIDES, sections[1:]):
        target = Path('guides') / f'{name}.md'
        # A task becomes a page title; its existing heading anchor stays valid.
        pages[target] = section.removeprefix('#')

    # Keep recovery and ownership concerns out of the everyday restore path.
    restore = Path('guides/restore.md')
    before, marker, recovery = pages[restore].partition('### Restore after a reinstall or on another computer\n')
    if not marker:
        raise ValueError('Missing README recovery section')
    pages[restore] = before + '\n## Using another computer?\n\n[Recover after a reinstall or move](guides/recovery.md).\n'
    pages[Path('guides/recovery.md')] = '# Restore after a reinstall or on another computer\n' + recovery

    controls = Path('guides/controls.md')
    handoff = re.search(r'^### Moving backup work to another computer\n.*?(?=^<details>|\Z)', pages[controls], re.MULTILINE | re.DOTALL)
    if not handoff:
        raise ValueError('Missing README writer handoff section')
    pages[Path('guides/handoff.md')] = handoff.group().removeprefix('##')
    pages[controls] = pages[controls].replace(handoff.group(), '')

    # Expandable README reference sections become short, independent web pages.
    topics = [
        ('selection', 'Choose files and selection rules'),
        ('backup-controls', 'Pause, resume, or cancel a backup'),
        ('results', 'Understand backup results'),
        ('advanced', 'Storage, power, and resource settings'),
        ('extras', 'Templates, appearance, and uninstalling'),
    ]
    details = list(re.finditer(r'<details>\n<summary>.*?</summary>\n(.*?)</details>', pages[controls], re.DOTALL))
    if len(details) != len(topics):
        raise ValueError('Update topics to match the README control sections')
    for (name, title), detail in zip(topics, details):
        target = Path('guides') / f'{name}.md'
        pages[target] = f'# {title}\n' + detail.group(1)
        pages[controls] = pages[controls].replace(detail.group(), f'- [{title}]({target.as_posix()})\n')
    pages[controls] += '\n- [Move backup work to another computer](guides/handoff.md)\n'
    pages[controls] = pages[controls].replace('Expand only what you need.', 'Choose the task you need.')

    anchors = {}
    for target, content in pages.items():
        for heading in re.findall(r'^#{1,6} (.+)$', content, re.MULTILINE):
            anchors[slug(heading)] = target

    sources = [ROOT / 'MANUAL.md', ROOT / 'TECHNICAL.md']
    sources += sorted((ROOT / 'docs').glob('*.md'))
    sources += sorted((ROOT / 'native/tests').glob('*.md'))
    for source in sources:
        pages[source.relative_to(ROOT)] = source.read_text()

    def rewrite_links(text, source, destination):
        def rewrite(match):
            url = match.group(1)
            if re.match(r'[a-zA-Z][\w+.-]*:', url) or url.startswith('//'):
                return match.group(0)
            filename, separator, anchor = url.partition('#')
            resolved = (ROOT / source.parent / filename).resolve() if filename else ROOT / source
            relative = resolved.relative_to(ROOT)
            if relative == Path('README.md'):
                target = anchors.get(anchor, Path('index.md'))
            elif relative in pages or relative == Path('backup-sets.template.json'):
                target = relative
            else:
                return '](' + REPOSITORY + relative.as_posix() + (separator + anchor if separator else '') + ')'
            # MkDocs collapses repeated hyphens in heading IDs; GitHub does not.
            anchor = re.sub(r'-+', '-', anchor)
            # Markdown links remain Markdown: MkDocs resolves their final URLs.
            link = Path(os.path.relpath(target, destination.parent)).as_posix()
            return '](' + link + (separator + anchor if separator else '') + ')'

        return re.sub(r'\]\(([^)]+)\)', rewrite, text)

    for destination, content in pages.items():
        source = Path('README.md') if destination == Path('index.md') or destination.parts[0] == 'guides' else destination
        content = rewrite_links(content, source, destination)
        content = content.replace('<details>', '<details markdown="1">')
        # Convert GitHub alerts to native, accessible documentation callouts.
        def alert(match):
            kind = match.group(1).lower()
            body = re.sub(r'^> ?', '', match.group(2), flags=re.MULTILINE)
            return f'!!! {kind}\n\n' + '\n'.join('    ' + line for line in body.splitlines()) + '\n'

        content = re.sub(r'^> \[!(TIP|NOTE|IMPORTANT|WARNING|CAUTION)\]\n((?:>[^\n]*\n?)+)', alert, content, flags=re.MULTILINE)
        output = OUTPUT / destination
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(content)

    shutil.copy2(ROOT / 'backup-sets.template.json', OUTPUT)
    print(f'Prepared {len(pages)} documentation pages in {OUTPUT}')


if __name__ == '__main__':
    prepare()
