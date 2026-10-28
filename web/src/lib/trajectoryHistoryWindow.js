export const TRAJECTORY_HISTORY_PAGE_NODES = 50;

/** Anchor to the mount-time tail: streaming appends never evict inspected rows. */
export function trajectoryHistoryWindow(nodes, requests, tailSeq, limit = TRAJECTORY_HISTORY_PAGE_NODES) {
  const latestSeq = nodes.at(-1)?.seq;
  const fixedTail = tailSeq ?? latestSeq;
  let tailIndex = -1;
  for (let index = nodes.length - 1; index >= 0; index -= 1) {
    if (nodes[index].seq <= fixedTail) { tailIndex = index; break; }
  }
  const replaced = tailIndex < 0 && latestSeq !== undefined;
  const endIndex = replaced ? nodes.length : tailIndex + 1;
  const startIndex = Math.max(0, endIndex - limit);
  const visibleNodes = startIndex === 0 ? nodes : nodes.slice(startIndex);
  const firstSeq = visibleNodes[0]?.seq ?? 0;
  return {
    tailSeq: replaced || tailSeq === undefined ? latestSeq : tailSeq,
    startIndex,
    nodes: visibleNodes,
    requests: startIndex === 0 ? requests : requests.filter(request =>
      request.startSeq >= firstSeq || (request.resultSeq ?? -Infinity) >= firstSeq),
  };
}
