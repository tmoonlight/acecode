import { readFileSync } from "node:fs";
import { KnowledgeGraphSchema } from "@understand-anything/core";

const graph = JSON.parse(readFileSync(process.argv[2], "utf8").replace(/^\uFEFF/, ""));
const checked = KnowledgeGraphSchema.safeParse(graph);
if (!checked.success) {
  console.error(JSON.stringify(checked.error.issues.slice(0, 12), null, 2));
  process.exit(1);
}
const ids = new Set(graph.nodes.map(node => node.id));
if (ids.size !== graph.nodes.length) throw new Error("Duplicate graph node ID");
const refs = [
  ...graph.edges.flatMap(edge => [edge.source, edge.target]),
  ...graph.layers.flatMap(layer => layer.nodeIds),
  ...graph.tour.flatMap(step => step.nodeIds),
];
const dangling = refs.find(id => !ids.has(id));
if (dangling !== undefined) throw new Error("Unknown node ID: " + dangling);
