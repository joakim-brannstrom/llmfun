# Mermaid: what the TUI renderer supports

Behavioral digest for agent use, derived from
`vendor/imgui_rich_md/docs/mermaid.md` (the canonical doc; update this file
when the vendored doc changes).

The renderer supports a subset of mermaid, meant for small and medium
diagrams. It lays nodes and edges out its own way - do not expect the exact
look or layout of the canonical mermaid rendering. A diagram it cannot read is
shown as code, with the number of the line that stopped the parser.

## Diagram types

| Type | Header | Support |
|---|---|---|
| Flowchart | `flowchart` or `graph`, with `TD`, `TB`, `BT`, `LR`, `RL` | yes |
| Sequence diagram | `sequenceDiagram` | yes |
| Class diagram | `classDiagram` | yes |
| All the others (state, ER, gantt, pie, git graph, mind map, timeline, ...) | | no: shown as code |

The variant headers `flowchart-elk` and `classDiagram-v2` are read as
`flowchart` and `classDiagram`. A YAML front matter before the header is read
and skipped: its title and configuration do not apply.

## Everywhere

- `%%` comments and `%%{init: ...}%%` directives are skipped: themes and
  configuration do not apply.
- Styling and interaction lines are read and ignored: `classDef`, `class`,
  `style`, `linkStyle`, `cssClass`, `:::name`, `click`, `callback`, `link`.
- Labels are plain text: `<br>` breaks a line, and entity codes (`#quot;`,
  `#35;`) are decoded. Markdown in labels, other HTML tags and icons
  (`fa:fa-car`) are shown as written.
- Accessibility lines (`accTitle: ...`, `accDescr: ...`, `accDescr { ... }`)
  and a sequence diagram's `title` are read and skipped.

## Flowcharts

Supported:
- Node shapes: `[rect]`, `(rounded)`, `([stadium])`, `[[subroutine]]`,
  `[(database)]`, `((circle))`, `(((double circle)))`, `{diamond}`,
  `{{hexagon}}`, `[/parallelogram/]`, `[\parallelogram\]`, `[/trapezoid\]`,
  `[\trapezoid/]`, `>asymmetric]`. Quoted labels may contain brackets.
- Links: `-->`, `---`, `-.->`, `-.-`, `==>`, `===`, `~~~` (invisible), with
  `o` and `x` ends, and on both sides (`<-->`, `o--o`, `x--x`). Text inside
  (`-- text -->`) or after (`-->|text|`).
- Chains (`A --> B --> C`) and groups (`A & B --> C & D`).
- Subgraphs: `subgraph id`, `subgraph id [title]`, `subgraph some title`;
  nested; links to and from a subgraph.
- `direction` inside a subgraph, but only when no link crosses the subgraph's
  border; ignored otherwise.

Not supported:
- The new shape syntax `A@{ shape: ... }`, edge ids (`e1@-->`), images and
  icons in nodes.
- Link lengths: `--->` is read as `-->` and does not make the link span more
  layers.
- The direction shorthands `>`, `<`, `^`, `v`.
- Edges are drawn as straight segments with right angles (no curves).

Layout behavior to expect:
- A subgraph with no link to the outside and no `direction` is laid out in the
  flipped direction.
- Dense diagrams (many edges between the same layers, narrow nodes) may have
  more edge crossings, and two edges may run very close to each other.

## Sequence diagrams

Supported:
- `participant A`, `participant A as Alice`, `actor B`; participants also
  appear at their first message.
- Messages: `->`, `-->` (no head), `->>`, `-->>` (filled head), `-x`, `--x`
  (cross), `-)`, `--)` (open head), `<<->>`, `<<-->>` (both ends); a message
  to itself.
- Activations: `activate` / `deactivate`, and the `+` / `-` after an arrow;
  nested ones.
- Notes: `Note left of A`, `Note right of A`, `Note over A`,
  `Note over A,B`.
- Frames: `loop`, `alt` / `else`, `opt`, `par` / `and`, `critical` /
  `option`, `break`, nested; `rect rgb(...)` and `rect rgba(...)` highlighted
  regions.
- `autonumber`, with an optional start and step.

Not supported (the diagram is shown as code): `box` groups, `create` and
`destroy`, participant types (`participant A@{ "type": "database" }`).
`links` lines are ignored.

## Class diagrams

Supported:
- `class Name`, `class Name { members }`, `Name : member`,
  `class Name["Label"]`, generics (`List~int~`, nested).
- Members: a member with parentheses is a method, any other an attribute;
  `*` makes it abstract (italic), `$` static (underlined); visibility signs
  are shown as written.
- Annotations: `<<interface>> Name`, or `<<abstract>>` inside the class.
- Relations: inheritance (`<|--`), composition (`*--`), aggregation (`o--`),
  association (`-->`), link (`--`), dependency (`..>`), realization (`..|>`),
  dashed link (`..`), lollipop interfaces (`()--`), two-way relations; a
  label (`: text`) and cardinalities (`"1" --> "*"`).
- Namespaces, nested, and dotted names; `note for Name "text"` and floating
  notes; `direction`.

Not supported: the styling lines (`click`, `callback`, `link`, styling) are
read and ignored.
