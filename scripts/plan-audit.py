#!/usr/bin/env python3
"""Read-only dependency ordering and product requirement coverage over canonical TODOs."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def load_graph(root):
    spec = importlib.util.spec_from_file_location('vision_graph', ROOT / 'scripts/todo-graph.py')
    graph = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = graph
    spec.loader.exec_module(graph)
    graph.REPO, graph.TODO_DIR = root, root / 'todo'
    return graph


def reference(todo, number):
    return f'D{todo.domain[:2]} T{todo.number} §{number}'


def dependencies(graph, todos):
    by_key = {(t.domain, t.number): t for t in todos}
    by_id = {t.id: t for t in todos}
    edges, errors = {}, []
    for todo in todos:
        for num, section in todo.sections.items():
            ref = reference(todo, num)
            edges[ref] = set()
            for raw in section.depends_on:
                found = graph.resolve_ref(raw, todo, by_key)
                if not found or found[0] not in by_id or found[1] not in by_id[found[0]].sections:
                    errors.append(f'{ref}: unresolved dependency {raw}')
                else:
                    edges[ref].add(reference(by_id[found[0]], found[1]))
            for identifier in todo.depends_on:
                if identifier not in by_id:
                    errors.append(f'{ref}: unresolved file dependency {identifier}')
                else:
                    edges[ref].update(reference(by_id[identifier], n) for n in by_id[identifier].sections)
    return edges, errors


def positions(graph, text):
    found, errors, phase = {}, [], None
    for line_no, line in enumerate(text.splitlines(), 1):
        heading = graph.PHASE_HEADING_RE.match(line)
        if heading:
            phase = int(heading['id'])
        row = graph.PLAN_ROW_RE.match(line)
        if row and phase is not None:
            ref = re.sub(r'\s+', ' ', row['ref'])
            if ref in found:
                errors.append(f'{ref}: duplicate phase row')
            found[ref] = (phase, line_no)
    return found, errors


def ancestors(edges, ref):
    seen, queue = set(), list(edges.get(ref, ()))
    while queue:
        node = queue.pop()
        if node in seen:
            continue
        seen.add(node)
        queue.extend(edges.get(node, ()))
    return seen


def evaluate(edges, order, vision):
    errors = []
    for ref, deps in edges.items():
        if ref not in order:
            errors.append(f'{ref}: missing phase row')
        for dep in deps:
            if ref in order and dep in order and order[dep] >= order[ref]:
                errors.append(f'{ref}: prerequisite {dep} is not earlier in phase/row order')
    for ref in order.keys() - edges.keys():
        errors.append(f'{ref}: phase row has no section')
    gate = vision['release_gate']
    if gate not in edges:
        errors.append(f'{gate}: release gate does not exist')
    reached = ancestors(edges, gate)
    if not vision['requirements']:
        errors.append('vision has no requirement owners')
    for name, refs in vision['requirements'].items():
        if not refs:
            errors.append(f'{name}: no owner')
        for ref in refs:
            if ref not in edges:
                errors.append(f'{name}: unknown owner {ref}')
            elif ref != gate and ref not in reached:
                errors.append(f'{name}: {ref} does not block {gate}')
    return errors


def inspect(root):
    graph = load_graph(root)
    todos = graph.load_todos()
    edges, errors = dependencies(graph, todos)
    order, row_errors = positions(graph, (root / 'todo/implementation-plan.md').read_text(encoding='utf-8'))
    vision = json.loads((root / 'docs/vision-map.json').read_text(encoding='utf-8'))
    if vision.get('schema') != 1:
        raise ValueError('unsupported vision map schema')
    errors += row_errors + evaluate(edges, order, vision)
    return {'files': len(todos), 'sections': len(edges), 'requirements': len(vision['requirements']), 'errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    try:
        result = inspect(args.root.resolve())
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f'plan audit: {exc}', file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for error in result['errors']: print(error)
        print(f'plan audit: {result["files"]} files, {result["sections"]} sections, {result["requirements"]} requirements, {len(result["errors"])} error(s)')
    return bool(result['errors'])


if __name__ == '__main__':
    raise SystemExit(main())
