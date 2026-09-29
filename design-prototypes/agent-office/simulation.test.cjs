// Run from any directory: node --test simulation.test.cjs
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const context = vm.createContext({});
for (const name of ['assets.js', 'simulation.js']) {
  vm.runInContext(fs.readFileSync(path.join(__dirname, name), 'utf8'), context, { filename: name });
}
const { Simulation } = context.PixelOffice;
const snapshot = sim => JSON.stringify({ time: sim.time, tasks: sim.tasks, agents: sim.agents, events: sim.events });

test('paused time freezes tasks, actor motion and the event log', () => {
  const sim = new Simulation(); sim.tick(10); sim.paused = true;
  const before = snapshot(sim); sim.tick(120); assert.equal(snapshot(sim), before);
  sim.paused = false; sim.tick(1); assert.ok(sim.time > 10);
});

test('speed scales explicit elapsed time exactly once', () => {
  const normal = new Simulation(), fast = new Simulation(); fast.speed = 4;
  normal.tick(4); fast.tick(1);
  assert.ok(Math.abs(normal.time - fast.time) < 1e-8);
  for (let i = 0; i < 6; i++) assert.ok(Math.abs(normal.tasks[i].elapsed - fast.tasks[i].elapsed) < 1e-8);
});

test('invalid time input cannot corrupt simulation state', () => {
  const sim = new Simulation(), before = snapshot(sim);
  for (const dt of [NaN, Infinity, -1, 0]) sim.tick(dt);
  assert.equal(snapshot(sim), before);
});

test('dependent work starts only after handoff, and only at the desk', () => {
  const sim = new Simulation();
  for (let frame = 0; frame < 1600; frame++) {
    const before = sim.tasks.map(t => t.elapsed); sim.tick(.1);
    for (const actor of sim.agents) {
      const t = sim.tasks[actor.index];
      if (t.elapsed > before[actor.index]) {
        assert.ok(t.deps.every(i => sim.tasks[i].elapsed === sim.tasks[i].duration));
        assert.ok(Math.hypot(actor.x - actor.home.x, actor.y - actor.home.y) < .02);
      }
    }
  }
  assert.equal(sim.doneCount, 6); assert.equal(sim.progress, 1);
});

test('coffee breaks stay inside the office, reserve one spot, and return', () => {
  const sim = new Simulation(); let sawWalk = false, sawCoffee = false, sawReturn = false;
  for (let frame = 0; frame < 2200; frame++) {
    sim.tick(.1);
    const reserved = sim.agents.filter(a => a.destination === 'coffee' && (a.route.length || a.action === 'coffee'));
    assert.ok(reserved.length <= 1);
    for (const a of sim.agents) {
      assert.ok(a.x >= 0 && a.x <= 14 && a.y >= 0 && a.y <= 11);
      sawWalk ||= a.action === 'walk'; sawCoffee ||= a.action === 'coffee';
      sawReturn ||= a.destination === 'home' && a.route.length > 0;
    }
  }
  assert.ok(sawWalk && sawCoffee && sawReturn); assert.equal(sim.doneCount, 6);
});

test('duplicate dispatch is rejected, a finished project can start fresh', () => {
  const sim = new Simulation(), before = snapshot(sim);
  assert.equal(sim.startProject(), false); assert.equal(snapshot(sim), before);
  sim.tick(240); assert.equal(sim.finished, true); assert.equal(sim.active, false);
  assert.equal(sim.startProject(), true); assert.equal(sim.projectNumber, 2);
  assert.equal(sim.progress, 0); assert.equal(sim.finished, false);
  assert.equal(sim.startProject(), false); sim.tick(240);
  assert.equal(sim.doneCount, 6); assert.equal(sim.finished, true);
});

test('a large explicit time step still resolves the entire dependency chain', () => {
  const sim = new Simulation(); sim.tick(300);
  assert.equal(sim.doneCount, 6);
  assert.ok(sim.tasks.every(t => t.elapsed === t.duration));
  assert.equal(sim.events.filter(e => e.text.includes('作品完成')).length, 1);
});
